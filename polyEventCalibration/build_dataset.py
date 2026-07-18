"""Main CLI entrypoint for the whole pipeline, and Part 4: join the three
intermediate CSVs (markets, reactions, manual official-timestamp review)
into the final calibration dataset.

Run each stage in sequence (see README.md for the full walkthrough):

    python build_dataset.py fetch-markets   --output data/markets.csv
    python build_dataset.py fetch-reactions --markets-csv data/markets.csv --output data/reactions.csv
    python build_dataset.py fetch-official  --start-date ... --end-date ... --output data/official_candidates.csv
    python build_dataset.py review          # prints resolution criteria + candidates per market
    #   ... hand-fill data/manual_review.csv ...
    python build_dataset.py build           --output data/calibration_dataset.csv

Every subcommand here is a thin wrapper around the corresponding function in
polymarket_fetch.py / official_sources.py / matcher.py -- each of those
modules is also independently runnable with its own CLI (`python
polymarket_fetch.py fetch-markets ...`), for anyone who'd rather import
just one piece.
"""
from __future__ import annotations

import argparse
import os
from typing import Optional

import pandas as pd
from dateutil import parser as dateparser

from common import RateLimitedSession, setup_logging
from matcher import CANDIDATE_COLUMNS, print_review, sync_manual_csv_template
from official_sources import DEFAULT_SOURCES, MAX_PAGES_DEFAULT, fetch_all
from polymarket_fetch import (
    DEFAULT_TAG_SLUGS,
    fetch_markets_dataframe,
    fetch_reactions_dataframe,
)

FINAL_COLUMNS = [
    "market_id", "condition_id", "event_id", "event_slug", "title", "tags",
    "resolution_source_field", "resolution_criteria_text", "volume", "start_date", "end_date", "closed_time",
    "official_timestamp", "official_timestamp_status", "official_source_url", "official_notes",
    "gdelt_mention_count", "gdelt_earliest_mention_ts", "gdelt_top_source_domains",
    "timestamp_source",
    "market_reaction_timestamp", "reaction_method", "price_points_count", "trades_count", "reaction_notes",
    "lag_seconds",
]


def _to_unix_seconds(value) -> float | None:
    if value is None or (isinstance(value, float) and pd.isna(value)) or str(value).strip() == "":
        return None
    try:
        ts = pd.Timestamp(dateparser.parse(str(value)))
    except (ValueError, OverflowError):
        return None
    if ts.tzinfo is None:
        ts = ts.tz_localize("UTC")
    return ts.timestamp()


def build_final_dataset(
    markets_df: pd.DataFrame,
    reactions_df: pd.DataFrame,
    manual_df: pd.DataFrame,
    candidates_df: Optional[pd.DataFrame] = None,
) -> pd.DataFrame:
    # market_id is the join key across all these CSVs; force it to str
    # everywhere regardless of how each was read (pandas otherwise infers
    # int64 for a numeric-looking column when dtype=str wasn't specified by
    # the caller, and pandas refuses to merge a str key against an int64
    # one).
    markets_df = markets_df.copy()
    markets_df["market_id"] = markets_df["market_id"].astype(str)

    if reactions_df is not None and not reactions_df.empty:
        reactions_df = reactions_df.copy()
        reactions_df["market_id"] = reactions_df["market_id"].astype(str)
        df = markets_df.merge(reactions_df, on="market_id", how="left", suffixes=("", "_reaction"))
    else:
        df = markets_df.copy()

    manual = manual_df.copy() if manual_df is not None else pd.DataFrame(columns=["market_id", "official_source_url", "official_timestamp", "notes"])
    manual["market_id"] = manual["market_id"].astype(str)
    manual = manual.rename(columns={"notes": "official_notes"})
    df = df.merge(manual, on="market_id", how="left")

    # GDELT produces at most one aggregate row per market (source=="gdelt",
    # see official_sources.fetch_gdelt_events), so this is a plain 1:1
    # left-merge on market_id, same shape as the manual-review merge above.
    gdelt_cols = ["market_id", "gdelt_mention_count", "gdelt_earliest_mention_ts", "gdelt_top_source_domains"]
    if candidates_df is not None and not candidates_df.empty and "source" in candidates_df.columns:
        gdelt = candidates_df[candidates_df["source"] == "gdelt"].copy()
    else:
        gdelt = pd.DataFrame(columns=gdelt_cols)
    if not gdelt.empty:
        gdelt["market_id"] = gdelt["market_id"].astype(str)
        for col in gdelt_cols:
            if col not in gdelt.columns:
                gdelt[col] = None
        gdelt = gdelt[gdelt_cols].drop_duplicates(subset=["market_id"])
    else:
        gdelt = pd.DataFrame(columns=gdelt_cols)
    df = df.merge(gdelt, on="market_id", how="left")

    if "official_timestamp" not in df.columns:
        df["official_timestamp"] = None
    # NB: Series.astype(str) does NOT reliably turn a missing/NaN cell into
    # the literal string "nan" (pandas keeps it as float NaN under the
    # hood in some dtype backends), and NaN is truthy in plain Python --
    # `pd.notna()` is the only reliable emptiness check here.
    df["official_timestamp_status"] = df["official_timestamp"].apply(
        lambda v: "confirmed" if pd.notna(v) and str(v).strip() != "" else "pending_manual_review")

    reaction_ts_col = "market_reaction_ts" if "market_reaction_ts" in df.columns else None

    # Precision tiering: a confirmed official_timestamp (whitehouse.gov/
    # state.gov, hand-picked into manual_review.csv) is always the primary
    # T-zero when available. Only when there is NO confirmed official
    # timestamp does this fall back to GDELT's earliest-mention timestamp
    # as a lower-precision estimate -- so downstream analysis can filter/
    # weight by timestamp_source instead of treating every row as equally
    # trustworthy. lag_seconds is computed against whichever T-zero was
    # actually used, so gdelt_estimate rows still carry a usable (if
    # noisier) lag rather than being left blank.
    timestamp_sources = []
    lag_values = []
    for _, row in df.iterrows():
        reaction_raw = row.get(reaction_ts_col) if reaction_ts_col else None
        has_reaction = reaction_raw is not None and not (isinstance(reaction_raw, float) and pd.isna(reaction_raw))

        t_zero_unix = None
        source = "none"
        if row["official_timestamp_status"] == "confirmed":
            t_zero_unix = _to_unix_seconds(row["official_timestamp"])
            if t_zero_unix is not None:
                source = "official"
        if t_zero_unix is None:
            gdelt_ts = row.get("gdelt_earliest_mention_ts")
            if pd.notna(gdelt_ts) and str(gdelt_ts).strip():
                t_zero_unix = _to_unix_seconds(gdelt_ts)
                if t_zero_unix is not None:
                    source = "gdelt_estimate"

        timestamp_sources.append(source)
        if t_zero_unix is not None and has_reaction:
            lag_values.append(float(reaction_raw) - t_zero_unix)
        else:
            lag_values.append(None)

    df["timestamp_source"] = timestamp_sources
    df["lag_seconds"] = lag_values

    if reaction_ts_col:
        df = df.rename(columns={reaction_ts_col: "market_reaction_timestamp"})
    else:
        df["market_reaction_timestamp"] = None

    for col in FINAL_COLUMNS:
        if col not in df.columns:
            df[col] = None

    return df[FINAL_COLUMNS]


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p1 = sub.add_parser("fetch-markets", help="Part 1: pull resolved candidate markets")
    p1.add_argument("--tags", default=",".join(DEFAULT_TAG_SLUGS))
    p1.add_argument("--end-date-min", default=None)
    p1.add_argument("--end-date-max", default=None)
    p1.add_argument("--max-events-per-tag", type=int, default=None)
    p1.add_argument("--output", default="data/markets.csv")
    p1.add_argument("--log-file", default="data/pipeline.log")

    p2 = sub.add_parser("fetch-reactions", help="Part 2: pull price/trade history, detect reaction timestamps")
    p2.add_argument("--markets-csv", default="data/markets.csv")
    p2.add_argument("--output", default="data/reactions.csv")
    p2.add_argument("--price-move-threshold", type=float, default=0.10)
    p2.add_argument("--price-window-minutes", type=int, default=60)
    p2.add_argument("--volume-percentile", type=float, default=95.0)
    p2.add_argument("--fidelity-minutes", type=int, default=5)
    p2.add_argument("--max-trades", type=int, default=20000)
    p2.add_argument("--log-file", default="data/pipeline.log")

    p3 = sub.add_parser("fetch-official", help="Part 3: scrape official releases + query GDELT corroboration")
    p3.add_argument("--sources", default=DEFAULT_SOURCES,
                    help=f"default: {DEFAULT_SOURCES!r} -- 'defense' is available but not included "
                         f"by default (disallowed by its own robots.txt, see official_sources.py)")
    p3.add_argument("--markets-csv", default="data/markets.csv",
                    help="required for the gdelt source (per-market queries); also used to derive "
                         "--start-date/--end-date automatically if they're not given")
    p3.add_argument("--start-date", default=None, help="YYYY-MM-DD, overrides markets-csv-derived range")
    p3.add_argument("--end-date", default=None, help="YYYY-MM-DD, overrides markets-csv-derived range")
    p3.add_argument("--window-days", type=int, default=3,
                    help="padding applied to the markets-csv-derived date range for whitehouse/state; "
                         "for gdelt, the actual per-market query window (default 3)")
    p3.add_argument("--max-pages", type=int, default=MAX_PAGES_DEFAULT)
    p3.add_argument("--output", default="data/official_candidates.csv")
    p3.add_argument("--log-file", default="data/pipeline.log")

    p4 = sub.add_parser("review", help="Part 3: print resolution criteria + candidates per market for manual review")
    p4.add_argument("--markets-csv", default="data/markets.csv")
    p4.add_argument("--candidates-csv", default="data/official_candidates.csv")
    p4.add_argument("--manual-csv", default="data/manual_review.csv")
    p4.add_argument("--window-days", type=int, default=3)
    p4.add_argument("--market-id", default=None)
    p4.add_argument("--log-file", default="data/pipeline.log")

    p5 = sub.add_parser("build", help="Part 4: join everything into the final calibration dataset")
    p5.add_argument("--markets-csv", default="data/markets.csv")
    p5.add_argument("--reactions-csv", default="data/reactions.csv")
    p5.add_argument("--manual-csv", default="data/manual_review.csv")
    p5.add_argument("--candidates-csv", default="data/official_candidates.csv",
                    help="source of the gdelt_estimate fallback timestamp (its gdelt rows)")
    p5.add_argument("--output", default="data/calibration_dataset.csv")
    p5.add_argument("--log-file", default="data/pipeline.log")

    args = parser.parse_args()
    logger = setup_logging(args.log_file)

    if args.command == "fetch-markets":
        session = RateLimitedSession(logger, min_delay_seconds=0.3)
        tag_slugs = [t.strip() for t in args.tags.split(",") if t.strip()]
        df = fetch_markets_dataframe(session, tag_slugs, args.end_date_min, args.end_date_max,
                                      args.max_events_per_tag)
        os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
        df.to_csv(args.output, index=False)
        logger.info("wrote %d market row(s) to %s", len(df), args.output)

    elif args.command == "fetch-reactions":
        session = RateLimitedSession(logger, min_delay_seconds=0.3)
        markets_df = pd.read_csv(args.markets_csv)
        df = fetch_reactions_dataframe(
            session, markets_df, args.price_move_threshold, args.price_window_minutes,
            args.volume_percentile, args.fidelity_minutes, args.max_trades)
        os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
        df.to_csv(args.output, index=False)
        logger.info("wrote %d reaction row(s) to %s", len(df), args.output)

    elif args.command == "fetch-official":
        session = RateLimitedSession(logger, min_delay_seconds=1.0)
        sources = [s.strip() for s in args.sources.split(",") if s.strip()]

        # markets_df is needed both to derive a date range (if not given
        # explicitly) and, separately, for gdelt's own per-market queries --
        # load it whenever either is true, not just the first.
        markets_df = None
        if "gdelt" in sources or not (args.start_date and args.end_date):
            if os.path.exists(args.markets_csv):
                markets_df = pd.read_csv(args.markets_csv, dtype=str)
            elif "gdelt" in sources:
                logger.error("gdelt requires --markets-csv (%s not found)", args.markets_csv)

        start_date, end_date = args.start_date, args.end_date
        if not start_date or not end_date:
            if markets_df is None or markets_df.empty:
                logger.error("could not derive a date range from %s and none was given explicitly", args.markets_csv)
                return
            end_dates = pd.to_datetime(markets_df["end_date"], utc=True, errors="coerce").dropna()
            if end_dates.empty:
                logger.error("could not derive a date range from %s and none was given explicitly", args.markets_csv)
                return
            pad = pd.Timedelta(days=args.window_days)
            start_date = start_date or (end_dates.min() - pad).date().isoformat()
            end_date = end_date or (end_dates.max() + pad).date().isoformat()
            logger.info("derived official-source fetch range from markets CSV: %s to %s", start_date, end_date)

        results = fetch_all(session, sources, dateparser.parse(start_date).date(),
                             dateparser.parse(end_date).date(), markets_df=markets_df,
                             window_days=args.window_days, max_pages=args.max_pages)
        all_records = [r for result in results for r in result.records]
        df = pd.DataFrame(all_records)
        os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
        df.to_csv(args.output, index=False)
        logger.info("wrote %d total candidate release(s) to %s", len(df), args.output)
        for result in results:
            if result.status != "ok":
                logger.warning("source %s finished with status=%s (%s) -- %d candidate(s) found",
                                result.source, result.status, result.detail, len(result.records))

    elif args.command == "review":
        markets_df = pd.read_csv(args.markets_csv, dtype=str)
        candidates_df = pd.read_csv(args.candidates_csv, dtype=str) if os.path.exists(args.candidates_csv) \
            else pd.DataFrame(columns=CANDIDATE_COLUMNS)
        manual_df = sync_manual_csv_template(markets_df, args.manual_csv)
        logger.info("manual-review CSV %s has %d row(s) (%d already filled in)",
                    args.manual_csv, len(manual_df),
                    int((manual_df["official_timestamp"].astype(str).str.strip() != "").sum()))
        print_review(markets_df, candidates_df, args.window_days, only_market_id=args.market_id)

    elif args.command == "build":
        markets_df = pd.read_csv(args.markets_csv, dtype=str)
        reactions_df = pd.read_csv(args.reactions_csv) if os.path.exists(args.reactions_csv) else pd.DataFrame()
        manual_df = pd.read_csv(args.manual_csv, dtype=str) if os.path.exists(args.manual_csv) else pd.DataFrame()
        candidates_df = pd.read_csv(args.candidates_csv, dtype=str) if os.path.exists(args.candidates_csv) else pd.DataFrame()

        final_df = build_final_dataset(markets_df, reactions_df, manual_df, candidates_df)
        os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
        final_df.to_csv(args.output, index=False)

        counts = final_df["timestamp_source"].value_counts()
        logger.info("wrote %d row(s) to %s -- timestamp_source breakdown: official=%d, "
                    "gdelt_estimate=%d, none=%d",
                    len(final_df), args.output,
                    int(counts.get("official", 0)), int(counts.get("gdelt_estimate", 0)),
                    int(counts.get("none", 0)))


if __name__ == "__main__":
    main()
