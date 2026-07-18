# poly_event_calibration

One-off historical data collection pipeline that builds a "official
government/military announcement timestamp vs Polymarket's own price/volume
reaction timestamp" dataset for resolved military/geopolitical Polymarket
markets. This is calibration/training data for a separate insider-trading
detection project -- a real corpus of "how fast did the market react to
genuine news" for events that have already happened. It is **not** a live
monitoring service (see `polyLagTracker/` in this repo for that).

## Pipeline overview

```
1. fetch-markets    Gamma API -> candidate resolved markets (data/markets.csv)
2. fetch-reactions  CLOB/Data API -> price+trade history -> reaction timestamp (data/reactions.csv)
3. fetch-official   whitehouse.gov / state.gov (high precision) + GDELT (broad, lower precision)
                     -> candidate official releases + corroboration (data/official_candidates.csv)
   review           prints each market's resolution criteria + matching candidates for a human to pick from
   (hand-fill data/manual_review.csv with the chosen official_timestamp)
4. build            joins everything -> data/calibration_dataset.csv, tagging each row's T-zero as
                     official (high precision) or gdelt_estimate (lower precision, fallback only)
```

Each stage writes a plain CSV that the next stage reads, so you can inspect/
edit/re-run any stage independently. Every HTTP fetch (url, status, row
count) is logged to both the console and `data/pipeline.log`.

## Install

```
cd polyEventCalibration
pip install -r requirements.txt
```

## Run, in sequence

All commands below can be run either via `build_dataset.py <subcommand>`
(shown here) or by calling the individual module directly, e.g.
`python polymarket_fetch.py fetch-markets ...` -- both call the same
underlying functions.

### 1. Fetch candidate markets

```
python build_dataset.py fetch-markets --output data/markets.csv
```

Pulls resolved (`closed=true`) events from the Gamma API filtered to a
curated set of tag slugs (`--tags`, default: `geopolitics,military,
military-invasion,military-strikes,war,international-relations,
department-of-defense,nato` -- confirmed live against the real `/tags`
taxonomy while building this; there is no single clean "category" field for
this, see the docstring in `polymarket_fetch.py`). Optionally narrow by
`--end-date-min`/`--end-date-max` (ISO-8601). Writes one row per market
(an event can bundle several), with columns including `market_id`,
`condition_id`, `title`, `tags`, `resolution_source_field`,
`resolution_criteria_text`, `clob_token_ids`, `end_date`, `closed_time`.

**Note on resolution text:** Polymarket's structured `resolutionSource`
field is very often empty in practice -- the actual resolution criteria
(which usually names a source in prose) lives in the market/event
`description` field. This tool captures both; `resolution_criteria_text`
is what you should actually read.

### 2. Fetch price/trade history and detect a reaction timestamp

```
python build_dataset.py fetch-reactions --markets-csv data/markets.csv --output data/reactions.csv
```

For each market, pulls its full price history (`CLOB /prices-history`,
per outcome token) and full trade history (`Data API /trades?market=
<condition_id>` -- note: that endpoint's `asset=` filter does **not**
actually filter, confirmed by testing, so this always uses `market=`).

"Reaction timestamp" is deliberately simple and fully tunable, not a fixed
model:
- **price-move candidate**: first timestamp where price has moved by at
  least `--price-move-threshold` (absolute probability-points, default
  `0.10` = 10 cents) versus the most recent point at least
  `--price-window-minutes` earlier (default 60).
- **volume-spike candidate**: the `--volume-percentile`-th percentile
  (default 95) of trade sizes across the market's *entire* history is used
  as the "big trade" bar; the first trade at/above that bar, in
  chronological order.
- The earlier of the two (whichever fires first) is
  `market_reaction_timestamp`; `reaction_method` records which signal(s)
  fired.

Tune these flags per your own definition of "significant" -- the defaults
are a reasonable starting point, not a validated threshold.

### 3. Fetch official-source + GDELT candidates, then manually confirm each match

```
python build_dataset.py fetch-official --markets-csv data/markets.csv --output data/official_candidates.csv
python build_dataset.py review
```

`fetch-official` combines two structurally different kinds of source,
described below, into one candidates CSV (`--sources`, default
`whitehouse,state,gdelt`). If you don't pass `--start-date`/`--end-date`
explicitly, the range for whitehouse.gov/state.gov is derived automatically
from `data/markets.csv`'s resolution dates, padded by `--window-days`
(default 3) on each side; GDELT always uses a per-market window of the same
size regardless (see below).

#### High precision: whitehouse.gov / state.gov (individually-dated official releases)

Confirmed live while building this (see `official_sources.py`'s docstring
for the full detail):
- `whitehouse.gov` -- works. robots.txt allows everything.
- `state.gov` -- works. robots.txt allows everything but asks for a
  5-second crawl delay, which is respected. Separately: this site served a
  fake HTTP-200 "Technical Difficulties" decoy page to a plain,
  self-identifying User-Agent -- a soft block that looks like success
  unless the body is actually checked (`_looks_like_soft_block` catches
  this on all sources, and `common.DEFAULT_USER_AGENT` uses a standard
  browser string to avoid triggering it in the first place; robots.txt is
  still what's actually respected/checked, this doesn't bypass it).
- `defense.gov` / `war.gov` -- **abandoned, out of scope**. Its own
  robots.txt returns HTTP 403 (which Python's `urllib.robotparser` treats
  as disallow-everything -- stdlib behavior), and the live releases page
  independently returned 403 "Access Denied" from an Akamai edge WAF too.
  Both point the same way: no automated access wanted, and there's no
  legitimate fix for that (rotating headers/UAs against an intentional
  block is evasion, not a bug fix, and isn't attempted here). The fetcher
  is left in the code for if policy ever changes, but it's not in
  `--sources` by default -- pass `--sources whitehouse,state,gdelt,defense`
  if you want to re-confirm this for yourself. **GDELT (below) is the
  broader-coverage layer used instead**, since its mention-volume behavior
  is actually a decent proxy for exactly the kind of military-operation
  news a DoD release would have covered.

#### Lower precision, broader coverage: GDELT (corroboration, not an official timestamp)

GDELT (the [GDELT DOC 2.0 API](https://api.gdeltproject.org/api/v2/doc/doc))
is a global news-monitoring index -- fundamentally different from the two
sources above. It doesn't give you one dated official release; it gives
you *how much and how early* a topic was being reported in the news, which
is useful when there's no official release to find at all (common for
military actions, where OSINT/press coverage breaks the story well before
-- or instead of -- any government statement). For each market, one GDELT
query is built from that market's own title + resolution criteria (see
`matcher.extract_search_terms` -- simple, dependency-free keyword
extraction: capitalized words in the title first, on the theory that
they're likely countries/people/orgs, then the most frequent remaining
significant words) and run over a window of that market's resolution date
+/- `--window-days`. A market with at least one matching article gets one
aggregate candidate row with:
- `gdelt_mention_count` -- number of distinct matching articles (capped at
  250, GDELT's hard per-query limit -- confirmed live: asking for more
  doesn't error, it returns HTTP 200 with a **plain-text** body instead of
  JSON, which this detects and logs rather than crashing on; a count
  sitting at exactly 250 is a floor, not an exact total, and is logged as
  such)
- `gdelt_earliest_mention_ts` -- timestamp of the earliest matching article
- `gdelt_top_source_domains` -- up to 5 most-frequent reporting domains,
  for manual spot-checking (the candidates CSV's `url` column for a GDELT
  row is a link to GDELT's own HTML results page for that exact query, so
  you can open it and look)

**GDELT's rate limit needed to be more conservative than documented.**
GDELT's 429 response states "please limit requests to one every 5
seconds," but confirmed live during development: sustained querying (this
pipeline queries once per market, in a loop -- exactly that pattern) kept
drawing 429s even with a strict 5-second gap, and stayed rate-limited for
minutes afterward -- consistent with a sustained-use penalty on top of the
flat per-request interval, not just the interval alone. The fetcher uses
double GDELT's stated minimum (10s) between requests and a patient retry
budget (up to 10 attempts, capped backoff) to ride out a transient
cooldown rather than failing that market's query outright. **Expect a full
`fetch-official` run with GDELT included to be slow** -- roughly
`10 seconds x number of markets` as a floor, more if you hit 429s. This is
logged up front so it's not a silent surprise.

Because GDELT candidates are query-built from one specific market, they
carry that `market_id` directly in the candidates CSV, unlike whitehouse.
gov/state.gov rows (which are scraped once across the whole date range and
only matched to a market afterward, by date proximity) -- see
`matcher.candidates_for_market` for why that split matters: without it, a
GDELT hit for one market could wrongly appear to corroborate a different
market that merely happened to resolve on a nearby date.

#### The manual review step

`review` (or `python build_dataset.py review --market-id <id>` for just
one) prints, for every market: its resolution criteria text, then any
whitehouse.gov/state.gov candidates (labeled high precision) and any GDELT
corroboration (labeled lower precision, explicitly flagged as "use only if
no official candidate fits") within `--window-days` of its resolution
date. **This step does not guess for you** -- automated fuzzy-matching
release text against resolution criteria was deliberately left out because
it's unreliable and would silently poison the dataset, and GDELT's mention
count/domain list is corroboration, not a proposed timestamp, for the same
reason. Read the printed criteria, read the candidate list, and if a
whitehouse.gov/state.gov candidate is the real trigger, open
`data/manual_review.csv` and fill in that row:

```
market_id,official_source_url,official_timestamp,notes
12345,https://www.state.gov/releases/.../,2024-03-31T14:32:00Z,"matches criteria exactly"
```

`official_timestamp` must be something `dateutil.parser.parse` understands
(ISO-8601 is safest). If no official candidate matches (site was blocked,
event predates the release archives, resolved on breaking-news consensus
rather than one official release, etc.), leave the row blank -- if GDELT
found corroboration for that market, Part 4 automatically falls back to
`gdelt_earliest_mention_ts` as a lower-precision estimate (see below); if
it didn't either, the market still appears in the final dataset, just with
no computed lag (see below). `data/manual_review.csv` is safe to re-run
`review`/`fetch-markets` against later: existing rows you've filled in are
never overwritten, only new market_ids get appended.

### 4. Build the final dataset

```
python build_dataset.py build --output data/calibration_dataset.csv
```

Joins markets + reactions + your manual review CSV + GDELT candidates into
one row per market, and decides each row's precision tier: a confirmed
`official_timestamp` is always used as T-zero when present; only when
there isn't one does a row fall back to `gdelt_earliest_mention_ts`. Which
one actually got used is recorded explicitly in `timestamp_source`, so
downstream analysis can filter to `official`-only for a clean high-trust
set, or include `gdelt_estimate` rows too for more volume at lower
precision -- rather than the dataset silently mixing the two.

## Output schema (`data/calibration_dataset.csv`)

| Column | Meaning |
|---|---|
| `market_id`, `condition_id`, `event_id`, `event_slug` | Polymarket identifiers |
| `title` | market question |
| `tags` | `\|`-separated Gamma API tags on the parent event |
| `resolution_source_field` | Polymarket's structured `resolutionSource` field (often empty, see above) |
| `resolution_criteria_text` | the market/event description -- the real resolution criteria to read |
| `volume`, `start_date`, `end_date`, `closed_time` | market metadata |
| `official_timestamp` | human-confirmed official announcement time, from `manual_review.csv` -- high precision |
| `official_timestamp_status` | `confirmed` or `pending_manual_review` -- rows are included either way, per the incremental-usability requirement |
| `official_source_url`, `official_notes` | from `manual_review.csv` |
| `gdelt_mention_count` | distinct GDELT-indexed articles found for this market (capped at 250, see above) |
| `gdelt_earliest_mention_ts` | earliest of those articles' timestamps -- lower precision, a corroboration proxy, not an official time |
| `gdelt_top_source_domains` | up to 5 most-frequent reporting domains, `\|`-separated, for manual spot-checking |
| `timestamp_source` | which T-zero `lag_seconds` was actually computed against: `official` (high precision), `gdelt_estimate` (fallback, lower precision), or `none` (neither available) -- **filter/weight downstream analysis by this column, don't treat all rows as equally trustworthy** |
| `market_reaction_timestamp` | unix seconds; earlier of the price-move/volume-spike candidates from step 2 |
| `reaction_method` | `price_move`, `volume_spike`, `price_move+volume_spike`, or `none` |
| `price_points_count`, `trades_count` | how much history was actually available for that market (0 is a real, meaningful value -- check `reaction_notes` if so) |
| `reaction_notes` | e.g. "no trades returned", "missing or invalid start/end date" |
| `lag_seconds` | `market_reaction_timestamp - <the timestamp_source T-zero>`; null when `timestamp_source == none`. Negative means the market moved *before* T-zero (worth double-checking, not necessarily wrong -- news, including GDELT-indexed reporting, often leaks into markets before any formal release) |

**A large-magnitude `lag_seconds` (days, not minutes) on a `gdelt_estimate`
row is a modeling-window mismatch, not a real finding -- treat it as such.**
GDELT's query window is centered on the market's *resolution* date, same as
whitehouse.gov/state.gov candidates, on the assumption that the informative
event happens close to when the market resolves. For a long-lived market
that actually reacted (Part 2) much earlier in its life and only formally
resolved later, `gdelt_earliest_mention_ts` isn't anywhere near the real
trigger -- confirmed in testing, where a market open for months produced a
multi-week `lag_seconds` this way. `official` rows don't have this problem
(a human picked the actual matching release, not a date-window guess), so
this is specifically a `gdelt_estimate`-tier caveat -- sanity-check any
`gdelt_estimate` row with an implausibly large `|lag_seconds|` before
trusting it, or widen/adjust `--window-days` per-market if you're chasing a
market you know is like this.

Rows with `official_timestamp_status == pending_manual_review` still get a
`lag_seconds` if GDELT found corroboration for them (`timestamp_source ==
gdelt_estimate`) -- only rows with `timestamp_source == none` have a null
lag. Filter on `official_timestamp_status` and/or `timestamp_source`
depending on what you're doing with the dataset next.

## Rate limits and politeness

- Polymarket APIs: a minimum delay between requests plus exponential
  backoff, capped at `max_backoff_seconds`, on HTTP 429
  (`common.RateLimitedSession`).
- Government sites: robots.txt is checked via `urllib.robotparser` before
  each source's first request; any crawl-delay it specifies is applied
  (state.gov asks for 5s). If robots.txt itself can't be fetched, that's
  logged and the fetch proceeds cautiously rather than either silently
  skipping or silently ignoring the question.
- GDELT: needed a more conservative delay than its own documented limit in
  practice -- see the GDELT section above. Expect `fetch-official` with
  GDELT included to take a while on a real-sized markets CSV; it logs an
  estimate up front.

## Files

- `common.py` -- shared logging setup + rate-limited HTTP session.
- `polymarket_fetch.py` -- Parts 1 and 2 (Gamma/CLOB/Data API).
- `official_sources.py` -- Part 3 fetchers (whitehouse.gov, state.gov, GDELT; defense.gov present but not default).
- `matcher.py` -- Part 3 candidate filtering + manual-review CSV read/write/print.
- `build_dataset.py` -- CLI entrypoint for all of the above, plus Part 4's join.
