# poly_lag_tracker analysis

`analyze_lag.py` analyzes `poly_lag_tracker`'s output CSV (see
`../README.md` for the collection tool and its output schema) and prints a
structured report on the lag distribution, data quality, and a few other
breakdowns useful for sanity-checking a collection run before treating its
numbers as a calibration baseline.

This is analysis tooling for the CSV output, not part of the C++ collector
itself, which is why it lives here rather than in `../src/`.

## Install

```
cd polyLagTracker/analysis
pip install -r requirements.txt
```

## Run

```
python analyze_lag.py
```

With no arguments, this reads `../../results/poly_lag.csv` (i.e.
`results/poly_lag.csv` at the repo root) relative to wherever this script
lives on disk -- not a hardcoded absolute path, so it works the same from
any checkout. Point it at a different file with `--input PATH`.

```
python analyze_lag.py --input /path/to/poly_lag.csv \
  --output-json report.json \
  --output-csv-dir breakdowns/
```

Useful flags (see `--help` for the full list):

| Flag | Default | What |
|---|---|---|
| `--chunksize` | 50000 | rows read per chunk |
| `--outlier-threshold-ms` | 300000 (5 min) | `\|lag_ms\|` at/above this is flagged as an outlier |
| `--gap-threshold-seconds` | 300 (5 min) | gap between consecutive rows' receipt timestamps (sorted, not file order -- see below) to flag as possible downtime |
| `--top-n-markets` | 20 | how many markets in the per-market summary |
| `--drift-window-minutes` | 60 | window size for the lag-drift-over-time section |
| `--size-buckets` | 4 (quartiles) | number of quantile buckets for trade-size-vs-lag |
| `--price-extremity-buckets` | 4 (quartiles) | number of quantile buckets for price-extremity-vs-lag |
| `--intensity-interval-seconds` | 60 | bucket width for the trading-intensity time series |
| `--output-json PATH` | (none) | write the full report as JSON |
| `--output-csv-dir DIR` | (none) | write per-hour/per-day/per-market/etc breakdowns as separate CSVs, for plotting |
| `--verbose` | off | DEBUG-level logging (per-chunk detail) |

## Performance: how this stays fast on a file that keeps growing

The CSV is read with `pandas.read_csv(..., chunksize=...)`, never as one
big `pandas.read_csv(path)` call -- the whole point is that this file only
grows (one flushed row per trade, for a multi-day collection run) and
should keep working well past its current size, not just today's ~300MB.
Two things make this actually fast rather than just "technically chunked":

- **Column pruning.** `raw_json` -- by far the largest column in the file,
  a full JSON payload per row -- and `block_number` are dropped via
  `usecols` *before* parsing, since nothing in this report needs them.
  (`payload_timestamp`, `block_timestamp_unix`, `side`, `price`, and
  `size` *are* read -- they feed the lag-decomposition, side, size, and
  price-extremity sections below.) Dropping `raw_json` alone removes most
  of the actual parsing and memory cost per chunk, since it dwarfs every
  other column combined.
- **Narrow running accumulators, not a growing DataFrame.** Peak memory
  for the *wide* data (one row per trade, several columns) is bounded by
  `--chunksize`, since each chunk is processed and discarded. What
  survives across chunks is a handful of narrow lists/sets/counters
  (`lag_ms` values for resolved rows, per-market trade counts, tx_hash
  strings seen so far, etc.) -- these scale with row count, but at maybe
  8-100 bytes per row depending on the field, not with the width of the
  original CSV row. This is what makes exact percentiles/medians and
  duplicate-detection possible without a streaming-approximation
  algorithm, while still keeping memory well below "load the whole file."

**Measured against the real file** (304MB, 428,085 rows, on a normal
laptop): **3.8 seconds** end to end (all report sections including the six
below), chunked in ~9 pieces of 50k rows each, at a sustained ~125k
rows/sec. That's the actual number this was tested against, not a
projection. A 3GB file (10x today's size) would be expected to take well
under a minute on the same hardware, comfortably inside the "a few
minutes, not tens of minutes" target this was built to.

Progress is logged once per chunk (`--verbose` adds more detail) so a run
against a much larger future file doesn't look hung -- each line shows the
running row count, per-chunk time, and rows/sec.

## Report sections

- **Overview** -- total rows, resolved=true/false counts and percentages,
  the covered date range (`recv_wall_iso` min/max), and unique
  markets/asset_ids seen.
- **Lag distribution** -- min/max/mean/median/std and percentiles (p5,
  p25, p50, p75, p95, p99) of `lag_ms`, for `resolved=true` rows only,
  reported overall and broken out by `hour_of_day` and `day_of_week` --
  the collection tool's own README notes lag may not be flat across time,
  and this is the bucketed view to check that.
- **Lag decomposition** -- splits the single `lag_ms` figure into
  `delivery_lag_ms = recv_wall_unix_ms - payload_timestamp` (WS/network
  delivery time) and `settlement_lag_ms = payload_timestamp -
  block_timestamp_unix*1000` (the gap between Polymarket's own internal
  match timestamp and the blockchain actually confirming it) --
  `payload_timestamp` is read as a string and parsed defensively
  (`pd.to_numeric(..., errors="coerce")`), since it's a JSON-derived field
  with no format guarantee. Confirmed against the real file that the two
  sum exactly to `lag_ms`, row for row -- this is a true decomposition,
  not a separate estimate. **Real finding**: delivery lag has a median of
  **8ms** (mean 16ms, p99 110ms) -- essentially negligible -- while
  settlement lag has a median of **-2,223ms**, i.e. almost the entire
  -2.2s headline figure is the off-chain-match-vs-on-chain-confirmation
  gap, not network/infrastructure delay. That matters for how any
  correction model gets applied downstream: this isn't "our server is
  slow," it's "Polymarket's matching engine and Polygon settlement are
  ~2.2s apart," a very different thing to correct for.
- **Side vs lag** -- median/mean/p95 `lag_ms` split by `side` (BUY/SELL).
  **Real finding**: BUY median -2,210ms vs SELL median -2,194ms -- a ~16ms
  difference, i.e. no meaningful asymmetry in the data checked so far.
- **Trade size vs lag** -- `size` bucketed into `--size-buckets` quantiles
  (quartiles by default), with count and median/p95 `lag_ms` per bucket.
  **Real finding**: a small but consistent trend, smallest-quartile median
  -2,177ms to largest-quartile median -2,256ms -- larger trades show
  *slightly* more negative lag. Worth re-checking as more data accumulates
  given this is directly relevant to insider-detection use (larger trades
  are the ones that matter more), but the effect size so far is modest.
- **Price-extremity vs resolution rate / lag** -- rows bucketed by
  `|price - 0.5|` into `--price-extremity-buckets` quantiles (Q1 =
  closest to 50/50 i.e. most contested, highest Q = closest to 0/1 i.e.
  most one-sided/thin), reporting resolution rate and median `lag_ms` per
  bucket across *all* rows, not just resolved ones -- checking whether
  illiquid/near-certain markets (exactly where insider trading would be
  easiest to hide) behave differently. **Real finding**: essentially
  flat -- resolution rate 98.5-98.8% and median lag -2,206 to -2,209ms
  across all four buckets, no meaningful difference by price extremity in
  the data checked so far.
- **Lag drift over time** -- resolved rows bucketed into fixed
  `--drift-window-minutes` windows across the full collection period, with
  count and median/mean/p95 `lag_ms` per window -- a trend line, not just
  the hour-of-day/day-of-week buckets above. **Real finding**: median lag
  fluctuates roughly between -1,950ms and -2,350ms across the run with no
  strong long-term drift, though the first few hours after startup show
  more window-to-window variability than the steadier state later --
  plausibly asset-discovery/warm-up churn. Worth re-running this section
  as the collection run grows to see if that stabilizes further.
- **Trading intensity** -- trades-per-`--intensity-interval-seconds`
  (default: per-minute) across the whole collection period. The stdout
  report shows only a summary (interval count, min/mean/max, the 5
  busiest intervals); the full time series is written to CSV via
  `--output-csv-dir` for plotting. **Real finding**: 2,798 one-minute
  intervals, ranging from 2 to 1,474 trades/minute (mean 153), with the
  busiest cluster around 2026-07-18 21:30-22:50 UTC -- useful context for
  later wallet/funding-graph work, and a natural place to cross-check
  against `queue_dropped`/resolution-rate dips if those ever show up (none
  did in the run checked here).
- **Negative vs positive lag** -- how many resolved rows had `lag_ms < 0`
  (Polymarket's WS notified before the on-chain settlement tx was mined --
  the dominant pattern observed in real collection runs so far) vs `> 0`
  (WS notified after). See the collector's own README for why negative
  lag is expected here, not a bug.
- **Unresolved row diagnostics** -- for `resolved=false` rows, a count/
  percentage breakdown by the `note` column (why resolution failed), so
  it's clear whether unresolved rows are one systemic cause or a mix of
  expected edge cases (pending confirmation, shutdown mid-poll, etc).
- **Match method breakdown** -- counts by `match_method`
  (`tx_hash`/`fuzzy_log_scan`/`unmatched`) across *all* rows. `tx_hash`
  rows are the trustworthy default; treat any `fuzzy_log_scan` rows as
  lower-confidence in downstream analysis (see the collector's README on
  why fuzzy matching doesn't verify market identity).
- **Per-market summary** -- the top N markets (`--top-n-markets`) by trade
  count, with trade count, what fraction of those resolved, and
  min/mean/median/max `lag_ms` among the resolved ones -- a quick way to
  spot a market behaving very differently from the rest.
- **Data quality flags**:
  - *Outliers*: resolved rows where `|lag_ms|` is at/above
    `--outlier-threshold-ms`, listed with their market and tx_hash for
    spot-checking. The default (5 minutes) is a "this is implausible for
    this domain" bar based on observed real lag being on the order of
    single-digit seconds; lower it to see more, e.g.
    `--outlier-threshold-ms 8000` surfaces the long tail past 8s.
  - *Timeline gaps*: consecutive rows' `recv_wall_unix_ms`, **sorted by
    time rather than taken in file order** (confirmed against the real
    file: rows are NOT always written in chronological order, since
    `poly_lag_tracker`'s worker threads flush a row as soon as that
    trade's on-chain confirmation resolves, which can finish out of
    arrival order), with any gap at/above `--gap-threshold-seconds`
    flagged as possible collector downtime/disconnection.
  - *Duplicate tx_hash*: any transaction hash appearing more than once.
    Not automatically a bug -- a single on-chain transaction can contain
    multiple order fills, each producing its own WS trade event with the
    same tx_hash -- but worth a manual look if the count is high or
    unexpected.

## Output formats

- Default: a labeled, human-readable report to stdout.
- `--output-json PATH`: the same data as structured JSON (one object per
  section, matching the stdout headers).
- `--output-csv-dir DIR`: small, plot-ready CSVs for the breakdowns that
  don't fit neatly in one summary number --
  `lag_by_hour_of_day.csv`, `lag_by_day_of_week.csv`,
  `per_market_summary.csv`, `unresolved_by_note.csv`,
  `match_method_breakdown.csv`, `side_vs_lag.csv`,
  `trade_size_vs_lag.csv`, `price_extremity_vs_lag.csv`,
  `lag_drift_over_time.csv`, and `trading_intensity_timeseries.csv` (the
  full per-interval trade-count series -- this is the one CSV here with no
  stdout equivalent beyond the summary, since it can run to thousands of
  rows for a multi-day run).

## Logging

Uses Python's `logging` module (INFO by default: per-chunk progress +
timing; `--verbose` for DEBUG) rather than ad-hoc prints, and logs total
script runtime at the end.
