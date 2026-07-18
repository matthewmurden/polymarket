"""Part 1: pull resolved military/geopolitical Polymarket markets via the
Gamma API's /events endpoint.
Part 2: pull each market's historical price/trade data and detect a
"market reaction timestamp" candidate.

Field names and endpoint behavior below were confirmed against the live
Gamma/CLOB/Data APIs while building this tool, not assumed:

- Category filtering: Polymarket does NOT expose one clean "category"
  taxonomy field with values like "military"/"geopolitical" -- /markets'
  top-level "category" field is a coarse bucket (e.g. "US-current-affairs")
  that doesn't distinguish military/geopolitical topics at all. The real,
  usable taxonomy is the /tags list (thousands of free-form tags) attached
  to each *event* (GET /events?tag_slug=<slug> filters correctly). Verified
  relevant tag slugs: geopolitics, military, military-invasion,
  military-strikes, war, international-relations, department-of-defense,
  nato. See DEFAULT_TAG_SLUGS below.
- "resolutionSource" is a real field on both events and markets, but in
  practice it's very often an empty string -- the actual resolution
  criteria text (which usually names the source in prose, e.g. "the CIA
  World Factbook page for Russia" or "official NATO website") lives in the
  free-text "description" field instead. This tool captures both and
  treats "description" as resolution_criteria_text.
- Historical prices: GET https://clob.polymarket.com/prices-history
  requires explicit startTs/endTs (unix seconds) -- interval=max with no
  explicit range silently returns an empty history, so this always derives
  a range from the market's own start/end.
- Historical trades: GET https://data-api.polymarket.com/trades?market=
  <conditionId> filters correctly; the same endpoint's "asset=<token id>"
  parameter does NOT filter (confirmed by testing -- it silently ignores
  the filter and returns unrelated trades), so this always uses
  market=<conditionId>, never asset=. Results are paginated via limit/
  offset, newest-first.
"""
from __future__ import annotations

import argparse
import json
import statistics
from dataclasses import dataclass, field
from typing import Optional

import pandas as pd

from common import RateLimitedSession, setup_logging

GAMMA_BASE = "https://gamma-api.polymarket.com"
CLOB_BASE = "https://clob.polymarket.com"
DATA_API_BASE = "https://data-api.polymarket.com"

# Confirmed live via GET /events?tag_slug=<slug>&closed=true -- see module
# docstring. Not an exhaustive taxonomy; pass --tags to override/extend.
DEFAULT_TAG_SLUGS = [
    "geopolitics",
    "military",
    "military-invasion",
    "military-strikes",
    "war",
    "international-relations",
    "department-of-defense",
    "nato",
]

EVENTS_PAGE_SIZE = 100  # confirmed: /events ignores limit values above 100
TRADES_PAGE_SIZE = 500


# --------------------------------------------------------------------------
# Part 1: candidate market list
# --------------------------------------------------------------------------

def fetch_events(
    session: RateLimitedSession,
    tag_slugs: list[str],
    closed: bool = True,
    end_date_min: Optional[str] = None,
    end_date_max: Optional[str] = None,
    max_events_per_tag: Optional[int] = None,
) -> list[dict]:
    """Pages through GET /events for each tag slug and returns the union of
    events (deduped by event id -- the same event can carry several of our
    target tags). end_date_min/max, if given, must be ISO-8601 strings and
    are passed straight through to the API's own filter.
    """
    logger = session.logger
    events_by_id: dict[str, dict] = {}

    for tag_slug in tag_slugs:
        offset = 0
        fetched_for_tag = 0
        while True:
            params = {
                "tag_slug": tag_slug,
                "closed": str(closed).lower(),
                "limit": EVENTS_PAGE_SIZE,
                "offset": offset,
            }
            if end_date_min:
                params["end_date_min"] = end_date_min
            if end_date_max:
                params["end_date_max"] = end_date_max
            query = "&".join(f"{k}={v}" for k, v in params.items())
            url = f"{GAMMA_BASE}/events?{query}"

            resp = session.get(url, row_count_fn=lambda r: len(r.json()))
            if resp is None or resp.status_code != 200:
                logger.error("fetch_events: giving up on tag_slug=%s at offset=%d (status=%s)",
                             tag_slug, offset, getattr(resp, "status_code", None))
                break

            page = resp.json()
            if not page:
                break

            for ev in page:
                events_by_id[ev["id"]] = ev
            fetched_for_tag += len(page)

            if len(page) < EVENTS_PAGE_SIZE:
                break
            offset += EVENTS_PAGE_SIZE
            if max_events_per_tag and fetched_for_tag >= max_events_per_tag:
                break

        logger.info("tag_slug=%s: %d event(s) fetched", tag_slug, fetched_for_tag)

    logger.info("fetch_events: %d unique event(s) across %d tag(s)", len(events_by_id), len(tag_slugs))
    return list(events_by_id.values())


def _safe_json_list(raw: Optional[str]) -> list:
    if not raw:
        return []
    try:
        return json.loads(raw)
    except (json.JSONDecodeError, TypeError):
        return []


def events_to_market_rows(events: list[dict]) -> list[dict]:
    """Flattens events -> one row per MARKET (an event can bundle several
    markets, e.g. multi-outcome events), carrying the parent event's tags/
    resolution text onto every market row so each row is self-contained.
    """
    rows = []
    for ev in events:
        tags = [t.get("label") for t in (ev.get("tags") or []) if t.get("label")]
        event_resolution_source = ev.get("resolutionSource") or ""
        event_description = ev.get("description") or ""

        for mkt in ev.get("markets") or []:
            clob_token_ids = _safe_json_list(mkt.get("clobTokenIds"))
            outcomes = _safe_json_list(mkt.get("outcomes"))
            outcome_prices = _safe_json_list(mkt.get("outcomePrices"))

            rows.append({
                "market_id": mkt.get("id"),
                "condition_id": mkt.get("conditionId"),
                "event_id": ev.get("id"),
                "event_slug": ev.get("slug"),
                "title": mkt.get("question") or ev.get("title"),
                "tags": "|".join(tags),
                "resolution_source_field": mkt.get("resolutionSource") or event_resolution_source,
                "resolution_criteria_text": mkt.get("description") or event_description,
                "outcomes": "|".join(str(o) for o in outcomes),
                "outcome_prices": "|".join(str(p) for p in outcome_prices),
                "clob_token_ids": "|".join(str(t) for t in clob_token_ids),
                "volume": mkt.get("volumeNum") or mkt.get("volume"),
                "start_date": mkt.get("startDate") or ev.get("startDate"),
                "end_date": mkt.get("endDate") or ev.get("endDate"),
                "closed_time": mkt.get("closedTime") or ev.get("closedTime"),
                "uma_resolution_status": mkt.get("umaResolutionStatus"),
            })
    return rows


def fetch_markets_dataframe(
    session: RateLimitedSession,
    tag_slugs: list[str],
    end_date_min: Optional[str] = None,
    end_date_max: Optional[str] = None,
    max_events_per_tag: Optional[int] = None,
) -> pd.DataFrame:
    events = fetch_events(session, tag_slugs, closed=True,
                           end_date_min=end_date_min, end_date_max=end_date_max,
                           max_events_per_tag=max_events_per_tag)
    rows = events_to_market_rows(events)
    df = pd.DataFrame(rows)
    if not df.empty:
        df = df.drop_duplicates(subset=["market_id"]).reset_index(drop=True)
    session.logger.info("fetch_markets_dataframe: %d market row(s) from %d event(s)", len(df), len(events))
    return df


# --------------------------------------------------------------------------
# Part 2: historical price/trade data + reaction-timestamp detection
# --------------------------------------------------------------------------

def _to_unix_seconds(iso_ts: Optional[str]) -> Optional[int]:
    if not iso_ts:
        return None
    ts = pd.Timestamp(iso_ts)
    if ts.tzinfo is None:
        ts = ts.tz_localize("UTC")
    return int(ts.timestamp())


# Confirmed live: /prices-history rejects a startTs/endTs span beyond
# somewhere between 15 and 18 days with HTTP 400 "'startTs' and 'endTs'
# interval is too long" -- undocumented, so this chunks any longer request
# into windows safely under that boundary and concatenates the results,
# rather than either failing on markets that ran longer than ~2 weeks or
# guessing the exact undocumented limit.
MAX_PRICE_HISTORY_WINDOW_SECONDS = 14 * 86400


def _fetch_price_history_chunk(
    session: RateLimitedSession,
    clob_token_id: str,
    start_ts: int,
    end_ts: int,
    fidelity_minutes: int,
) -> list[dict]:
    url = (f"{CLOB_BASE}/prices-history?market={clob_token_id}"
           f"&startTs={start_ts}&endTs={end_ts}&fidelity={fidelity_minutes}")
    resp = session.get(url, row_count_fn=lambda r: len(r.json().get("history", [])))
    if resp is None or resp.status_code != 200:
        session.logger.warning("fetch_price_history: failed for token=%s window=[%d,%d] (status=%s)",
                                clob_token_id, start_ts, end_ts, getattr(resp, "status_code", None))
        return []
    return resp.json().get("history", [])


def fetch_price_history(
    session: RateLimitedSession,
    clob_token_id: str,
    start_ts: int,
    end_ts: int,
    fidelity_minutes: int,
) -> list[dict]:
    """GET /prices-history for one outcome token, transparently chunked
    across MAX_PRICE_HISTORY_WINDOW_SECONDS-sized windows if the requested
    range is longer than that. Returns [{"t": unix_sec, "p": price}, ...]
    in ascending time order (the endpoint already returns ascending order
    within a window; chunks are fetched in ascending order too, so no
    re-sort is needed)."""
    history: list[dict] = []
    window_start = start_ts
    while window_start < end_ts:
        window_end = min(window_start + MAX_PRICE_HISTORY_WINDOW_SECONDS, end_ts)
        history.extend(_fetch_price_history_chunk(session, clob_token_id, window_start, window_end, fidelity_minutes))
        window_start = window_end
    return history


def fetch_trades(
    session: RateLimitedSession,
    condition_id: str,
    max_trades: int = 20000,
) -> list[dict]:
    """GET /trades?market=<condition_id>, paginated. Returns trades in
    whatever order the API gives them (newest-first, confirmed live) --
    callers that need chronological order should sort by "timestamp"
    themselves, which fetch_trades does NOT do so the raw fetch order is
    preserved for logging/debugging.
    """
    trades: list[dict] = []
    offset = 0
    while len(trades) < max_trades:
        url = f"{DATA_API_BASE}/trades?market={condition_id}&limit={TRADES_PAGE_SIZE}&offset={offset}"
        resp = session.get(url, row_count_fn=lambda r: len(r.json()))
        if resp is None or resp.status_code != 200:
            session.logger.warning("fetch_trades: failed for market=%s at offset=%d (status=%s)",
                                    condition_id, offset, getattr(resp, "status_code", None))
            break
        page = resp.json()
        if not page:
            break
        trades.extend(page)
        if len(page) < TRADES_PAGE_SIZE:
            break
        offset += TRADES_PAGE_SIZE
    return trades[:max_trades]


@dataclass
class ReactionResult:
    market_id: str
    condition_id: str
    price_points_count: int = 0
    trades_count: int = 0
    price_reaction_ts: Optional[int] = None
    volume_reaction_ts: Optional[int] = None
    reaction_ts: Optional[int] = None
    reaction_method: str = "none"
    notes: list[str] = field(default_factory=list)


def detect_reaction(
    price_history: list[dict],
    trades: list[dict],
    price_move_threshold: float,
    price_window_minutes: int,
    volume_percentile: float,
) -> tuple[Optional[int], Optional[int]]:
    """Core "what counts as a reaction" logic, deliberately simple and
    fully parameterized rather than hardcoded (per spec):

    - price candidate: first timestamp t[i] where the price has moved by
      at least `price_move_threshold` (absolute probability-points, e.g.
      0.10 = 10 cents) versus the most recent prior point at least
      `price_window_minutes` earlier.
    - volume candidate: the `volume_percentile`-th percentile of trade
      sizes across the market's *entire* trade history is used as the
      "significant size" bar; the first trade (in chronological order)
      at/above that bar is the candidate.

    Returns (price_reaction_ts, volume_reaction_ts); either may be None if
    that signal never fired (no price history / no trades / threshold
    never crossed).
    """
    price_reaction_ts = None
    if price_history:
        window_seconds = price_window_minutes * 60
        points = sorted(price_history, key=lambda pt: pt["t"])
        for i, cur in enumerate(points):
            # Find the most recent earlier point at least window_seconds back.
            j = i
            while j > 0 and points[i]["t"] - points[j - 1]["t"] < window_seconds:
                j -= 1
            if j == i:
                continue
            if abs(cur["p"] - points[j]["p"]) >= price_move_threshold:
                price_reaction_ts = cur["t"]
                break

    volume_reaction_ts = None
    if trades:
        sizes = [float(t["size"]) for t in trades if t.get("size") is not None]
        if sizes:
            threshold = statistics.quantiles(sizes, n=100)[int(volume_percentile) - 1] \
                if len(sizes) >= 2 else sizes[0]
            chronological = sorted(trades, key=lambda t: t["timestamp"])
            for t in chronological:
                if t.get("size") is not None and float(t["size"]) >= threshold:
                    volume_reaction_ts = int(t["timestamp"])
                    break

    return price_reaction_ts, volume_reaction_ts


def compute_reaction_for_market(
    session: RateLimitedSession,
    market_row: dict,
    price_move_threshold: float,
    price_window_minutes: int,
    volume_percentile: float,
    fidelity_minutes: int,
    max_trades: int,
) -> ReactionResult:
    market_id = market_row["market_id"]
    condition_id = market_row["condition_id"]
    result = ReactionResult(market_id=market_id, condition_id=condition_id)

    start_ts = _to_unix_seconds(market_row.get("start_date"))
    end_ts = _to_unix_seconds(market_row.get("closed_time") or market_row.get("end_date"))
    if not start_ts or not end_ts or start_ts >= end_ts:
        result.notes.append("missing or invalid start/end date, skipped price history fetch")
    else:
        token_ids = [t for t in str(market_row.get("clob_token_ids", "")).split("|") if t]
        price_history: list[dict] = []
        for token_id in token_ids:
            price_history.extend(fetch_price_history(session, token_id, start_ts, end_ts, fidelity_minutes))
        result.price_points_count = len(price_history)
        if not price_history:
            result.notes.append("no price history returned")

        if not condition_id:
            result.notes.append("no condition_id, skipped trades fetch")
            trades = []
        else:
            trades = fetch_trades(session, condition_id, max_trades=max_trades)
        result.trades_count = len(trades)
        if not trades:
            result.notes.append("no trades returned")

        price_ts, volume_ts = detect_reaction(
            price_history, trades, price_move_threshold, price_window_minutes, volume_percentile)
        result.price_reaction_ts = price_ts
        result.volume_reaction_ts = volume_ts

        candidates = [(ts, method) for ts, method in
                      [(price_ts, "price_move"), (volume_ts, "volume_spike")] if ts is not None]
        if candidates:
            candidates.sort(key=lambda pair: pair[0])
            result.reaction_ts, result.reaction_method = candidates[0]
            if len(candidates) > 1:
                result.reaction_method = "price_move+volume_spike"
        else:
            result.notes.append("neither price-move nor volume-spike threshold was crossed")

    return result


def fetch_reactions_dataframe(
    session: RateLimitedSession,
    markets_df: pd.DataFrame,
    price_move_threshold: float,
    price_window_minutes: int,
    volume_percentile: float,
    fidelity_minutes: int,
    max_trades: int,
) -> pd.DataFrame:
    rows = []
    total = len(markets_df)
    for i, market_row in enumerate(markets_df.to_dict("records")):
        session.logger.info("computing reaction for market %d/%d: %s (%s)",
                             i + 1, total, market_row.get("market_id"), market_row.get("title"))
        result = compute_reaction_for_market(
            session, market_row, price_move_threshold, price_window_minutes,
            volume_percentile, fidelity_minutes, max_trades)
        rows.append({
            "market_id": result.market_id,
            "condition_id": result.condition_id,
            "price_points_count": result.price_points_count,
            "trades_count": result.trades_count,
            "price_reaction_ts": result.price_reaction_ts,
            "volume_reaction_ts": result.volume_reaction_ts,
            "market_reaction_ts": result.reaction_ts,
            "reaction_method": result.reaction_method,
            "reaction_notes": "; ".join(result.notes),
        })
    return pd.DataFrame(rows)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _add_fetch_markets_args(p: argparse.ArgumentParser):
    p.add_argument("--tags", default=",".join(DEFAULT_TAG_SLUGS),
                   help="comma-separated Gamma API tag slugs (default: curated military/geopolitical set)")
    p.add_argument("--end-date-min", default=None, help="ISO-8601, only events resolving on/after this")
    p.add_argument("--end-date-max", default=None, help="ISO-8601, only events resolving on/before this")
    p.add_argument("--max-events-per-tag", type=int, default=None)
    p.add_argument("--output", default="data/markets.csv")
    p.add_argument("--log-file", default="data/pipeline.log")


def _add_fetch_reactions_args(p: argparse.ArgumentParser):
    p.add_argument("--markets-csv", default="data/markets.csv")
    p.add_argument("--output", default="data/reactions.csv")
    p.add_argument("--price-move-threshold", type=float, default=0.10,
                   help="absolute price move (0-1 probability points) that counts as a reaction (default 0.10)")
    p.add_argument("--price-window-minutes", type=int, default=60,
                   help="rolling window for the price-move check, minutes (default 60)")
    p.add_argument("--volume-percentile", type=float, default=95.0,
                   help="trade-size percentile (of the market's full trade history) that counts as a spike (default 95)")
    p.add_argument("--fidelity-minutes", type=int, default=5,
                   help="prices-history sample spacing, minutes (default 5)")
    p.add_argument("--max-trades", type=int, default=20000)
    p.add_argument("--log-file", default="data/pipeline.log")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p1 = sub.add_parser("fetch-markets", help="Part 1: pull resolved candidate markets")
    _add_fetch_markets_args(p1)

    p2 = sub.add_parser("fetch-reactions", help="Part 2: pull price/trade history, detect reaction timestamps")
    _add_fetch_reactions_args(p2)

    args = parser.parse_args()
    logger = setup_logging(args.log_file)
    session = RateLimitedSession(logger, min_delay_seconds=0.3)

    if args.command == "fetch-markets":
        tag_slugs = [t.strip() for t in args.tags.split(",") if t.strip()]
        df = fetch_markets_dataframe(session, tag_slugs, args.end_date_min, args.end_date_max,
                                      args.max_events_per_tag)
        df.to_csv(args.output, index=False)
        logger.info("wrote %d market row(s) to %s", len(df), args.output)

    elif args.command == "fetch-reactions":
        markets_df = pd.read_csv(args.markets_csv)
        df = fetch_reactions_dataframe(
            session, markets_df, args.price_move_threshold, args.price_window_minutes,
            args.volume_percentile, args.fidelity_minutes, args.max_trades)
        df.to_csv(args.output, index=False)
        logger.info("wrote %d reaction row(s) to %s", len(df), args.output)


if __name__ == "__main__":
    main()
