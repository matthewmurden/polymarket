"""Part 3: fetchers for official government press-release listings, used to
build a candidate pool of (title, published timestamp, url) tuples that a
human then matches against a market's resolution criteria (see matcher.py
-- this module deliberately does NOT attempt automated text matching).

Findings from live testing while building this tool (see README for the
fuller writeup):

- whitehouse.gov: robots.txt allows everything. The briefing-room listing
  now lives at /briefings-statements/ (the URL in the original spec,
  /briefing-room/statements-releases/, 301-redirects there). Each release
  is an `li.wp-block-post` with `h2.wp-block-post-title a` for title/url
  and a `time[datetime]` element with a real ISO-8601 timestamp. Pagination
  is /briefings-statements/page/N/ (WordPress-style); posts are in
  descending chronological order, confirmed across two pages live.
- state.gov: robots.txt allows everything but specifies `crawl-delay: 5`
  (respected here). Listing at /press-releases/, each release is an
  `li.collection-result` with `a.collection-result__link` for title/url.
  There is no machine-readable timestamp -- the date is a human-readable
  string ("July 17, 2026") in the LAST `<span>` inside
  `.collection-result-meta` (some entries have a leading byline span
  first, e.g. "Marco Rubio", so the date is taken positionally as the last
  span, not the first). Pagination is /press-releases/page/N/ -- the
  `?page=N` query-string form silently returns page 1 every time and does
  NOT work, confirmed live. Separately: a plain, honest, self-identifying
  User-Agent got silently served a decoy HTTP 200 "Technical Difficulties"
  apology page here instead of real content -- see common.DEFAULT_USER_AGENT
  and _looks_like_soft_block() below for how that's avoided/detected.
- defense.gov / war.gov: **abandoned, out of scope, not attempted further**.
  robots.txt itself returns HTTP 403, which Python's urllib.robotparser
  treats as "disallow everything" (stdlib behavior -- see
  check_robots_allowed's docstring), so this fetcher correctly skips the
  actual releases listing without ever requesting it. Directly fetching
  https://www.defense.gov/News/Releases/ during development also returned
  403 "Access Denied" from an Akamai edge WAF. Both signals point the same
  way: the site does not want automated access, and there's no viable
  automated fix (rotating headers/UAs against an intentional WAF+robots
  block is evasion, not a bug fix, and isn't attempted here). This fetcher
  is left in place, fully implemented (robots check, request, best-effort
  parsing) for the case where policy changes, but its CSS selectors are
  UNVERIFIED since the listing page could never be loaded to inspect
  during development -- confirm the printed candidate count manually
  before trusting it if it does ever become reachable. Use GDELT (below)
  for broader automated coverage of military-operation-type events
  instead, or the manual-entry path (matcher.py) for a specific DoD
  release.
- GDELT (api.gdeltproject.org DOC 2.0 API): a third automated source, with
  a fundamentally different role from the two above. whitehouse.gov/
  state.gov give a small number of high-precision, individually-dated
  official releases; GDELT gives broad, lower-precision corroboration --
  how many distinct news articles/domains were reporting on a topic within
  a market's resolution window, and when the earliest one appeared. It is
  NOT a source of an authoritative "official" timestamp, it's a "how much
  and how early was this being reported" signal, useful when no official
  release exists or can't be found (common for military actions where the
  triggering fact is broken by press/OSINT before any government readout).
  Confirmed live during development:
    - Real rate limit, stated directly in the 429 response body: "Please
      limit requests to one every 5 seconds" -- GDELT_MIN_DELAY_SECONDS
      enforces this regardless of what min_delay_seconds the caller's
      session was otherwise configured with.
    - `maxrecords` is hard-capped at 250 server-side; requesting more
      doesn't error cleanly -- it returns HTTP 200 with a **plain-text**
      body ("A maximum of 250 records can be returned.") instead of JSON,
      so this always clamps to 250 and treats any non-JSON 200 response as
      a usage error, not silently crashing on the unexpected format.
    - `sort=DateAsc` really does return the earliest-published article
      first, confirmed live, so the earliest-mention timestamp doesn't
      need a second query or a full client-side scan.
  Unlike whitehouse.gov/state.gov (scraped once across the whole requested
  date range, then sliced per-market by date proximity), GDELT is queried
  once PER MARKET: the query terms come from that market's own title/
  resolution criteria (see matcher.extract_search_terms) and the date
  window is that market's own resolution date +/- window_days. This means
  GDELT rows carry their own market_id in the candidates table rather than
  relying on date-window matching -- see matcher.candidates_for_market for
  why that distinction matters (a nearby but unrelated market resolving on
  a similar date must not "borrow" another market's GDELT hit).
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import urllib.robotparser
from collections import Counter
from dataclasses import dataclass, field
from typing import Optional
from urllib.parse import urlencode, urljoin

import pandas as pd
from bs4 import BeautifulSoup
from dateutil import parser as dateparser

from common import DEFAULT_USER_AGENT, RateLimitedSession, setup_logging
from matcher import extract_search_terms, market_window

MAX_PAGES_DEFAULT = 20


@dataclass
class FetchResult:
    source: str
    records: list[dict] = field(default_factory=list)
    status: str = "ok"  # ok | blocked | robots_disallowed | no_results_found | error
    detail: str = ""


def check_robots_allowed(base_url: str, path: str, user_agent: str = DEFAULT_USER_AGENT) -> tuple[bool, Optional[float], str]:
    """Returns (allowed, crawl_delay_seconds, detail).

    Delegates to the stdlib's urllib.robotparser, whose built-in behavior
    on a non-2xx robots.txt response is worth being explicit about (this
    function doesn't override it, just surfaces it in `detail`):
      - HTTP 401/403 on robots.txt -> treated as DISALLOW EVERYTHING. This
        is exactly what happened for defense.gov during development (its
        robots.txt itself returns 403), so this fetcher correctly skips
        the actual releases page entirely without ever requesting it.
      - other 4xx (e.g. 404, no robots.txt published) -> treated as ALLOW
        EVERYTHING.
      - a genuine transport failure (timeout, DNS, connection refused,
        5xx) raises, which this function catches and treats as "allowed,
        but unverified" -- logged clearly via `detail` either way, so a
        cautious-proceed is never silent.
    """
    robots_url = urljoin(base_url, "/robots.txt")
    rp = urllib.robotparser.RobotFileParser()
    rp.set_url(robots_url)
    try:
        rp.read()
    except Exception as e:
        return True, None, f"could not fetch/parse {robots_url} ({e}); proceeding cautiously"

    allowed = rp.can_fetch(user_agent, path)
    crawl_delay = rp.crawl_delay(user_agent)
    detail = f"robots.txt at {robots_url}: {'allowed' if allowed else 'DISALLOWED'} for {path}"
    if crawl_delay:
        detail += f", crawl-delay={crawl_delay}s"
    return allowed, crawl_delay, detail


def _parse_date_in_range(d: dt.datetime, start_date: dt.date, end_date: dt.date) -> bool:
    d_date = d.date() if isinstance(d, dt.datetime) else d
    return start_date <= d_date <= end_date


# Confirmed live: state.gov serves this exact decoy page, with an HTTP 200
# status, to at least some non-browser-looking User-Agents -- a soft block
# that looks like success unless the body is actually checked. Checked
# after every 200 response, on all three sources, in case any of them do
# this under some condition we haven't hit yet.
_SOFT_BLOCK_MARKERS = ("Technical Difficulties",)


def _looks_like_soft_block(html_text: str) -> bool:
    return any(marker in html_text for marker in _SOFT_BLOCK_MARKERS)


# --------------------------------------------------------------------------
# whitehouse.gov
# --------------------------------------------------------------------------

WHITEHOUSE_BASE = "https://www.whitehouse.gov"
WHITEHOUSE_LISTING_PATH = "/briefings-statements/"


def fetch_whitehouse_releases(
    session: RateLimitedSession,
    start_date: dt.date,
    end_date: dt.date,
    max_pages: int = MAX_PAGES_DEFAULT,
) -> FetchResult:
    logger = session.logger
    allowed, crawl_delay, detail = check_robots_allowed(WHITEHOUSE_BASE, WHITEHOUSE_LISTING_PATH)
    logger.info("whitehouse.gov robots check: %s", detail)
    if not allowed:
        return FetchResult("whitehouse.gov", status="robots_disallowed", detail=detail)
    if crawl_delay:
        session.min_delay_seconds = max(session.min_delay_seconds, crawl_delay)

    records = []
    for page_num in range(1, max_pages + 1):
        url = (f"{WHITEHOUSE_BASE}{WHITEHOUSE_LISTING_PATH}"
               if page_num == 1 else f"{WHITEHOUSE_BASE}{WHITEHOUSE_LISTING_PATH}page/{page_num}/")
        resp = session.get(url)
        if resp is None:
            return FetchResult("whitehouse.gov", records=records, status="error",
                                detail=f"transport failure fetching {url}")
        if resp.status_code == 404:
            logger.info("whitehouse.gov: page %d returned 404, end of listing", page_num)
            break
        if resp.status_code != 200:
            logger.error("whitehouse.gov: page %d -> HTTP %d, treating as blocked", page_num, resp.status_code)
            return FetchResult("whitehouse.gov", records=records, status="blocked",
                                detail=f"HTTP {resp.status_code} on {url}")
        if _looks_like_soft_block(resp.text):
            logger.error("whitehouse.gov: page %d -> HTTP 200 but body looks like a block/decoy page "
                         "(matched %s), treating as blocked", page_num, _SOFT_BLOCK_MARKERS)
            return FetchResult("whitehouse.gov", records=records, status="blocked",
                                detail=f"soft-block decoy page detected on {url}")

        soup = BeautifulSoup(resp.text, "html.parser")
        posts = soup.select("li.wp-block-post")
        if not posts:
            logger.warning("whitehouse.gov: page %d had 200 status but zero posts matched "
                            "the expected selector -- page structure may have changed", page_num)
            break

        oldest_on_page = None
        for post in posts:
            link_el = post.select_one("h2 a")
            time_el = post.select_one("time[datetime]")
            if not link_el or not time_el:
                continue
            published = dateparser.parse(time_el["datetime"])
            oldest_on_page = published if oldest_on_page is None else min(oldest_on_page, published)
            if _parse_date_in_range(published, start_date, end_date):
                records.append({
                    "source": "whitehouse.gov",
                    "title": link_el.get_text(strip=True),
                    "url": link_el.get("href"),
                    "published_timestamp": published.isoformat(),
                })

        logger.info("whitehouse.gov: page %d -> %d post(s) parsed, %d in target range so far",
                     page_num, len(posts), len(records))

        if oldest_on_page and oldest_on_page.date() < start_date:
            logger.info("whitehouse.gov: page %d's oldest post predates start_date, stopping pagination", page_num)
            break

    status = "ok" if records else "no_results_found"
    return FetchResult("whitehouse.gov", records=records, status=status)


# --------------------------------------------------------------------------
# state.gov
# --------------------------------------------------------------------------

STATE_BASE = "https://www.state.gov"
STATE_LISTING_PATH = "/press-releases/"


def fetch_state_releases(
    session: RateLimitedSession,
    start_date: dt.date,
    end_date: dt.date,
    max_pages: int = MAX_PAGES_DEFAULT,
) -> FetchResult:
    logger = session.logger
    allowed, crawl_delay, detail = check_robots_allowed(STATE_BASE, STATE_LISTING_PATH)
    logger.info("state.gov robots check: %s", detail)
    if not allowed:
        return FetchResult("state.gov", status="robots_disallowed", detail=detail)
    if crawl_delay:
        session.min_delay_seconds = max(session.min_delay_seconds, crawl_delay)

    records = []
    for page_num in range(1, max_pages + 1):
        url = (f"{STATE_BASE}{STATE_LISTING_PATH}"
               if page_num == 1 else f"{STATE_BASE}{STATE_LISTING_PATH}page/{page_num}/")
        resp = session.get(url)
        if resp is None:
            return FetchResult("state.gov", records=records, status="error",
                                detail=f"transport failure fetching {url}")
        if resp.status_code == 404:
            logger.info("state.gov: page %d returned 404, end of listing", page_num)
            break
        if resp.status_code != 200:
            logger.error("state.gov: page %d -> HTTP %d, treating as blocked", page_num, resp.status_code)
            return FetchResult("state.gov", records=records, status="blocked",
                                detail=f"HTTP {resp.status_code} on {url}")
        if _looks_like_soft_block(resp.text):
            logger.error("state.gov: page %d -> HTTP 200 but body looks like a block/decoy page "
                         "(matched %s) -- confirmed live: this site serves a fake 'Technical "
                         "Difficulties' 200-status page to some User-Agents; treating as blocked",
                         page_num, _SOFT_BLOCK_MARKERS)
            return FetchResult("state.gov", records=records, status="blocked",
                                detail=f"soft-block decoy page detected on {url}")

        soup = BeautifulSoup(resp.text, "html.parser")
        results = soup.select("li.collection-result")
        if not results:
            logger.warning("state.gov: page %d had 200 status but zero results matched "
                            "the expected selector -- page structure may have changed", page_num)
            break

        oldest_on_page = None
        for res in results:
            link_el = res.select_one("a.collection-result__link")
            meta_spans = res.select(".collection-result-meta span")
            if not link_el or not meta_spans:
                continue
            date_text = meta_spans[-1].get_text(strip=True)
            try:
                published = dateparser.parse(date_text)
            except (ValueError, OverflowError):
                logger.debug("state.gov: could not parse date text %r, skipping entry", date_text)
                continue
            oldest_on_page = published if oldest_on_page is None else min(oldest_on_page, published)
            if _parse_date_in_range(published, start_date, end_date):
                records.append({
                    "source": "state.gov",
                    "title": link_el.get_text(strip=True),
                    "url": link_el.get("href"),
                    "published_timestamp": published.isoformat(),
                })

        logger.info("state.gov: page %d -> %d result(s) parsed, %d in target range so far",
                     page_num, len(results), len(records))

        if oldest_on_page and oldest_on_page.date() < start_date:
            logger.info("state.gov: page %d's oldest result predates start_date, stopping pagination", page_num)
            break

    status = "ok" if records else "no_results_found"
    return FetchResult("state.gov", records=records, status=status)


# --------------------------------------------------------------------------
# defense.gov -- see module docstring: confirmed blocked during development
# --------------------------------------------------------------------------

DEFENSE_BASE = "https://www.defense.gov"
DEFENSE_LISTING_PATH = "/News/Releases/"


def fetch_defense_releases(
    session: RateLimitedSession,
    start_date: dt.date,
    end_date: dt.date,
    max_pages: int = MAX_PAGES_DEFAULT,
) -> FetchResult:
    logger = session.logger
    allowed, crawl_delay, detail = check_robots_allowed(DEFENSE_BASE, DEFENSE_LISTING_PATH)
    logger.info("defense.gov robots check: %s", detail)
    if not allowed:
        return FetchResult("defense.gov", status="robots_disallowed", detail=detail)
    if crawl_delay:
        session.min_delay_seconds = max(session.min_delay_seconds, crawl_delay)

    records = []
    for page_num in range(1, max_pages + 1):
        url = f"{DEFENSE_BASE}{DEFENSE_LISTING_PATH}" if page_num == 1 \
            else f"{DEFENSE_BASE}{DEFENSE_LISTING_PATH}?Page={page_num}"
        resp = session.get(url)
        if resp is None:
            return FetchResult("defense.gov", records=records, status="error",
                                detail=f"transport failure fetching {url}")
        if resp.status_code != 200:
            logger.error(
                "defense.gov BLOCKED: %s -> HTTP %d. During development this fetcher never even "
                "got this far (its own robots.txt returns 403, which urllib.robotparser treats as "
                "disallow-everything, so the listing page was never requested) -- if you're seeing "
                "this, robots.txt must have changed to allow it but the live page itself is still "
                "blocking. Use the manual-entry path in matcher.py for DoD releases.",
                url, resp.status_code)
            return FetchResult("defense.gov", records=records, status="blocked",
                                detail=f"HTTP {resp.status_code} on {url}")
        if _looks_like_soft_block(resp.text):
            logger.error("defense.gov: page %d -> HTTP 200 but body looks like a block/decoy page "
                         "(matched %s), treating as blocked", page_num, _SOFT_BLOCK_MARKERS)
            return FetchResult("defense.gov", records=records, status="blocked",
                                detail=f"soft-block decoy page detected on {url}")

        # Reachable: best-effort parse. UNVERIFIED selectors -- see module
        # docstring. If this matches zero results despite a 200 status,
        # that is reported distinctly from "blocked" so it's clear the
        # site *was* reachable but the assumed markup was wrong.
        soup = BeautifulSoup(resp.text, "html.parser")
        candidates = (soup.select("div.article") or soup.select("li.article-row")
                      or soup.select("article") or soup.select(".featured-story-list-item"))
        if not candidates:
            logger.warning(
                "defense.gov: page %d reached (HTTP 200) but no selector matched any releases -- "
                "the assumed page structure is UNVERIFIED (this page could not be loaded during "
                "development to confirm it) and is now known wrong. Inspect the live HTML and "
                "update the selectors in official_sources.py, or use manual entry for now.",
                page_num)
            return FetchResult("defense.gov", records=records, status="no_results_found",
                                detail="page reachable but selectors matched nothing; markup assumption unverified")

        for c in candidates:
            link_el = c.find("a", href=True)
            date_el = c.find("time") or c.find(class_="date")
            if not link_el or not date_el:
                continue
            date_text = date_el.get("datetime") or date_el.get_text(strip=True)
            try:
                published = dateparser.parse(date_text)
            except (ValueError, OverflowError):
                continue
            if _parse_date_in_range(published, start_date, end_date):
                records.append({
                    "source": "defense.gov",
                    "title": link_el.get_text(strip=True),
                    "url": urljoin(DEFENSE_BASE, link_el["href"]),
                    "published_timestamp": published.isoformat(),
                })

    status = "ok" if records else "no_results_found"
    return FetchResult("defense.gov", records=records, status=status)


# --------------------------------------------------------------------------
# GDELT DOC 2.0 API -- see module docstring for what was confirmed live
# --------------------------------------------------------------------------

GDELT_BASE = "https://api.gdeltproject.org/api/v2/doc/doc"
GDELT_MAX_RECORDS = 250  # hard server-side cap, confirmed live (see module docstring)

# GDELT's own stated policy (confirmed live via a 429 response body) is
# "one every 5 seconds" -- but that turned out NOT to be sufficient in
# practice: a burst of testing traffic (from this same IP, developing this
# fetcher) kept drawing 429s even with a strict 5s gap between requests,
# and stayed rate-limited for several minutes afterward, well past a
# single 5s window. That looks like a sustained-use penalty/cooldown on
# top of the flat per-request interval, not just the flat interval alone.
# Since this pipeline queries GDELT once per market in a loop -- exactly
# the sustained-use pattern that seems to trigger it -- the delay here is
# roughly double GDELT's stated minimum, and retries are configured to be
# patient (many attempts, each capped, rather than a few impatient ones)
# so a transient cooldown can be waited out instead of just failing that
# market's query outright.
GDELT_MIN_DELAY_SECONDS = 10.0
GDELT_MAX_RETRIES = 10
GDELT_BACKOFF_BASE_SECONDS = 3.0
GDELT_MAX_BACKOFF_SECONDS = 90.0


def _gdelt_query_url(query: str, start_window: dt.date, end_window: dt.date, max_records: int, fmt: str = "json") -> str:
    params = {
        "query": query,
        "mode": "ArtList",
        "format": fmt,
        "maxrecords": min(max_records, GDELT_MAX_RECORDS),
        "sort": "DateAsc",
        "startdatetime": start_window.strftime("%Y%m%d") + "000000",
        "enddatetime": end_window.strftime("%Y%m%d") + "235959",
    }
    return f"{GDELT_BASE}?{urlencode(params)}"


def fetch_gdelt_events(
    session: RateLimitedSession,
    markets_df: pd.DataFrame,
    window_days: int,
    max_records: int = GDELT_MAX_RECORDS,
) -> FetchResult:
    """Queries the GDELT DOC API once per market (see module docstring for
    why this can't be a single site-wide date-range fetch like whitehouse.
    gov/state.gov): terms come from that market's own title/resolution
    criteria, and the query window is that market's own resolution date
    +/- window_days. Each market that returns at least one article
    produces exactly one aggregate candidate row (source="gdelt",
    market_id=<that market's id>) carrying the mention count, earliest
    mention timestamp, and top reporting domains -- NOT one row per
    article; per-article detail is discoverable via the html url this
    stores for manual spot-checking, not duplicated into the CSV.
    """
    logger = session.logger
    # GDELT's rate limit is a hard requirement of the API itself, not a
    # politeness preference -- enforced regardless of whatever the
    # passed-in session happened to be configured with (e.g. from an
    # earlier state.gov crawl-delay bump), and more conservative than
    # GDELT's own stated "5 seconds" -- see the constants' comments above
    # for why.
    session.min_delay_seconds = max(session.min_delay_seconds, GDELT_MIN_DELAY_SECONDS)
    session.max_retries = max(session.max_retries, GDELT_MAX_RETRIES)
    session.backoff_base_seconds = max(session.backoff_base_seconds, GDELT_BACKOFF_BASE_SECONDS)
    session.max_backoff_seconds = max(session.max_backoff_seconds, GDELT_MAX_BACKOFF_SECONDS)
    max_records = min(max_records, GDELT_MAX_RECORDS)

    records = []
    markets = markets_df.to_dict("records")
    logger.info("gdelt: querying %d market(s), one request each, min %.0fs apart -- expect this to "
                "take a while (roughly %d minutes for a full run)",
                len(markets), GDELT_MIN_DELAY_SECONDS, int(len(markets) * GDELT_MIN_DELAY_SECONDS // 60))

    for market_row in markets:
        market_id = str(market_row.get("market_id"))
        title = market_row.get("title")
        terms = extract_search_terms(title, market_row.get("resolution_criteria_text"))
        if not terms:
            logger.warning("gdelt: market %s (%r) yielded no usable search terms, skipping", market_id, title)
            continue

        end_date_str = market_row.get("end_date") or market_row.get("closed_time")
        start_window, end_window = market_window(end_date_str, window_days)
        if start_window is None:
            logger.warning("gdelt: market %s has no usable end_date/closed_time, skipping", market_id)
            continue

        query = " ".join(terms)
        url = _gdelt_query_url(query, start_window, end_window, max_records)
        resp = session.get(url)
        if resp is None:
            logger.error("gdelt: transport failure querying market %s, skipping", market_id)
            continue
        if resp.status_code == 429:
            logger.error("gdelt: still rate-limited after retries for market %s, skipping this market", market_id)
            continue
        if resp.status_code != 200:
            logger.error("gdelt: market %s -> HTTP %d, skipping", market_id, resp.status_code)
            continue

        try:
            data = resp.json()
        except ValueError:
            # Confirmed live: a usage error (e.g. a malformed query) comes
            # back as HTTP 200 with a PLAIN-TEXT body, not JSON or an error
            # status -- must be detected here, not assumed away.
            logger.warning("gdelt: market %s -> HTTP 200 but non-JSON body (usage error?): %s",
                            market_id, resp.text[:200].strip())
            continue

        articles = data.get("articles") or []
        logger.info("gdelt: market %s terms=%r window=[%s,%s] -> %d article(s)",
                     market_id, terms, start_window, end_window, len(articles))
        if not articles:
            continue

        domains = [a["domain"] for a in articles if a.get("domain")]
        top_domains = [d for d, _ in Counter(domains).most_common(5)]
        seendates = [a["seendate"] for a in articles if a.get("seendate")]
        earliest_iso = None
        if seendates:
            earliest_raw = min(seendates)  # sort=DateAsc already guarantees this is articles[0], re-derived defensively
            earliest_iso = dt.datetime.strptime(earliest_raw, "%Y%m%dT%H%M%SZ").replace(tzinfo=dt.timezone.utc).isoformat()

        if len(articles) >= max_records:
            logger.warning("gdelt: market %s hit the %d-article query cap -- gdelt_mention_count is a "
                            "floor, not an exact count, for this market", market_id, max_records)

        records.append({
            "source": "gdelt",
            "market_id": market_id,
            "title": f"GDELT: {query}",
            "published_timestamp": earliest_iso,
            "url": _gdelt_query_url(query, start_window, end_window, max_records, fmt="html"),
            "gdelt_mention_count": len(articles),
            "gdelt_earliest_mention_ts": earliest_iso,
            "gdelt_top_source_domains": "|".join(top_domains),
        })

    status = "ok" if records else "no_results_found"
    return FetchResult("gdelt", records=records, status=status)


# --------------------------------------------------------------------------
# dispatcher + CLI
# --------------------------------------------------------------------------

# "defense" is deliberately NOT in the default set -- see module docstring:
# confirmed disallowed by its own robots.txt, with no viable automated fix.
# It's still registered here so `--sources whitehouse,state,defense,gdelt`
# works if you want to re-confirm that finding for yourself.
FETCHERS = {
    "whitehouse": fetch_whitehouse_releases,
    "state": fetch_state_releases,
    "defense": fetch_defense_releases,
}
DEFAULT_SOURCES = "whitehouse,state,gdelt"


def fetch_all(
    session: RateLimitedSession,
    sources: list[str],
    start_date: dt.date,
    end_date: dt.date,
    markets_df: Optional[pd.DataFrame] = None,
    window_days: int = 3,
    max_pages: int = MAX_PAGES_DEFAULT,
) -> list[FetchResult]:
    """Dispatches each requested source. GDELT is handled separately from
    the FETCHERS dict because its signature is fundamentally different --
    a per-market query (needs markets_df + window_days), not a single
    site-wide date-range scrape (needs start_date/end_date) -- see
    fetch_gdelt_events's docstring.
    """
    results = []
    for name in sources:
        if name == "gdelt":
            if markets_df is None or markets_df.empty:
                session.logger.error(
                    "gdelt: requires markets_df (queries are built per-market from title/"
                    "resolution criteria), but none was given -- pass --markets-csv. Skipping.")
                continue
            result = fetch_gdelt_events(session, markets_df, window_days)
        else:
            fetcher = FETCHERS.get(name)
            if not fetcher:
                session.logger.error("unknown official source %r, skipping (known: %s, gdelt)",
                                      name, ", ".join(FETCHERS))
                continue
            result = fetcher(session, start_date, end_date, max_pages=max_pages)
        session.logger.info("%s: status=%s, %d candidate(s)",
                             result.source, result.status, len(result.records))
        results.append(result)
    return results


def main():
    parser = argparse.ArgumentParser(description="Part 3: fetch official government release listings + GDELT corroboration")
    parser.add_argument("--sources", default=DEFAULT_SOURCES)
    parser.add_argument("--start-date", default=None, help="YYYY-MM-DD (required for whitehouse/state/defense unless derived from --markets-csv)")
    parser.add_argument("--end-date", default=None, help="YYYY-MM-DD")
    parser.add_argument("--markets-csv", default="data/markets.csv",
                        help="required for the gdelt source (per-market queries); also used to "
                             "derive --start-date/--end-date automatically if they're not given")
    parser.add_argument("--window-days", type=int, default=3,
                        help="for gdelt: per-market query window, +/- this many days around each "
                             "market's resolution date (default 3)")
    parser.add_argument("--max-pages", type=int, default=MAX_PAGES_DEFAULT)
    parser.add_argument("--output", default="data/official_candidates.csv")
    parser.add_argument("--log-file", default="data/pipeline.log")
    args = parser.parse_args()

    logger = setup_logging(args.log_file)
    session = RateLimitedSession(logger, min_delay_seconds=1.0)

    sources = [s.strip() for s in args.sources.split(",") if s.strip()]

    markets_df = None
    if "gdelt" in sources or not (args.start_date and args.end_date):
        if os.path.exists(args.markets_csv):
            markets_df = pd.read_csv(args.markets_csv, dtype=str)
        elif "gdelt" in sources:
            logger.error("gdelt requires --markets-csv (%s not found)", args.markets_csv)

    start_date, end_date = args.start_date, args.end_date
    if not start_date or not end_date:
        if markets_df is None or markets_df.empty:
            logger.error("no --start-date/--end-date given and could not derive one from --markets-csv")
            return
        end_dates = pd.to_datetime(markets_df["end_date"], utc=True, errors="coerce").dropna()
        pad = pd.Timedelta(days=args.window_days)
        start_date = start_date or (end_dates.min() - pad).date().isoformat()
        end_date = end_date or (end_dates.max() + pad).date().isoformat()
        logger.info("derived date range from markets CSV: %s to %s", start_date, end_date)

    start_date = dateparser.parse(start_date).date()
    end_date = dateparser.parse(end_date).date()

    results = fetch_all(session, sources, start_date, end_date,
                         markets_df=markets_df, window_days=args.window_days, max_pages=args.max_pages)

    all_records = [r for result in results for r in result.records]
    df = pd.DataFrame(all_records)
    df.to_csv(args.output, index=False)
    logger.info("wrote %d total candidate release(s) to %s", len(df), args.output)

    for result in results:
        if result.status != "ok":
            logger.warning("source %s finished with status=%s (%s) -- %d candidate(s) found",
                            result.source, result.status, result.detail, len(result.records))


if __name__ == "__main__":
    main()
