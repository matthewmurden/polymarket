#!/usr/bin/env python3
"""Analyzes poly_lag_tracker's output CSV (see ../README.md for the schema)
without ever loading the whole file into memory at once.

The file only grows (poly_lag_tracker appends one flushed row per trade,
for hours/days at a time) and is already ~300MB / ~430k rows at the time
this was written, so a plain `pandas.read_csv(path)` is the wrong default
here -- it works today but is exactly the kind of thing that quietly stops
working a week from now. Instead:

  - The CSV is read in chunks (`pandas.read_csv(..., chunksize=...)`), and
    `raw_json` / `payload_timestamp` / `block_number` / `block_timestamp_unix`
    are dropped via `usecols` before parsing even happens -- raw_json in
    particular is by far the largest column in the file and nothing here
    needs it, so this alone removes most of the parsing/memory cost.
  - Peak memory therefore scales with chunksize (one bounded-size DataFrame
    at a time), not with file size. What DOES scale with file size is a set
    of small, purely numeric/string accumulators kept across chunks (lag_ms
    values, timestamps, tx_hash strings, per-market counts) -- these are
    needed for exact percentiles/medians/gap-detection/duplicate-detection,
    which aren't incrementally computable without them. For a file this
    size that's a few tens of MB at most; see the README for the actual
    numbers measured against the real file.

Run `python analyze_lag.py --help` for options; see the accompanying
README.md for what each report section means and how to interpret it.
"""
from __future__ import annotations

import argparse
import csv
import json
import logging
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd

LOG = logging.getLogger("analyze_lag")

# Only these are ever read off disk -- see module docstring. Still excludes
# raw_json (by far the largest column) and block_number, neither of which
# any report section needs. payload_timestamp/block_timestamp_unix/side/
# price/size were added for the lag-decomposition, side, size, and
# price-extremity sections -- each one earns its place in this list by
# being read by at least one section below.
USECOLS = [
    "recv_wall_iso", "recv_wall_unix_ms", "day_of_week", "hour_of_day",
    "market", "asset_id", "side", "price", "size", "payload_timestamp",
    "tx_hash", "match_method", "resolved", "block_timestamp_unix",
    "lag_ms", "note",
]

# `resolved` is written as literal lowercase "true"/"false" text by the C++
# tool. pandas' C parser WILL auto-infer that as a bool column, but only
# consistently if every chunk happens to contain both values -- confirmed
# against the real file that resolved=False rows are rare (~1% in the
# sample checked) and chunk boundaries can land in a run of all-True rows,
# which risks a different inferred dtype for that one chunk. Reading it as
# plain str and comparing against the literal text sidesteps that
# per-chunk inference risk entirely. payload_timestamp gets the same
# str-then-pd.to_numeric(errors="coerce") treatment for the same reason
# (it's a JSON-derived string field, not guaranteed numeric-looking in
# every row) -- confirmed against the real file it's a plain millisecond
# unix timestamp as text (e.g. "1784372104655").
DTYPE = {
    "recv_wall_iso": str,
    "day_of_week": str,
    "hour_of_day": "Int64",
    "market": str,
    "asset_id": str,
    "side": str,
    "payload_timestamp": str,
    "tx_hash": str,
    "match_method": str,
    "resolved": str,
    "note": str,
}

DEFAULT_CHUNKSIZE = 50_000
DEFAULT_OUTLIER_THRESHOLD_MS = 300_000.0  # 5 minutes -- see README for why
DEFAULT_GAP_THRESHOLD_SECONDS = 300.0     # 5 minutes
DEFAULT_TOP_N_MARKETS = 20
DEFAULT_DRIFT_WINDOW_MINUTES = 60.0
DEFAULT_SIZE_BUCKETS = 4
DEFAULT_PRICE_EXTREMITY_BUCKETS = 4
DEFAULT_INTENSITY_INTERVAL_SECONDS = 60.0

PERCENTILES = [5, 25, 50, 75, 95, 99]


def default_input_path() -> Path:
    """../../results/poly_lag.csv relative to this script -- i.e. the repo
    root's results/ dir, regardless of the caller's cwd. Not hardcoded as
    an absolute path so this stays portable across checkouts/machines.
    """
    return (Path(__file__).resolve().parent / ".." / ".." / "results" / "poly_lag.csv").resolve()


def setup_logging(verbose: bool) -> None:
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        stream=sys.stdout,
    )


# --------------------------------------------------------------------------
# Accumulator: the running state carried across chunks. See module
# docstring for the memory-scaling rationale.
# --------------------------------------------------------------------------

class Accumulator:
    def __init__(self):
        self.total_rows = 0
        self.resolved_true = 0
        self.resolved_false = 0

        self.min_recv_wall_iso: Optional[str] = None
        self.max_recv_wall_iso: Optional[str] = None

        self.unique_markets: set[str] = set()
        self.unique_assets: set[str] = set()

        # lag_ms values for resolved=true rows only, overall and bucketed.
        self.lag_all: list[float] = []
        self.lag_by_hour: dict[int, list[float]] = defaultdict(list)
        self.lag_by_day: dict[str, list[float]] = defaultdict(list)
        self.lag_by_market: dict[str, list[float]] = defaultdict(list)

        # trade counts (ALL rows, resolved or not) per market, to find the
        # top-N by volume regardless of resolution status.
        self.trade_count_by_market: Counter = Counter()
        self.resolved_count_by_market: Counter = Counter()

        # resolved=false diagnostics
        self.note_counts: Counter = Counter()

        # across all rows regardless of resolved status
        self.match_method_counts: Counter = Counter()

        # data-quality checks
        self.tx_hash_seen: set[str] = set()
        self.tx_hash_duplicates: Counter = Counter()
        self.recv_wall_unix_ms_all: list[int] = []

        # for the outlier report: keep (lag_ms, market, tx_hash) tuples for
        # rows beyond the threshold, so the report can name names, not just
        # a count. Bounded by construction (only extreme rows are kept).
        self.outlier_rows: list[tuple[float, str, str]] = []

        # --- lag decomposition (resolved rows with a parseable
        # payload_timestamp AND block_timestamp_unix only) ---
        # delivery = recv_wall_unix_ms - payload_timestamp (network/WS delivery)
        # settlement = payload_timestamp - block_timestamp_unix*1000 (off-chain match vs on-chain confirm)
        # delivery + settlement == lag_ms, confirmed against the real file.
        self.delivery_lag_all: list[float] = []
        self.settlement_lag_all: list[float] = []

        # --- side vs lag (resolved rows) ---
        self.lag_by_side: dict[str, list[float]] = defaultdict(list)

        # --- trade size vs lag (resolved rows with a parseable size) ---
        # kept as parallel (size, lag) pairs rather than pre-bucketed,
        # since bucket edges (quantiles) aren't known until the whole
        # column has been seen -- same reasoning as lag_by_market etc,
        # scales with resolved row count, not file size.
        self.size_lag_pairs: list[tuple[float, float]] = []

        # --- lag drift over time (resolved rows) ---
        # keyed by the row's recv_wall_unix_ms floored to a fixed window
        # (window size decided by the caller of process_chunk), value is
        # that window's start in unix ms.
        self.drift_buckets: dict[int, list[float]] = defaultdict(list)

        # --- price-extremity correlation (ALL rows with a parseable price) ---
        # (distance-from-0.5, resolved boolean, lag_ms-or-None) per row --
        # needs ALL rows, not just resolved ones, to compute a resolution
        # *rate* per bucket, not just lag within already-resolved rows.
        self.price_extremity_rows: list[tuple[float, bool, Optional[float]]] = []

        # --- trading intensity time series (ALL rows) ---
        # keyed by recv_wall_unix_ms floored to a fixed interval (decided
        # by the caller), value is trade count in that interval. A plain
        # Counter here is intentionally the *only* one of the six new
        # accumulators that doesn't scale with total row count in the
        # list-of-floats sense -- it's one integer per interval bucket
        # (e.g. ~2800 entries for a 2-day run at 1-minute resolution).
        self.trades_per_interval: Counter = Counter()


def process_chunk(
    chunk: pd.DataFrame,
    acc: Accumulator,
    outlier_threshold_ms: float,
    drift_window_ms: float,
    intensity_interval_ms: float,
) -> None:
    acc.total_rows += len(chunk)

    resolved_mask = chunk["resolved"] == "true"
    n_resolved = int(resolved_mask.sum())
    acc.resolved_true += n_resolved
    acc.resolved_false += len(chunk) - n_resolved

    non_null_iso = chunk["recv_wall_iso"].dropna()
    if not non_null_iso.empty:
        chunk_min, chunk_max = non_null_iso.min(), non_null_iso.max()
        if acc.min_recv_wall_iso is None or chunk_min < acc.min_recv_wall_iso:
            acc.min_recv_wall_iso = chunk_min
        if acc.max_recv_wall_iso is None or chunk_max > acc.max_recv_wall_iso:
            acc.max_recv_wall_iso = chunk_max

    acc.unique_markets.update(chunk["market"].dropna().unique())
    acc.unique_assets.update(chunk["asset_id"].dropna().unique())

    acc.trade_count_by_market.update(chunk["market"].dropna())
    acc.resolved_count_by_market.update(chunk.loc[resolved_mask, "market"].dropna())

    acc.match_method_counts.update(chunk["match_method"].fillna("(blank)"))

    unresolved = chunk.loc[~resolved_mask]
    notes = unresolved["note"].fillna("(no note recorded)")
    notes = notes.where(notes.str.strip() != "", "(no note recorded)")
    acc.note_counts.update(notes)

    resolved_chunk = chunk.loc[resolved_mask]
    lag_values = pd.to_numeric(resolved_chunk["lag_ms"], errors="coerce").dropna()
    if not lag_values.empty:
        idx = lag_values.index
        lag_list = lag_values.tolist()
        acc.lag_all.extend(lag_list)

        markets_for_lag = resolved_chunk.loc[idx, "market"]
        hours_for_lag = resolved_chunk.loc[idx, "hour_of_day"]
        days_for_lag = resolved_chunk.loc[idx, "day_of_week"]
        tx_for_lag = resolved_chunk.loc[idx, "tx_hash"]
        sides_for_lag = resolved_chunk.loc[idx, "side"]
        sizes_for_lag = pd.to_numeric(resolved_chunk.loc[idx, "size"], errors="coerce")
        payload_ts_for_lag = pd.to_numeric(resolved_chunk.loc[idx, "payload_timestamp"], errors="coerce")
        block_ts_for_lag = pd.to_numeric(resolved_chunk.loc[idx, "block_timestamp_unix"], errors="coerce")
        recv_ms_for_lag = pd.to_numeric(resolved_chunk.loc[idx, "recv_wall_unix_ms"], errors="coerce")

        for lag, market, hour, day, tx, side, size, payload_ts, block_ts, recv_ms in zip(
            lag_list, markets_for_lag, hours_for_lag, days_for_lag, tx_for_lag,
            sides_for_lag, sizes_for_lag, payload_ts_for_lag, block_ts_for_lag, recv_ms_for_lag,
        ):
            if pd.notna(hour):
                acc.lag_by_hour[int(hour)].append(lag)
            if pd.notna(day):
                acc.lag_by_day[str(day)].append(lag)
            if pd.notna(market):
                acc.lag_by_market[str(market)].append(lag)
            if abs(lag) >= outlier_threshold_ms:
                acc.outlier_rows.append((lag, str(market), str(tx)))

            if pd.notna(side):
                acc.lag_by_side[str(side)].append(lag)
            if pd.notna(size):
                acc.size_lag_pairs.append((float(size), lag))
            if pd.notna(payload_ts) and pd.notna(block_ts):
                acc.settlement_lag_all.append(float(payload_ts) - float(block_ts) * 1000.0)
                if pd.notna(recv_ms):
                    acc.delivery_lag_all.append(float(recv_ms) - float(payload_ts))
            if pd.notna(recv_ms):
                window_start = (int(recv_ms) // int(drift_window_ms)) * int(drift_window_ms)
                acc.drift_buckets[window_start].append(lag)

    # Price-extremity needs ALL rows (resolved or not) to compute a
    # resolution *rate* per bucket, not just lag among already-resolved
    # ones -- so this is a separate pass over the whole chunk, not the
    # resolved-only slice above.
    price_num = pd.to_numeric(chunk["price"], errors="coerce")
    valid_price = price_num.notna()
    if valid_price.any():
        distances = (price_num[valid_price] - 0.5).abs()
        resolved_sub = resolved_mask[valid_price]
        lag_sub = pd.to_numeric(chunk.loc[valid_price, "lag_ms"], errors="coerce")
        for dist, is_resolved, lag in zip(distances, resolved_sub, lag_sub):
            acc.price_extremity_rows.append(
                (float(dist), bool(is_resolved), float(lag) if pd.notna(lag) else None)
            )

    # Trading intensity: every row counts (WS message volume, not
    # resolution outcome), and this is vectorizable (no per-row Python
    # loop needed) since a Counter.update over the bucketed values is
    # enough.
    recv_ms_all_num = pd.to_numeric(chunk["recv_wall_unix_ms"], errors="coerce").dropna()
    intensity_buckets = (recv_ms_all_num // intensity_interval_ms * intensity_interval_ms).astype("int64")
    acc.trades_per_interval.update(intensity_buckets.tolist())

    for tx in chunk["tx_hash"].dropna():
        if tx == "":
            continue
        if tx in acc.tx_hash_seen:
            acc.tx_hash_duplicates[tx] += 1
        else:
            acc.tx_hash_seen.add(tx)

    ts = pd.to_numeric(chunk["recv_wall_unix_ms"], errors="coerce").dropna()
    acc.recv_wall_unix_ms_all.extend(int(v) for v in ts.tolist())


def load_and_process(
    path: Path,
    chunksize: int,
    outlier_threshold_ms: float,
    drift_window_ms: float,
    intensity_interval_ms: float,
) -> Accumulator:
    acc = Accumulator()
    start = time.monotonic()
    LOG.info("reading %s in chunks of %d rows...", path, chunksize)

    chunk_iter = pd.read_csv(
        path,
        usecols=USECOLS,
        dtype=DTYPE,
        chunksize=chunksize,
        engine="c",
    )

    for i, chunk in enumerate(chunk_iter, start=1):
        chunk_start = time.monotonic()
        process_chunk(chunk, acc, outlier_threshold_ms, drift_window_ms, intensity_interval_ms)
        chunk_elapsed = time.monotonic() - chunk_start
        total_elapsed = time.monotonic() - start
        LOG.info(
            "chunk %d: +%d rows (total %d) in %.2fs (%.0f rows/s overall)",
            i, len(chunk), acc.total_rows, chunk_elapsed,
            acc.total_rows / total_elapsed if total_elapsed > 0 else 0.0,
        )

    LOG.info("finished reading %d row(s) in %.1fs", acc.total_rows, time.monotonic() - start)
    return acc


# --------------------------------------------------------------------------
# Report sections
# --------------------------------------------------------------------------

def _percentile_stats(values: list[float]) -> dict:
    if not values:
        return {"count": 0}
    arr = np.asarray(values, dtype=np.float64)
    pct = np.percentile(arr, PERCENTILES)
    return {
        "count": int(arr.size),
        "min": float(arr.min()),
        "max": float(arr.max()),
        "mean": float(arr.mean()),
        "median": float(np.median(arr)),
        "std": float(arr.std(ddof=1)) if arr.size > 1 else 0.0,
        **{f"p{p}": float(v) for p, v in zip(PERCENTILES, pct)},
    }


def overview_stats(acc: Accumulator) -> dict:
    total = acc.total_rows
    return {
        "total_rows": total,
        "resolved_true": acc.resolved_true,
        "resolved_false": acc.resolved_false,
        "resolved_true_pct": (100.0 * acc.resolved_true / total) if total else 0.0,
        "resolved_false_pct": (100.0 * acc.resolved_false / total) if total else 0.0,
        "min_recv_wall_iso": acc.min_recv_wall_iso,
        "max_recv_wall_iso": acc.max_recv_wall_iso,
        "unique_markets": len(acc.unique_markets),
        "unique_asset_ids": len(acc.unique_assets),
    }


def lag_distribution_stats(acc: Accumulator) -> dict:
    by_hour = {str(h): _percentile_stats(v) for h, v in sorted(acc.lag_by_hour.items())}
    by_day = {d: _percentile_stats(v) for d, v in acc.lag_by_day.items()}
    return {
        "overall": _percentile_stats(acc.lag_all),
        "by_hour_of_day": by_hour,
        "by_day_of_week": by_day,
    }


def sign_breakdown_stats(acc: Accumulator) -> dict:
    arr = np.asarray(acc.lag_all, dtype=np.float64)
    total = arr.size
    negative = int((arr < 0).sum())
    positive = int((arr > 0).sum())
    zero = total - negative - positive
    return {
        "total_resolved_with_lag": total,
        "negative_count": negative,
        "negative_pct": (100.0 * negative / total) if total else 0.0,
        "positive_count": positive,
        "positive_pct": (100.0 * positive / total) if total else 0.0,
        "zero_count": zero,
        "zero_pct": (100.0 * zero / total) if total else 0.0,
    }


def unresolved_diagnostics(acc: Accumulator) -> dict:
    total_unresolved = acc.resolved_false
    breakdown = {
        note: {"count": count, "pct": (100.0 * count / total_unresolved) if total_unresolved else 0.0}
        for note, count in acc.note_counts.most_common()
    }
    return {"total_unresolved": total_unresolved, "by_note": breakdown}


def match_method_stats(acc: Accumulator) -> dict:
    total = acc.total_rows
    return {
        method: {"count": count, "pct": (100.0 * count / total) if total else 0.0}
        for method, count in acc.match_method_counts.most_common()
    }


def per_market_summary(acc: Accumulator, top_n: int) -> list[dict]:
    top_markets = acc.trade_count_by_market.most_common(top_n)
    rows = []
    for market, trade_count in top_markets:
        lag_values = acc.lag_by_market.get(market, [])
        resolved_count = acc.resolved_count_by_market.get(market, 0)
        stats = _percentile_stats(lag_values)
        rows.append({
            "market": market,
            "trade_count": trade_count,
            "resolved_count": resolved_count,
            "resolved_pct": (100.0 * resolved_count / trade_count) if trade_count else 0.0,
            "median_lag_ms": stats.get("median"),
            "mean_lag_ms": stats.get("mean"),
            "min_lag_ms": stats.get("min"),
            "max_lag_ms": stats.get("max"),
        })
    return rows


def lag_decomposition_stats(acc: Accumulator) -> dict:
    """delivery_lag_ms (recv_wall_unix_ms - payload_timestamp, i.e. WS/
    network delivery time) and settlement_lag_ms (payload_timestamp -
    block_timestamp_unix*1000, i.e. off-chain-match-vs-on-chain-confirm
    gap) -- confirmed against the real file that these two sum exactly to
    lag_ms, so this is a true decomposition, not a separate estimate.
    """
    return {
        "delivery_lag_ms": _percentile_stats(acc.delivery_lag_all),
        "settlement_lag_ms": _percentile_stats(acc.settlement_lag_all),
    }


def _quantile_bucket_edges(values: np.ndarray, n_buckets: int) -> np.ndarray:
    edges = np.percentile(values, np.linspace(0, 100, n_buckets + 1))
    # Guard against degenerate (all-equal or heavily-duplicated) inputs
    # producing non-increasing edges, which would break np.digitize.
    for i in range(1, len(edges)):
        if edges[i] <= edges[i - 1]:
            edges[i] = np.nextafter(edges[i - 1], np.inf)
    return edges


def trade_size_vs_lag(acc: Accumulator, n_buckets: int) -> list[dict]:
    if not acc.size_lag_pairs:
        return []
    sizes = np.array([s for s, _ in acc.size_lag_pairs], dtype=np.float64)
    lags = np.array([l for _, l in acc.size_lag_pairs], dtype=np.float64)
    edges = _quantile_bucket_edges(sizes, n_buckets)
    bucket_idx = np.clip(np.digitize(sizes, edges[1:-1], right=True), 0, n_buckets - 1)

    rows = []
    for b in range(n_buckets):
        mask = bucket_idx == b
        if not mask.any():
            continue
        stats = _percentile_stats(lags[mask].tolist())
        rows.append({
            "size_bucket": f"Q{b + 1}",
            "size_min": float(sizes[mask].min()),
            "size_max": float(sizes[mask].max()),
            "count": int(mask.sum()),
            "median_lag_ms": stats.get("median"),
            "mean_lag_ms": stats.get("mean"),
            "p95_lag_ms": stats.get("p95"),
        })
    return rows


def side_vs_lag(acc: Accumulator) -> dict:
    return {side: _percentile_stats(v) for side, v in sorted(acc.lag_by_side.items())}


def lag_drift_over_time(acc: Accumulator) -> list[dict]:
    rows = []
    for window_start_ms in sorted(acc.drift_buckets):
        stats = _percentile_stats(acc.drift_buckets[window_start_ms])
        rows.append({
            "window_start_iso": pd.Timestamp(window_start_ms, unit="ms", tz="UTC").isoformat(),
            "window_start_unix_ms": window_start_ms,
            "count": stats.get("count"),
            "median_lag_ms": stats.get("median"),
            "mean_lag_ms": stats.get("mean"),
            "p95_lag_ms": stats.get("p95"),
        })
    return rows


def price_extremity_stats(acc: Accumulator, n_buckets: int) -> list[dict]:
    """Buckets rows by |price - 0.5| (0 = perfectly contested 50/50, 0.5 =
    fully resolved to 0 or 1) into quantile buckets, and reports the
    resolution rate and resolved-row lag stats per bucket. Near-certain/
    thin markets (high distance-from-0.5) are exactly the ones worth
    checking for worse resolution/lag behavior, per the project's stated
    interest in illiquid markets as a place insider trading could hide.
    """
    if not acc.price_extremity_rows:
        return []
    distances = np.array([d for d, _, _ in acc.price_extremity_rows], dtype=np.float64)
    resolved_flags = np.array([r for _, r, _ in acc.price_extremity_rows], dtype=bool)
    lags = [l for _, _, l in acc.price_extremity_rows]

    edges = _quantile_bucket_edges(distances, n_buckets)
    bucket_idx = np.clip(np.digitize(distances, edges[1:-1], right=True), 0, n_buckets - 1)

    rows = []
    for b in range(n_buckets):
        mask = bucket_idx == b
        if not mask.any():
            continue
        total = int(mask.sum())
        resolved_count = int(resolved_flags[mask].sum())
        bucket_lags = [lags[i] for i in np.where(mask)[0] if lags[i] is not None]
        stats = _percentile_stats(bucket_lags)
        d_sub = distances[mask]
        rows.append({
            "extremity_bucket": f"Q{b + 1}",
            "distance_from_half_min": float(d_sub.min()),
            "distance_from_half_max": float(d_sub.max()),
            "total_count": total,
            "resolved_count": resolved_count,
            "resolved_pct": (100.0 * resolved_count / total) if total else 0.0,
            "median_lag_ms": stats.get("median"),
            "mean_lag_ms": stats.get("mean"),
        })
    return rows


def trading_intensity_series(acc: Accumulator) -> list[dict]:
    rows = []
    for window_start_ms in sorted(acc.trades_per_interval):
        rows.append({
            "window_start_iso": pd.Timestamp(window_start_ms, unit="ms", tz="UTC").isoformat(),
            "window_start_unix_ms": window_start_ms,
            "trade_count": acc.trades_per_interval[window_start_ms],
        })
    return rows


def trading_intensity_summary(series_rows: list[dict]) -> dict:
    if not series_rows:
        return {}
    counts = np.array([r["trade_count"] for r in series_rows], dtype=np.int64)
    busiest = sorted(series_rows, key=lambda r: -r["trade_count"])[:5]
    return {
        "interval_count": len(series_rows),
        "min_trades_per_interval": int(counts.min()),
        "max_trades_per_interval": int(counts.max()),
        "mean_trades_per_interval": float(counts.mean()),
        "busiest_intervals": busiest,
    }


def data_quality_flags(acc: Accumulator, outlier_threshold_ms: float, gap_threshold_seconds: float) -> dict:
    # Outliers: already collected during chunk processing, bounded to only
    # the rows that actually crossed the threshold.
    outliers_sorted = sorted(acc.outlier_rows, key=lambda t: -abs(t[0]))
    top_outliers = [
        {"lag_ms": lag, "market": market, "tx_hash": tx}
        for lag, market, tx in outliers_sorted[:20]
    ]

    # Timeline gaps: recv_wall_unix_ms is NOT guaranteed to be in file
    # order (confirmed against the real file -- worker threads write rows
    # as their RPC resolution finishes, not in arrival order), so this
    # sorts the full accumulated timestamp list before diffing. Memory
    # cost is one int64 per row, see module docstring.
    gaps = []
    if len(acc.recv_wall_unix_ms_all) > 1:
        ts_sorted = np.sort(np.asarray(acc.recv_wall_unix_ms_all, dtype=np.int64))
        diffs_ms = np.diff(ts_sorted)
        gap_threshold_ms = gap_threshold_seconds * 1000.0
        gap_idx = np.where(diffs_ms >= gap_threshold_ms)[0]
        for idx in gap_idx:
            gaps.append({
                "gap_seconds": float(diffs_ms[idx]) / 1000.0,
                "start_unix_ms": int(ts_sorted[idx]),
                "end_unix_ms": int(ts_sorted[idx + 1]),
                "start_iso": pd.Timestamp(int(ts_sorted[idx]), unit="ms", tz="UTC").isoformat(),
                "end_iso": pd.Timestamp(int(ts_sorted[idx + 1]), unit="ms", tz="UTC").isoformat(),
            })
        gaps.sort(key=lambda g: -g["gap_seconds"])

    duplicate_tx_hashes = [
        {"tx_hash": tx, "extra_occurrences": count}
        for tx, count in acc.tx_hash_duplicates.most_common(20)
    ]

    return {
        "outlier_threshold_ms": outlier_threshold_ms,
        "outlier_count": len(acc.outlier_rows),
        "top_outliers": top_outliers,
        "gap_threshold_seconds": gap_threshold_seconds,
        "gap_count": len(gaps),
        "gaps": gaps[:20],
        "duplicate_tx_hash_count": len(acc.tx_hash_duplicates),
        "top_duplicate_tx_hashes": duplicate_tx_hashes,
    }


# --------------------------------------------------------------------------
# Report rendering
# --------------------------------------------------------------------------

def _fmt(v, digits=1) -> str:
    if v is None:
        return "n/a"
    if isinstance(v, float):
        return f"{v:,.{digits}f}"
    return f"{v:,}"


def print_report(results: dict) -> None:
    w = 88
    print("\n" + "=" * w)
    print(" POLY_LAG_TRACKER OUTPUT ANALYSIS")
    print("=" * w)

    ov = results["overview"]
    print("\n--- Overview " + "-" * (w - 13))
    print(f"total rows:        {_fmt(ov['total_rows'], 0)}")
    print(f"resolved=true:     {_fmt(ov['resolved_true'], 0)} ({_fmt(ov['resolved_true_pct'])}%)")
    print(f"resolved=false:    {_fmt(ov['resolved_false'], 0)} ({_fmt(ov['resolved_false_pct'])}%)")
    print(f"date range:        {ov['min_recv_wall_iso']}  to  {ov['max_recv_wall_iso']}")
    print(f"unique markets:    {_fmt(ov['unique_markets'], 0)}")
    print(f"unique asset_ids:  {_fmt(ov['unique_asset_ids'], 0)}")

    lag = results["lag_distribution"]
    print("\n--- Lag distribution (resolved=true only, lag_ms) " + "-" * (w - 51))
    o = lag["overall"]
    if o.get("count"):
        print(f"n={_fmt(o['count'],0)}  min={_fmt(o['min'])}  max={_fmt(o['max'])}  "
              f"mean={_fmt(o['mean'])}  median={_fmt(o['median'])}  std={_fmt(o['std'])}")
        print("percentiles (ms): " + "  ".join(f"p{p}={_fmt(o[f'p{p}'])}" for p in PERCENTILES))
    else:
        print("no resolved rows with a usable lag_ms value")

    print("\nby hour_of_day (UTC):")
    print(f"  {'hour':>4}  {'n':>8}  {'median':>10}  {'mean':>10}  {'p95':>10}")
    for hour in sorted(lag["by_hour_of_day"], key=lambda h: int(h)):
        s = lag["by_hour_of_day"][hour]
        if not s.get("count"):
            continue
        print(f"  {hour:>4}  {s['count']:>8,}  {_fmt(s['median']):>10}  {_fmt(s['mean']):>10}  {_fmt(s['p95']):>10}")

    print("\nby day_of_week:")
    day_order = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
    print(f"  {'day':>4}  {'n':>8}  {'median':>10}  {'mean':>10}  {'p95':>10}")
    for day in sorted(lag["by_day_of_week"], key=lambda d: day_order.index(d) if d in day_order else 99):
        s = lag["by_day_of_week"][day]
        if not s.get("count"):
            continue
        print(f"  {day:>4}  {s['count']:>8,}  {_fmt(s['median']):>10}  {_fmt(s['mean']):>10}  {_fmt(s['p95']):>10}")

    ld = results["lag_decomposition"]
    print("\n--- Lag decomposition (resolved=true, delivery + settlement = lag_ms) " + "-" * (w - 72))
    for label, key in [("delivery (WS/network)", "delivery_lag_ms"), ("settlement (off-chain match vs on-chain confirm)", "settlement_lag_ms")]:
        s = ld[key]
        if s.get("count"):
            print(f"{label}:")
            print(f"  n={_fmt(s['count'],0)}  min={_fmt(s['min'])}  max={_fmt(s['max'])}  "
                  f"mean={_fmt(s['mean'])}  median={_fmt(s['median'])}  std={_fmt(s['std'])}")
            print("  percentiles (ms): " + "  ".join(f"p{p}={_fmt(s[f'p{p}'])}" for p in PERCENTILES))
        else:
            print(f"{label}: no usable rows")

    sv = results["side_vs_lag"]
    print("\n--- Side vs lag (resolved=true) " + "-" * (w - 33))
    print(f"  {'side':>6}  {'n':>8}  {'median':>10}  {'mean':>10}  {'p95':>10}")
    for side, s in sv.items():
        if not s.get("count"):
            continue
        print(f"  {side:>6}  {s['count']:>8,}  {_fmt(s['median']):>10}  {_fmt(s['mean']):>10}  {_fmt(s['p95']):>10}")

    tsl = results["trade_size_vs_lag"]
    print(f"\n--- Trade size vs lag (resolved=true, {len(tsl)} quantile buckets) " + "-" * max(0, w - 46 - len(str(len(tsl)))))
    print(f"  {'bucket':>7}  {'size range':>26}  {'n':>8}  {'median_lag':>11}  {'p95_lag':>10}")
    for row in tsl:
        size_range = f"{row['size_min']:,.2f} - {row['size_max']:,.2f}"
        print(f"  {row['size_bucket']:>7}  {size_range:>26}  {row['count']:>8,}  "
              f"{_fmt(row['median_lag_ms']):>11}  {_fmt(row['p95_lag_ms']):>10}")

    pe = results["price_extremity"]
    print(f"\n--- Price-extremity vs resolution rate / lag (all rows, {len(pe)} quantile buckets) " + "-" * max(0, w - 58 - len(str(len(pe)))))
    print("  (Q1 = closest to 50/50 i.e. most contested, highest Q = closest to 0/1 i.e. most one-sided/thin)")
    print(f"  {'bucket':>7}  {'dist-from-0.5 range':>22}  {'n':>8}  {'resolved%':>9}  {'median_lag':>11}")
    for row in pe:
        d_range = f"{row['distance_from_half_min']:.3f} - {row['distance_from_half_max']:.3f}"
        print(f"  {row['extremity_bucket']:>7}  {d_range:>22}  {row['total_count']:>8,}  "
              f"{_fmt(row['resolved_pct']):>9}  {_fmt(row['median_lag_ms']):>11}")

    dr = results["lag_drift"]
    print(f"\n--- Lag drift over time ({len(dr)} window(s)) " + "-" * max(0, w - 30 - len(str(len(dr)))))
    print(f"  {'window start (UTC)':<22}  {'n':>8}  {'median_lag':>11}  {'mean_lag':>10}  {'p95_lag':>10}")
    for row in dr:
        window_label = row["window_start_iso"][:19]
        print(f"  {window_label:<22}  {row['count']:>8,}  {_fmt(row['median_lag_ms']):>11}  "
              f"{_fmt(row['mean_lag_ms']):>10}  {_fmt(row['p95_lag_ms']):>10}")

    ti = results["trading_intensity_summary"]
    print("\n--- Trading intensity " + "-" * (w - 23))
    if ti:
        print(f"intervals: {_fmt(ti['interval_count'], 0)}   "
              f"min/mean/max trades per interval: {_fmt(ti['min_trades_per_interval'],0)} / "
              f"{_fmt(ti['mean_trades_per_interval'])} / {_fmt(ti['max_trades_per_interval'],0)}")
        print("busiest intervals:")
        for row in ti["busiest_intervals"]:
            print(f"  {row['window_start_iso'][:19]}  {row['trade_count']:>8,} trades")
        print("(full time series written to CSV via --output-csv-dir; not printed here)")
    else:
        print("no data")

    sb = results["sign_breakdown"]
    print("\n--- Negative vs positive lag " + "-" * (w - 30))
    print(f"negative (WS before on-chain confirm): {_fmt(sb['negative_count'],0)} ({_fmt(sb['negative_pct'])}%)")
    print(f"positive (WS after on-chain confirm):  {_fmt(sb['positive_count'],0)} ({_fmt(sb['positive_pct'])}%)")
    print(f"exactly zero:                          {_fmt(sb['zero_count'],0)} ({_fmt(sb['zero_pct'])}%)")

    ur = results["unresolved_diagnostics"]
    print("\n--- Unresolved row diagnostics (resolved=false) " + "-" * (w - 49))
    print(f"total unresolved: {_fmt(ur['total_unresolved'], 0)}")
    for note, s in ur["by_note"].items():
        print(f"  {s['count']:>8,}  ({_fmt(s['pct'])}%)  {note}")

    mm = results["match_method"]
    print("\n--- Match method breakdown (all rows) " + "-" * (w - 39))
    for method, s in mm.items():
        print(f"  {s['count']:>8,}  ({_fmt(s['pct'])}%)  {method}")

    pm = results["per_market"]
    print(f"\n--- Per-market summary (top {len(pm)} by trade count) " + "-" * max(0, w - 40 - len(str(len(pm)))))
    print(f"  {'market':<66}  {'trades':>8}  {'resolved%':>9}  {'median_lag':>11}")
    for row in pm:
        market_short = row["market"][:64] + ".." if len(row["market"]) > 66 else row["market"]
        print(f"  {market_short:<66}  {row['trade_count']:>8,}  {_fmt(row['resolved_pct']):>9}  "
              f"{_fmt(row['median_lag_ms']):>11}")

    dq = results["data_quality"]
    print("\n--- Data quality flags " + "-" * (w - 24))
    print(f"outlier threshold: |lag_ms| >= {_fmt(dq['outlier_threshold_ms'],0)}")
    print(f"outliers found:    {_fmt(dq['outlier_count'], 0)}")
    for o in dq["top_outliers"][:10]:
        print(f"  lag_ms={_fmt(o['lag_ms'],0):>12}  market={o['market']}  tx_hash={o['tx_hash']}")

    print(f"\ngap threshold:     >= {_fmt(dq['gap_threshold_seconds'],0)}s between consecutive rows (sorted by time)")
    print(f"gaps found:        {_fmt(dq['gap_count'], 0)}")
    for g in dq["gaps"][:10]:
        print(f"  {_fmt(g['gap_seconds'],1):>10}s   {g['start_iso']}  ->  {g['end_iso']}")

    print(f"\nduplicate tx_hash values: {_fmt(dq['duplicate_tx_hash_count'], 0)} distinct hash(es) appeared more than once")
    print("  (not necessarily a bug -- one on-chain tx can contain multiple fills; spot-check if unexpected)")
    for d in dq["top_duplicate_tx_hashes"][:10]:
        print(f"  {d['tx_hash']}  (+{d['extra_occurrences']} extra occurrence(s))")

    print("\n" + "=" * w + "\n")


def write_json(results: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w") as f:
        json.dump(results, f, indent=2, default=str)
    LOG.info("wrote JSON report to %s", path)


def write_csvs(results: dict, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)

    lag = results["lag_distribution"]

    hour_rows = []
    for hour, s in lag["by_hour_of_day"].items():
        row = {"hour_of_day": hour, **s}
        hour_rows.append(row)
    _write_csv_rows(out_dir / "lag_by_hour_of_day.csv", hour_rows)

    day_rows = []
    for day, s in lag["by_day_of_week"].items():
        row = {"day_of_week": day, **s}
        day_rows.append(row)
    _write_csv_rows(out_dir / "lag_by_day_of_week.csv", day_rows)

    _write_csv_rows(out_dir / "per_market_summary.csv", results["per_market"])

    note_rows = [{"note": note, **s} for note, s in results["unresolved_diagnostics"]["by_note"].items()]
    _write_csv_rows(out_dir / "unresolved_by_note.csv", note_rows)

    match_rows = [{"match_method": m, **s} for m, s in results["match_method"].items()]
    _write_csv_rows(out_dir / "match_method_breakdown.csv", match_rows)

    side_rows = [{"side": side, **s} for side, s in results["side_vs_lag"].items()]
    _write_csv_rows(out_dir / "side_vs_lag.csv", side_rows)

    _write_csv_rows(out_dir / "trade_size_vs_lag.csv", results["trade_size_vs_lag"])
    _write_csv_rows(out_dir / "price_extremity_vs_lag.csv", results["price_extremity"])
    _write_csv_rows(out_dir / "lag_drift_over_time.csv", results["lag_drift"])
    _write_csv_rows(out_dir / "trading_intensity_timeseries.csv", results["trading_intensity_series"])

    LOG.info("wrote breakdown CSVs to %s", out_dir)


def _write_csv_rows(path: Path, rows: list[dict]) -> None:
    if not rows:
        LOG.debug("skipping %s, no rows", path)
        return
    fieldnames = list(rows[0].keys())
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, default=None,
                        help="path to poly_lag.csv (default: ../../results/poly_lag.csv relative to this script)")
    parser.add_argument("--chunksize", type=int, default=DEFAULT_CHUNKSIZE,
                        help=f"rows per chunk while reading (default {DEFAULT_CHUNKSIZE})")
    parser.add_argument("--outlier-threshold-ms", type=float, default=DEFAULT_OUTLIER_THRESHOLD_MS,
                        help=f"|lag_ms| at/above this is flagged as an outlier (default {DEFAULT_OUTLIER_THRESHOLD_MS:.0f})")
    parser.add_argument("--gap-threshold-seconds", type=float, default=DEFAULT_GAP_THRESHOLD_SECONDS,
                        help=f"gap between consecutive (sorted) rows' recv_wall timestamps to flag, seconds (default {DEFAULT_GAP_THRESHOLD_SECONDS:.0f})")
    parser.add_argument("--top-n-markets", type=int, default=DEFAULT_TOP_N_MARKETS,
                        help=f"how many markets to include in the per-market summary (default {DEFAULT_TOP_N_MARKETS})")
    parser.add_argument("--drift-window-minutes", type=float, default=DEFAULT_DRIFT_WINDOW_MINUTES,
                        help=f"window size for the lag-drift-over-time section, minutes (default {DEFAULT_DRIFT_WINDOW_MINUTES:.0f})")
    parser.add_argument("--size-buckets", type=int, default=DEFAULT_SIZE_BUCKETS,
                        help=f"number of quantile buckets for trade-size-vs-lag (default {DEFAULT_SIZE_BUCKETS})")
    parser.add_argument("--price-extremity-buckets", type=int, default=DEFAULT_PRICE_EXTREMITY_BUCKETS,
                        help=f"number of quantile buckets for price-extremity-vs-lag (default {DEFAULT_PRICE_EXTREMITY_BUCKETS})")
    parser.add_argument("--intensity-interval-seconds", type=float, default=DEFAULT_INTENSITY_INTERVAL_SECONDS,
                        help=f"bucket width for the trading-intensity time series, seconds (default {DEFAULT_INTENSITY_INTERVAL_SECONDS:.0f})")
    parser.add_argument("--output-json", type=Path, default=None, help="write the full report as JSON to this path")
    parser.add_argument("--output-csv-dir", type=Path, default=None,
                        help="write per-hour/per-day/per-market/etc breakdown CSVs into this directory")
    parser.add_argument("--verbose", action="store_true", help="DEBUG-level logging")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    setup_logging(args.verbose)

    input_path = args.input or default_input_path()
    if not input_path.exists():
        LOG.error("input file not found: %s", input_path)
        return 1

    drift_window_ms = args.drift_window_minutes * 60_000.0
    intensity_interval_ms = args.intensity_interval_seconds * 1000.0

    script_start = time.monotonic()
    acc = load_and_process(
        input_path, args.chunksize, args.outlier_threshold_ms, drift_window_ms, intensity_interval_ms
    )

    LOG.info("computing report statistics...")
    intensity_series = trading_intensity_series(acc)
    results = {
        "input_path": str(input_path),
        "overview": overview_stats(acc),
        "lag_distribution": lag_distribution_stats(acc),
        "lag_decomposition": lag_decomposition_stats(acc),
        "side_vs_lag": side_vs_lag(acc),
        "trade_size_vs_lag": trade_size_vs_lag(acc, args.size_buckets),
        "price_extremity": price_extremity_stats(acc, args.price_extremity_buckets),
        "lag_drift": lag_drift_over_time(acc),
        "trading_intensity_series": intensity_series,
        "trading_intensity_summary": trading_intensity_summary(intensity_series),
        "sign_breakdown": sign_breakdown_stats(acc),
        "unresolved_diagnostics": unresolved_diagnostics(acc),
        "match_method": match_method_stats(acc),
        "per_market": per_market_summary(acc, args.top_n_markets),
        "data_quality": data_quality_flags(acc, args.outlier_threshold_ms, args.gap_threshold_seconds),
    }

    print_report(results)

    if args.output_json:
        write_json(results, args.output_json)
    if args.output_csv_dir:
        write_csvs(results, args.output_csv_dir)

    LOG.info("total runtime: %.1fs", time.monotonic() - script_start)
    return 0


if __name__ == "__main__":
    sys.exit(main())
