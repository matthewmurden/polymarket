"""Part 3 (continued): semi-automated candidate matching.

This module deliberately does NOT attempt to automatically decide which
scraped official release resolved a given market -- fuzzy-matching release
text against resolution criteria is unreliable (e.g. a DoD release
confirming a strike and a market asking "will the US strike X by Friday"
share almost no vocabulary in common) and would silently produce wrong
official_timestamp values feeding directly into the calibration dataset.
Instead: for each market, filter the scraped candidates down to the
market's own resolution-date window and print both the market's resolution
criteria text and the filtered candidate list side by side, so a human
picks the right one (or none) and records it in a plain CSV.
"""
from __future__ import annotations

import argparse
import os
import re
import textwrap
from collections import Counter
from typing import Optional

import pandas as pd
from dateutil import parser as dateparser

from common import setup_logging

MANUAL_CSV_COLUMNS = ["market_id", "official_source_url", "official_timestamp", "notes"]

CANDIDATE_COLUMNS = [
    "source", "market_id", "title", "published_timestamp", "url",
    "gdelt_mention_count", "gdelt_earliest_mention_ts", "gdelt_top_source_domains",
]


def market_window(end_date_str: Optional[str], window_days: int):
    """Returns (start_date, end_date) as plain dates, window_days before/
    after end_date_str -- shared by print_review's date-window filtering
    (below) and official_sources.fetch_gdelt_events, which needs the same
    per-market window to scope its GDELT query."""
    if not end_date_str or pd.isna(end_date_str):
        return None, None
    end = dateparser.parse(str(end_date_str))
    delta = pd.Timedelta(days=window_days)
    end_ts = pd.Timestamp(end)
    if end_ts.tzinfo is not None:
        end_ts = end_ts.tz_localize(None)
    return (end_ts - delta).date(), (end_ts + delta).date()


# Simple, dependency-free keyword extraction for building a GDELT search
# query from a market's title/resolution criteria -- no NLP library, per
# project constraints. Not sophisticated: capitalized words in the title
# are treated as likely proper nouns (countries, people, orgs) and given
# priority, since those are the most useful terms for a news search; the
# remaining slots are filled with the most frequent significant words
# across the title + a slice of the resolution criteria. Good enough to
# narrow a GDELT query to the right topic, not a claim of precision.
_STOPWORDS = {
    "a", "an", "the", "and", "or", "but", "if", "on", "in", "to", "of", "by",
    "at", "is", "are", "was", "were", "be", "been", "being", "will", "would",
    "shall", "should", "can", "could", "this", "that", "these", "those", "it",
    "its", "as", "for", "with", "from", "not", "no", "yes", "otherwise",
    "market", "markets", "resolve", "resolves", "resolved", "resolution",
    "outcome", "outcomes", "listed", "date", "before", "after", "any",
    "point", "than", "into", "over", "under", "also", "such", "qualify",
    "count", "otherwise", "credible", "reporting", "consensus", "source",
    "has", "have", "had", "having", "does", "did", "do",
    "january", "february", "march", "april", "may", "june", "july",
    "august", "september", "october", "november", "december",
}

_WORD_RE = re.compile(r"[A-Za-z][A-Za-z'-]*")

# Confirmed live: GDELT's DOC API rejects a query containing any keyword
# shorter than 3 characters outright (HTTP 200, plain-text body "Your
# search contained a keyword that was too short" -- not JSON, not an error
# status; see official_sources.fetch_gdelt_events, which detects this).
# Two-letter country/org acronyms are common and useful in exactly this
# market's domain (US/UK/EU/UN), so rather than just dropping them, expand
# the handful worth expanding into their multi-word form (each word of
# which clears the 3-char minimum) instead of losing the signal entirely.
_SHORT_ACRONYM_EXPANSIONS = {
    "us": ["United", "States"],
    "uk": ["United", "Kingdom"],
    "eu": ["European", "Union"],
    "un": ["United", "Nations"],
}
GDELT_MIN_KEYWORD_LENGTH = 3


def extract_search_terms(title: Optional[str], resolution_criteria_text: Optional[str], max_terms: int = 6) -> list[str]:
    title = title or ""
    criteria = resolution_criteria_text or ""

    title_words = _WORD_RE.findall(title)
    proper_nouns = []
    seen = set()
    for w in title_words:
        if w[0].isupper() and w.lower() not in _STOPWORDS and len(w) > 1:
            key = w.lower()
            if key in seen:
                continue
            seen.add(key)
            if len(w) < GDELT_MIN_KEYWORD_LENGTH:
                expansion = _SHORT_ACRONYM_EXPANSIONS.get(key)
                if expansion:
                    proper_nouns.extend(expansion)
                # else: too short and no known expansion -- drop it rather
                # than send a query GDELT will reject outright.
                continue
            proper_nouns.append(w)

    body = f"{title} {criteria[:500]}"  # cap criteria length -- it can run to several paragraphs
    all_words = [w.lower() for w in _WORD_RE.findall(body)]
    freq = Counter(w for w in all_words if w not in _STOPWORDS and len(w) >= GDELT_MIN_KEYWORD_LENGTH)
    for w in proper_nouns:
        freq.pop(w.lower(), None)  # already included; don't also fill a frequency slot with it

    remaining_slots = max(0, max_terms - len(proper_nouns))
    remaining = [w for w, _ in freq.most_common(remaining_slots)]

    return (proper_nouns + remaining)[:max_terms]


def candidates_for_market(market_row: dict, candidates_df: pd.DataFrame, window_days: int) -> pd.DataFrame:
    """Filters the full candidates table down to the ones relevant to this
    market. Two different mechanisms, depending on how the candidate was
    found:
      - GDELT rows carry their own market_id (they were fetched with a
        query built FROM this specific market's title/criteria, in a date
        window already scoped to this market) -- these are included
        whenever market_id matches, full stop.
      - whitehouse.gov/state.gov rows are scraped once across the whole
        requested date range with no market association, so they're
        included by the generic date-window check instead (+/- window_days
        around this market's resolution date). Without this split, a
        market-specific GDELT hit for one market could spuriously appear
        under a *different* market that merely resolved on a nearby date --
        this keeps the two candidate types from bleeding into each other.
    """
    if candidates_df is None or candidates_df.empty:
        return pd.DataFrame(columns=CANDIDATE_COLUMNS)

    df = candidates_df.copy()
    if "market_id" not in df.columns:
        df["market_id"] = ""
    df["market_id"] = df["market_id"].fillna("").astype(str)
    market_id = str(market_row.get("market_id"))

    own_market_rows = df[df["market_id"] == market_id]

    generic_rows = df[df["market_id"] == ""]
    end_date_str = market_row.get("end_date") or market_row.get("closed_time")
    start_window, end_window = market_window(end_date_str, window_days)
    if start_window is not None and not generic_rows.empty:
        generic_rows = generic_rows.copy()
        generic_rows["_published_date"] = pd.to_datetime(
            generic_rows["published_timestamp"], utc=True, errors="coerce").dt.date
        mask = (generic_rows["_published_date"] >= start_window) & (generic_rows["_published_date"] <= end_window)
        generic_rows = generic_rows[mask].drop(columns=["_published_date"])
    else:
        generic_rows = generic_rows.iloc[0:0]

    combined = pd.concat([own_market_rows, generic_rows], ignore_index=True)
    for col in CANDIDATE_COLUMNS:
        if col not in combined.columns:
            combined[col] = ""
    return combined[CANDIDATE_COLUMNS].sort_values("published_timestamp")


def load_or_init_manual_csv(path: str) -> pd.DataFrame:
    if os.path.exists(path):
        df = pd.read_csv(path, dtype=str)
        for col in MANUAL_CSV_COLUMNS:
            if col not in df.columns:
                df[col] = ""
        return df[MANUAL_CSV_COLUMNS]
    return pd.DataFrame(columns=MANUAL_CSV_COLUMNS)


def sync_manual_csv_template(markets_df: pd.DataFrame, path: str) -> pd.DataFrame:
    """Ensures every market_id in markets_df has a row in the manual-review
    CSV (creating the file if needed), WITHOUT touching rows a human has
    already filled in -- safe to re-run after fetching more markets.
    """
    existing = load_or_init_manual_csv(path)
    existing_ids = set(existing["market_id"].astype(str))

    new_rows = []
    for market_id in markets_df["market_id"].astype(str):
        if market_id not in existing_ids:
            new_rows.append({"market_id": market_id, "official_source_url": "",
                              "official_timestamp": "", "notes": ""})

    if new_rows:
        existing = pd.concat([existing, pd.DataFrame(new_rows)], ignore_index=True)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    existing.to_csv(path, index=False)
    return existing


def print_review(
    markets_df: pd.DataFrame,
    candidates_df: pd.DataFrame,
    window_days: int,
    only_market_id: Optional[str] = None,
) -> None:
    rows = markets_df.to_dict("records")
    if only_market_id:
        rows = [r for r in rows if str(r.get("market_id")) == str(only_market_id)]
        if not rows:
            print(f"no market with market_id={only_market_id!r} found in markets CSV")
            return

    for row in rows:
        print("=" * 100)
        print(f"market_id: {row.get('market_id')}   condition_id: {row.get('condition_id')}")
        print(f"title:     {row.get('title')}")
        print(f"tags:      {row.get('tags')}")
        print(f"end_date:  {row.get('end_date')}   closed_time: {row.get('closed_time')}")
        print("-" * 100)
        print("resolution criteria text:")
        raw_criteria = row.get("resolution_criteria_text")
        criteria = str(raw_criteria).strip() if pd.notna(raw_criteria) and str(raw_criteria).strip() else "(none captured)"
        print(textwrap.fill(criteria, width=96, initial_indent="  ", subsequent_indent="  "))
        resolution_source_field = row.get("resolution_source_field")
        if pd.notna(resolution_source_field) and str(resolution_source_field).strip():
            print(f"  (resolutionSource field: {resolution_source_field})")
        print("-" * 100)

        matched = candidates_for_market(row, candidates_df, window_days)
        official_rows = matched[matched["source"] != "gdelt"]
        gdelt_rows = matched[matched["source"] == "gdelt"]

        if official_rows.empty:
            print(f"no scraped official-source (whitehouse.gov/state.gov) candidates found within "
                  f"+/-{window_days} days of resolution -- fill in data/manual_review.csv by hand "
                  f"for this market, or widen --window-days and re-run fetch-official.")
        else:
            print(f"{len(official_rows)} candidate OFFICIAL release(s) (high precision) within "
                  f"+/-{window_days} days:")
            for i, cand in enumerate(official_rows.to_dict("records"), start=1):
                print(f"  [{i}] {cand['published_timestamp']}  ({cand['source']})")
                print(f"      {cand['title']}")
                print(f"      {cand['url']}")

        if not gdelt_rows.empty:
            print()
            print(f"GDELT corroboration (LOWER PRECISION -- a mention-volume proxy, not a "
                  f"dated official release; use only if no official candidate above fits):")
            for cand in gdelt_rows.to_dict("records"):
                print(f"      earliest mention: {cand['gdelt_earliest_mention_ts']}   "
                      f"mentions: {cand['gdelt_mention_count']}")
                print(f"      top domains: {cand['gdelt_top_source_domains']}")
                print(f"      {cand['url']}")
        print()
        print("  -> to record your pick, edit the manual-review CSV row for this market_id:")
        print("     market_id,official_source_url,official_timestamp,notes")
        print()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--markets-csv", default="data/markets.csv")
    parser.add_argument("--candidates-csv", default="data/official_candidates.csv")
    parser.add_argument("--manual-csv", default="data/manual_review.csv")
    parser.add_argument("--window-days", type=int, default=3,
                        help="how many days before/after a market's resolution date to search "
                             "official-source candidates (default 3)")
    parser.add_argument("--market-id", default=None, help="only review one market")
    parser.add_argument("--log-file", default="data/pipeline.log")
    args = parser.parse_args()

    logger = setup_logging(args.log_file)

    markets_df = pd.read_csv(args.markets_csv, dtype=str)
    candidates_df = pd.read_csv(args.candidates_csv, dtype=str) if os.path.exists(args.candidates_csv) \
        else pd.DataFrame(columns=CANDIDATE_COLUMNS)

    manual_df = sync_manual_csv_template(markets_df, args.manual_csv)
    logger.info("manual-review CSV %s has %d row(s) (%d already filled in)",
                args.manual_csv, len(manual_df),
                int((manual_df["official_timestamp"].astype(str).str.strip() != "").sum()))

    print_review(markets_df, candidates_df, args.window_days, only_market_id=args.market_id)


if __name__ == "__main__":
    main()
