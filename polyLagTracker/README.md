# poly_lag_tracker

Long-running C++ tool that measures the real, steady-state lag between a
Polymarket trade's true on-chain execution time and the moment it's
delivered over Polymarket's CLOB market WebSocket. This is measurement
infrastructure for an insider-trading-detection project: the output is
meant to become the baseline correction factor for all downstream timing
analysis, not a one-shot benchmark like `polyAreaTesting` (which measures
network/region latency, not ingestion lag against ground truth).

It reuses `polyAreaTesting`'s libcurl + IXWebSocket + CMake `FetchContent`
approach, but is architecturally a different kind of tool: one persistent
WS connection instead of fresh-connect-per-sample, a Polygon RPC client for
ground truth, and a crash-safe incrementally-flushed CSV instead of an
end-of-run summary.

## Confirmed live payload schema (read this before changing --match-mode)

This was empirically verified against Polymarket's live market-channel feed
while building this tool (see `--dump-raw-messages` below to reproduce):

- The channel carries three message shapes: `event_type: "book"` (full
  snapshot, pushed once per subscribed asset immediately on subscribe --
  sometimes several of these arrive batched together as a **top-level JSON
  array** in one WS frame), `event_type: "price_change"` (incremental book
  deltas, with per-entry data nested under a `price_changes` array), and
  `event_type: "last_trade_price"` (an actual trade fill).
- **Only `last_trade_price` messages are real trade fills**, and they
  **do carry a genuine on-chain `transaction_hash` field** directly in the
  payload (a real 32-byte tx hash, not to be confused with the shorter
  ~20-byte internal `hash` field present on `book`/`price_change` messages,
  which is an orderbook/state hash and NOT a transaction hash).
- Because of this, **tx-hash matching is reliable and is the default**
  (`match_mode = txhash`). The fuzzy log-scan path exists as a fallback for
  the (currently unobserved) case where a trade fill lacks a tx hash, and
  ships unconfigured on purpose -- see "Fuzzy log-scan matching" below.

If Polymarket changes this schema, re-verify with `--dump-raw-messages`
before trusting any lag numbers -- don't assume the above stays true
forever.

## What it does

1. Opens **one persistent** WebSocket connection to Polymarket's CLOB
   market channel and subscribes to a configured list of asset/token ids.
   IXWebSocket's built-in automatic reconnection (exponential backoff
   between `reconnect_min_backoff_ms`/`reconnect_max_backoff_ms`) handles
   drops; every connect/disconnect is logged with a timestamp, and the
   subscribe message is re-sent on every reconnect.
2. On every trade-shaped message, immediately captures both
   `std::chrono::steady_clock` (monotonic) and `std::chrono::system_clock`
   (wall-clock) receipt time, before any JSON parsing -- this is the "WS
   receipt time" half of the lag measurement.
3. Hands the parsed trade off to a worker-thread pool, which queries a
   Polygon JSON-RPC endpoint for the transaction's block, polling with a
   short retry/backoff since a trade can be pushed over WS slightly before
   (or, as observed live, sometimes clearly before) its settlement
   transaction is actually mined.
4. Computes `lag_ms = ws_receipt_wall_clock_ms - block_timestamp_ms` and
   appends one row per trade to a CSV, flushed immediately so a crash loses
   at most the in-flight row.
5. Also decodes the trader's wallet address for each trade directly from
   the CTF Exchange V2 contract's `OrderFilled` event log in that same
   transaction receipt (see "Wallet address resolution" below) -- the WS
   payload itself doesn't carry one, and this needs no separate API call.
6. For each trade with a resolved wallet, looks up (or backfills, on first
   sight) that wallet's trading history and computes a fast Stage 1
   anomaly score against it (see "Wallet history store" and "Anomaly
   scoring" below) -- deliberately simple, wallet-level-only scoring;
   expensive funding-graph tracing is a planned Stage 2, out of scope here.
7. Logs connection health, message throughput, running lag stats
   (min/median/max so far), wallet-resolution counts, and Stage 1
   cache-hit/flagged counts on a configurable interval.

## Build

Dependencies: CMake >= 3.15, a C++17 compiler, libcurl + OpenSSL + SQLite3
dev headers, and internet access at **configure time** to fetch IXWebSocket
and nlohmann/json via `FetchContent` (same rationale as `polyAreaTesting`:
not reliably packaged at a fixed version via apt/brew).

```
sudo apt update && sudo apt install -y build-essential cmake libcurl4-openssl-dev libssl-dev libsqlite3-dev chrony
cd polyLagTracker
mkdir -p build && cd build
cmake ..
cmake --build . -j
```

Produces `./poly_lag_tracker`. `chrony` is not a build dependency of the
binary itself, but install and enable it (`sudo systemctl enable --now
chronyd`) on the deployment host -- the startup NTP check (below) expects
`chronyc` to exist.

## Before your first real run: dump raw payloads

```
./poly_lag_tracker --auto-discover-assets --dump-raw-messages 50
```

One command: picks currently active markets (see "Auto-discovering active
markets" below), connects, subscribes, writes 50 raw WS payloads to
`raw_messages.jsonl` (one JSON value per line -- some lines are arrays, see
above), then exits. No RPC calls, no CSV output, no periodic refresh (dump
mode is short-lived by design). Use this after any deploy to a new host or
after a gap in usage to reconfirm the schema notes above still hold before
trusting `--match-mode auto`/`txhash` output.

If you'd rather pin a specific market instead of letting discovery pick,
pass `--asset-ids <id1>,<id2>,...` instead (get live ids from
`https://data-api.polymarket.com/trades?limit=20`, field `"asset"`) --
`--asset-ids` always wins over `--auto-discover-assets` if both are given.

## Run

```
./poly_lag_tracker --config ../config/example.ini \
  --auto-discover-assets \
  --rpc-url https://your-polygon-rpc-endpoint
```

A config file and CLI flags can be mixed; CLI flags override the config
file. See `./poly_lag_tracker --help` for the full flag list, or
`config/example.ini` for every field with inline comments. Key ones:

| Flag | What |
|---|---|
| `--auto-discover-assets` | pick the subscription set from currently active trades, by volume (see below) |
| `--discover-by-category` | pick the subscription set from open markets under `--discover-tags`, regardless of volume (see "Category-based market discovery" below; mutually exclusive with `--auto-discover-assets`) |
| `--discover-tags` | comma-separated Gamma API tag slugs for `--discover-by-category` (default: a confirmed-live politics/geopolitics/military/elections/regulatory set) |
| `--asset-ids` | comma-separated token ids to subscribe to instead of discovering them; wins if set alongside either discovery mode |
| `--rpc-url` | Polygon JSON-RPC endpoint (required unless `--match-mode off`) |
| `--match-mode` | `auto`\|`txhash` (default)\|`fuzzy`\|`off` |
| `--output` | CSV path (default `poly_lag.csv`) |
| `--log-interval-sec` | periodic stats log cadence (default 300 = 5 min) |
| `--worker-threads` | RPC-resolution workers (default 4) |
| `--force-unsynced-clock` | override the NTP gate (not recommended) |
| `--wallet-history-db` | SQLite cache path for wallet history/anomaly scoring (default `wallet_history.db`) |
| `--anomaly-score-flag-threshold` | combined Stage 1 score at/above which a trade is flagged (default `0.7`) |

### Auto-discovering active markets

Polymarket's highest-volume markets (e.g. the BTC/ETH/XRP 5-minute up/down
markets) churn and get replaced every few minutes -- confirmed live while
testing this: an asset id sampled from recent trades can return an empty
order book within about a minute as its market resolves and a new one takes
its place. A hand-curated `--asset-ids` list goes stale fast, and there's
no way to know in advance which markets will actually be active. With
`--auto-discover-assets`:

- At startup, the tool samples the last `--discover-trade-sample-size`
  (default 500) trades from `https://data-api.polymarket.com/trades`,
  counts how often each asset id appears (a proxy for current activity),
  and subscribes to the top `--discover-top-n` (default 40). Exactly which
  ids were picked, and their trade counts in the sample, are logged so a
  run is reproducible/debuggable afterward.
- Every `--discover-refresh-interval-sec` (default 1800 = 30 min, `0`
  disables this), it re-samples, diffs the new top-N against what's
  currently subscribed, and logs every asset added ("newly active") or
  dropped ("aged out of top-N"). **Confirmed empirically that Polymarket's
  market-channel WS rejects a second subscribe message on an already-open
  connection** (the server replies the plain-text string
  `INVALID OPERATION`, even for an identical resubscribe) -- there is no
  live add/remove, so when the set actually changes, the tool closes and
  reopens the WS connection with the updated list rather than trying to
  patch the existing one. This briefly interrupts the trade stream (a
  normal reconnect, logged like any other), which is preferable to running
  with a silently stale subscription.
- If the discovery HTTP call fails outright (network error, bad JSON, no
  trades in the sample) at startup, the tool logs the error and exits
  non-zero rather than silently starting with an empty subscription --
  there's one retry after a short delay first. A periodic *refresh* that
  fails is treated less strictly: it's logged and skipped, keeping the
  current subscription, since a multi-day unattended run shouldn't die
  over one transient HTTP hiccup hours in.
- `--asset-ids`, if also given, always wins (auto-discovery is skipped
  entirely, with a warning) -- use it when you want a fixed, reproducible
  market set instead of a moving one.

Runs until SIGINT/SIGTERM, which triggers a graceful shutdown: the WS
stops, in-flight RPC resolutions get a chance to finish (or bail out
cleanly if already mid-poll), and the CSV is already flushed row-by-row so
there's nothing left to lose.

### Category-based market discovery

`--discover-by-category` is an alternative to `--auto-discover-assets`
above (mutually exclusive -- passing both is a startup error), not a
replacement. Volume-based discovery is still the right tool for the job it
was built for: generating high trade volume to measure lag statistics
reliably. It is the *wrong* tool for this project's current purpose,
wallet-level anomaly detection -- **informed trading is only structurally
possible on markets resolved by a real-world decision** (political,
geopolitical, regulatory, and similar), not on high-frequency speculative
markets like the 5-minute crypto up/down contracts that dominate trade
volume and have no possibility of genuine insider information. The Stage 1
sanity check (see "Wallet frequency segmentation" above) found this
directly: volume-based discovery left low-frequency, human-scale wallets
-- the population this detector is actually meant to catch -- at just
**2.4%** of the watched population.

This mode instead subscribes to **every currently open market** under a
configured set of Gamma API tag slugs, regardless of trade volume, via
`GET /events?tag_slug=<slug>&closed=false` (the same tag-slug approach
`polyEventCalibration/` proved out for offline calibration data -- see
that project's `polymarket_fetch.py` docstring). Two things confirmed live
while building this, not assumed:

- **Tag taxonomy**: Polymarket has no single clean "category" field --
  `/tags` is the real, usable taxonomy, and slugs have to be checked
  individually. `--discover-tags` (comma-separated, default below) was
  confirmed live against the real taxonomy at gamma-api.polymarket.com,
  covering politics, geopolitics, military, elections, and regulatory/
  monetary-policy categories:
  ```
  politics, elections, geopolitics, military, military-strikes, nato,
  tariffs, supreme-court, congress, fomc, interest-rates, monetary-policy,
  government-shutdown, regulation, fda, sec
  ```
  (`military-invasion`, `war`, `international-relations`,
  `department-of-defense`, `regulatory`, `antitrust`, `sanctions`,
  `senate`, `trade-policy` were also checked live -- all either don't
  exist as slugs or currently have zero open events; not included in the
  default since an empty tag contributes nothing, but harmless to add via
  `--discover-tags` if that changes.)
- **An event's own `closed=false` does NOT mean every market inside it is
  still open.** A multi-outcome event can stay "open" overall while
  individual markets within it have already resolved -- confirmed live by
  inspecting a real event where the top-level `closed` was `false` but its
  nested market's `closed` was `true`. This tool filters at the **market**
  level, not just the event level, for exactly that reason.

**This can be a genuinely large number of markets -- confirmed live, not
assumed.** A test run against the default tag set found **1,840 unique
open events, 19,056 open markets, 38,096 individual outcome tokens** to
subscribe to (each binary market has 2 outcome tokens -- Yes/No -- both
subscribed, so trades on either side are captured; NegRisk multi-outcome
markets contribute more). That's ~950x the ~40 assets volume-based
discovery typically subscribes to. Above 1,000 outcome tokens, the tool
logs a loud warning (`discover-by-category: N outcome token(s)... well
above the 1,000 token soft threshold`) rather than silently proceeding --
but it still **attempts the full list**, since silently truncating would
defeat the entire point of this mode (watching every low-volume market a
topic covers, not just the loudest ones).

**A sustained 1-hour live test at this scale (38,096 subscribed tokens)
found a real, specific cost, not a hypothetical one**: the WS server
responded to the subscribe with repeated close code `1013 "slow consumer:
send buffer full"`, producing bursts of 10-18 rapid reconnect cycles
within a few seconds -- once immediately on the initial connection, once
more about 6.5 minutes in. IXWebSocket's auto-reconnect recovered both
times without intervention, and after roughly minute 7 the connection was
completely stable for the remaining 53 minutes (1 further disconnect,
total). Trade capture and Stage 1 scoring were unaffected throughout --
`queue_depth` stayed in the 0-3 range and `queue_dropped` stayed at 0 for
the full hour -- so this is a startup-phase cost to budget for
(expect some churn and possibly-delayed data in the first several minutes
after (re)connecting at this scale) rather than a sustained throughput
problem. Watch `queue_depth` and `connects`/`disconnects` in the health
log after startup the same way you would for RPC throughput.

That same 1-hour run confirmed the population-imbalance fix works: Low-
frequency wallets were **17.95%** of the watched population (158 of 880
distinct cached wallets), vs. **2.4%** for a volume-based run -- roughly a
**7.5x** increase in exactly the population this detector exists to
catch, and Stage 1's `age_score`/`concentration_score` distributions
stayed spread out (not compressed near the ceiling) across a much larger
scored sample (234 trades) than the shorter segmentation-only
re-verification run managed. All 10 trades flagged during that hour were
on genuinely political/geopolitical/regulatory markets (Fed rate
decisions, a US-Iran ceasefire market, a UK by-election, a congressional
race), each wallet's own real, isolated size outlier -- none looked like a
backfill-cap artifact.

Refresh follows the same reconnect-based pattern as volume-based discovery
above (Polymarket rejects a second subscribe on an open connection,
confirmed there) -- but category-resolved markets change composition far
less often than 5-minute crypto markets, so `--discover-refresh-interval-sec`
defaults to **14,400s (4h)** for this mode instead of volume-mode's 1,800s
(30min), automatically, unless you pass the flag explicitly (logged either
way at startup).

### NTP clock sync gate

Every lag number this tool produces is only meaningful if the local system
clock is genuinely NTP-synced -- a clock offset is indistinguishable from
real ingestion lag in the output. On startup the tool shells out to
`chronyc tracking` (falling back to `ntpstat` if chrony isn't installed)
and **refuses to start** if the clock isn't confirmed synced, unless you
pass `--force-unsynced-clock`. Fix this on the host
(`sudo systemctl enable --now chronyd`, then check `chronyc tracking` shows
`Leap status : Normal`) before collecting data you intend to use for real
analysis. **This must be resolved before trusting any lag numbers.**

### Fuzzy log-scan matching (opt-in, unverified, off by default)

`--match-mode fuzzy` (or `auto`, which falls back to it when a trade lacks
a tx hash) scans `eth_getLogs` for an `OrderFilled`-shaped event on a
configured exchange contract, in a block range estimated from the trade's
receipt wall-clock time, and picks the closest price/size match within
tolerance. This tool **deliberately ships with no hardcoded contract
address or event topic hash** for this path -- both
`exchange_contract_address` and `order_filled_topic0` must be set by hand
(confirm them against Polygonscan/Polymarket's own contract docs
yourself), and `--match-mode fuzzy` fails fast at startup if they aren't.

Given the live schema confirmation above (trade fills already carry a real
tx hash), this path shouldn't be needed in practice -- treat it as a
fallback for a schema change, not a routine mode. It also does **not**
verify market/asset identity when picking the winning transaction out of a
block range (decoding which token id was traded requires a bignum-correct
comparison, which the *wallet* decoder below does, but this path's own
price/size scoring doesn't cross-check it against the winning log), only
price/size/time proximity. Rows resolved this way carry
`match_method = fuzzy_log_scan` in the CSV specifically so you can filter
them out (or weight them lower) in downstream analysis.

### Wallet address resolution

Polymarket's WS trade payload does **not** carry the trader's wallet
address at all -- there's no field for it anywhere in a `last_trade_price`
message, and the raw on-chain transaction's own `from`/`to` don't help
either, since Polymarket executes trades through a relayer/proxy-wallet
architecture where those fields reflect a shared operator address, not the
end user.

This tool decodes it a different way instead: the CTF Exchange V2 contract
on Polygon (`0xe111180000d2663c0091e4f400237545b87b996b`) emits an
`OrderFilled(bytes32 orderHash, address maker, address taker, uint8 side,
uint256 tokenId, uint256 makerAmountFilled, uint256 takerAmountFilled,
uint256 fee, bytes32 builder, bytes32 metadata)` event for every fill, and
despite the relayer architecture above, **its `maker`/`taker` fields are
the real trading wallets** -- confirmed by decoding this event from the
same transaction receipt already fetched for on-chain confirmation and
comparing the result against 83 known-correct `(tx_hash, proxyWallet)`
pairs pulled from a live capture (back when this tool still used an
earlier Data-API-based lookup): **83/83 correct, 0 wrong, 0 unresolved**,
before that lookup was replaced with this. The event signature, the
contract address, and the maker/taker semantics were all confirmed
directly from `Polymarket/ctf-exchange-v2`'s own Solidity source
(`Events.sol`/`Trading.sol`/`Structs.sol`), not guessed.

Two things make this more than "read `maker`/`taker` off the log":
- **A single settlement transaction can batch several unrelated fills**
  into multiple `OrderFilled` logs (Polymarket batches matched orders for
  gas efficiency), so the right log has to be picked out by exact `tokenId`
  match plus price/size within tolerance against the WS-captured trade --
  not just "the first `OrderFilled` log in the tx" or "any log mentioning
  this address," either of which can silently pick the wrong fill when a
  wallet happens to appear in more than one log in the same tx.
- **The exchange also emits a redundant "mirror" log against itself** for
  NegRisk mint/merge fills (`taker == the exchange contract's own
  address`), re-stating the taker's own fill for internal bookkeeping. No
  special-casing for this turned out to be necessary: comparing the
  WS-captured `side` (BUY/SELL) to the log's decoded `side` (always the
  *maker* order's side, per `Structs.sol`) and picking `maker` when they
  match or `taker` when they don't resolves the correct wallet either way,
  mirror log or not.

This runs automatically wherever a transaction receipt (or, in
`--match-mode fuzzy`, a matched block-range log) is already available --
there's nothing to configure, retry, or opt out of. Unresolved trades are
still written with an empty wallet field and a specific
`wallet_resolve_note` (e.g. `no OrderFilled log matched this trade's
tokenId+price/size`, or `ambiguous: 2 distinct wallets resolved across 2
matching log(s)` if the tokenId+price/size match itself turned out
ambiguous) rather than being dropped, per the tool's existing philosophy of
never discarding a captured trade over a resolution failure.

### Wallet history store

Confirmed live before this was built: Polymarket's Data API supports
filtering trades by wallet at `GET /trades?user=<0x-address>`, standard
`offset`+`limit` pagination, no meaningful page-size cap found (tested up
to 5000 in one call against a real high-volume wallet). A genuinely
never-traded, well-formed address cleanly returns `[]` -- a real, honest
"zero history" result, not an error.

**Two gotchas found while confirming this, both guarded against in code**
(`wallet_history_fetch.cpp`): `?wallet=` (the other plausible param name)
is **not** valid -- it's silently ignored and falls back to returning
generic, unfiltered recent-trades data from unrelated wallets. Worse, *any*
malformed `user` value (wrong length, non-hex) triggers the exact same
silent fallback instead of an error or empty array. So this tool (a)
rejects a wallet address up front unless it's exactly `0x` + 40 hex chars,
and (b) independently checks every returned trade's own `proxyWallet`
actually matches the query, discarding (and logging) any row where it
doesn't. Skipping either check would risk silently attributing a random
wallet's trading history to the wrong address in the cache.

When a trade's wallet isn't yet cached, this tool pages through that
endpoint once to backfill the wallet's full history (bounded: 20 pages of
500 = 10,000 trades max per wallet, a deliberate cap not an unbounded
fetch), extracting total trade count, earliest trade timestamp (an
account-age proxy), a running mean/stddev of trade size (via [Welford's
online algorithm](https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm),
not stored per-trade), and the set of market categories traded in. Every
subsequent trade from an already-cached wallet updates that same record
incrementally instead of re-fetching. All of this lives in a small SQLite
database (`--wallet-history-db`, default `wallet_history.db`) -- one
connection, one mutex, deliberately not a connection pool or WAL-tuned,
since this tool's trade volume doesn't need either and a single lock is
trivial to reason about.

Market category/tag isn't on the Data API's trade objects at all --
resolving it needs a separate two-hop Gamma API lookup (`conditionId` ->
`/markets?condition_ids=` -> embedded event id -> `/events?id=` -> `tags[]`,
using the tag with the lowest numeric id as the "primary" category, a
documented heuristic confirmed against real markets, not guessed) that is
cached forever per market once resolved.

**Important, confirmed via live load-testing, not assumed**: that category
lookup reliably succeeds for currently-live markets but returns nothing for
markets that have already resolved/expired and been pruned from Gamma's
index. A first implementation did this lookup for every trade encountered
during a wallet's historical backfill -- since most of a wallet's *past*
trades are for by-then-expired markets, this made the vast majority of
backfill category lookups fail while still paying the full network
round-trip cost for each one, which backed up the whole worker pool's
trade queue badly (observed live: queue depth climbing from ~100 to ~600
over 4 minutes with barely any trades actually finishing processing).
**Fixed** by only ever doing the network category lookup for a wallet's
current, just-arrived trade (whose market is virtually certain to still be
live); historical trades during backfill use a cache-only lookup and are
recorded as `unknown` category if not already cached, no network call. A
live re-test after this fix processed the same feed with queue depth
staying flat/near-zero throughout -- see "Testing" below for the numbers.

Two further limitations worth knowing about before trusting the age
signal from this cache, both observed directly during load-testing rather
than theorized:
- **The `maxPages` backfill bound (10,000 trades) biases the age signal
  for extremely high-frequency wallets.** A bot trading roughly once per
  second will have already produced 10,000 trades within about 2.8 hours,
  so "the earliest trade this cache actually saw" for such a wallet isn't
  its true account age -- it's just how far back the bounded backfill
  reached. This showed up live as several very-high-volume wallets scoring
  a near-maximal age_score despite clearly not being new accounts.
- **The get-history / fold-in / persist sequence isn't atomic across
  worker threads.** Two trades from the same wallet processed
  concurrently by different workers can race, and one update can be lost
  (observed live: one high-frequency wallet's cached `trade_count` was
  seen going into two rows built from the same prior count, one second
  apart, on what were evidently two different worker threads). Given this
  is exploratory Stage 1 scoring meant to be tuned against real data
  rather than a ledger of record, that's an accepted simplicity trade-off
  for now, not fixed with per-wallet locking in this version.

### Wallet frequency segmentation

A manual sanity check of an earlier, unsegmented version of the scorer
below found that `anomaly_age_score` and `anomaly_concentration_score`
were both compressed near their ceiling for the large majority of scored
trades -- not because most wallets are genuinely new or narrowly focused,
but because the scored population mixed two structurally different kinds
of wallet:

- **low-frequency, human-scale wallets** (a handful of trades) -- the
  shape of the known real insider-trading cases in this project's own FFIC
  reference data: a small number of unusually large, well-timed trades,
  not sustained activity.
- **high-frequency, likely-automated/market-maker wallets** (thousands of
  trades), which routinely hit `wallet_history_fetch.hpp`'s `maxPages`
  backfill cap (10,000 trades). Hitting that cap makes an extremely
  established, obviously-not-new wallet look artificially "new" to
  `age_score` -- its cached `first_seen_unix` just reflects "however far
  back the capped backfill reached", not the wallet's true first trade --
  the exact opposite of what `age_score` is meant to signal.

Rather than trying to fix `age_score`/`concentration_score`'s formulas so
they somehow work for both populations at once (they weren't touched --
see below), the scorer now classifies every wallet into a frequency tier
**before** any scoring happens (`wallet_segment.hpp/cpp`), using its
cached `trade_count` and trades/day since first seen (trades/day, not raw
count alone, since 500 trades over 2 years and 500 trades in 2 days are
very different wallets):

| Tier | Condition | CLI flag (default) |
|---|---|---|
| **High** | `trade_count` ≥ threshold, or trades/day ≥ threshold | `--segment-high-min-trades` (2000), `--segment-high-min-trades-per-day` (50.0) |
| **Low** | `trade_count` ≤ threshold **and** trades/day ≤ threshold | `--segment-low-max-trades` (50), `--segment-low-max-trades-per-day` (5.0) |
| **Medium** | everything else | -- |

Trades/day is computed over an elapsed-time denominator floored at 1 day,
so a wallet with e.g. 3 trades within the same minute doesn't get
extrapolated into an astronomical rate; a backfill-cap-truncated
`first_seen_unix` (see above) only ever makes the trades/day estimate
*larger* than the true rate, never smaller, so that bias pushes
classification in the safe direction (toward High), not away from it. The
High check on raw `trade_count` alone also means any wallet that hit the
backfill cap is High regardless of the rate estimate.

These defaults aren't arbitrary -- they were confirmed against a live
run's tier distribution (see "Testing" below for the actual numbers, both
per-trade and a true unique-wallet census from the SQLite cache) before
being kept. **Confirmed live: on the currently-highest-volume markets this
tool's `--auto-discover-assets` mode subscribes to, the population is
heavily skewed toward High/Medium** (bots and market-makers dominate raw
trade volume, by construction) -- Low-tier wallets are a small minority of
what's captured in any given run there, which matches the intuition that
genuine human-scale/insider-shaped activity is rare, not a sign the
thresholds are miscalibrated. A broader, lower-volume asset subscription
would likely surface proportionally more Low-tier wallets.

### Anomaly scoring (Stage 1) -- Low-frequency wallets only

A fast, cheap, wallet-level suspicion score, entirely from data already in
the local cache above -- no additional network call happens at scoring
time itself. Deliberately simple and fully documented (`anomaly_score.cpp`,
**unchanged** by the segmentation work above) rather than a trained model,
since it's meant to be read, understood, and hand-tuned against real data,
not treated as a black box.

**This only runs for wallets classified Low-frequency** (see "Wallet
frequency segmentation" above). Medium and High-frequency wallets are
excluded from this component entirely -- see "Excluded populations" below
for why and what they get instead. Three components, each a value in
`[0, 1]`:

- **`anomaly_size_score`** -- how large this trade is relative to the
  wallet's own prior typical size, as a one-sided z-score (only unusually
  *large* trades count) capped at 4 standard deviations. With fewer than 2
  prior trades there's no real stddev to compare against, so this falls
  back to treating 10% of the prior mean as a stand-in "typical spread" --
  and with zero prior trades (mean 0 too), that fallback collapses toward
  zero, so essentially any nonzero first trade reads as maximally large.
  That's a deliberate, documented consequence of the formula, not a hidden
  special case.
- **`anomaly_age_score`** -- how new the wallet is, linearly scaled from 1.0
  at age 0 down to 0.0 at 30 days since its earliest known trade (see the
  `maxPages` bias noted above for high-frequency wallets specifically).
- **`anomaly_concentration_score`** -- a
  [Herfindahl-Hirschman Index](https://en.wikipedia.org/wiki/Herfindahl%E2%80%93Hirschman_Index)
  over the wallet's category history *including this trade*, standard
  concentration-measure math: ranges from near 0 (spread evenly across
  many categories) to 1 (all trades, this one included, in a single
  category).

`anomaly_total_score` is a simple, equally-weighted average of the three
-- not validated against labeled data, a starting point for tuning. A
trade at or above `--anomaly-score-flag-threshold` (default `0.7`) is
marked `anomaly_flagged=true` and logged distinctly
(`stage1_flagged=...` in the periodic health log).

**Stage 2 hook (scaffold only, not implemented)**: every trade that
crosses the flag threshold calls `onWalletFlagged(wallet_address, score)`
(`stage2_hook.hpp/cpp`), currently just a placeholder log line. This is
the intended integration point for a future, deliberately out-of-scope
Stage 2: expensive funding-graph tracing (where a flagged wallet's
collateral came from, whether it connects to other flagged wallets, etc.)
that should only ever run for the small subset of wallets Stage 1 flags,
not on every trade.

#### Excluded populations (Medium and High-frequency wallets)

Medium and High-frequency wallets (see "Wallet frequency segmentation"
above) do **not** get the three-component score at all -- not scored
badly, not silently dropped either. Every trade from one is still written
to the CSV with `wallet_frequency_tier` set (`medium`/`high`) and
`anomaly_scope` set to `excluded_medium_frequency` / `excluded_high_frequency`,
with `anomaly_note` explaining why; the score columns themselves are left
empty, matching how an unresolved wallet's row already looks.

**This is intentional, not an oversight, and this component is not the
right tool for that population.** A wallet generating thousands of trades
is a continuous order-flow problem, not a "does this one trade look
unusual against a sparse history" problem -- the literature here is order
flow microstructure measures built for exactly that continuous-activity
setting: **PIN** (probability of informed trading), **VPIN** (its
volume-synchronized variant, suited to fast/bucketed markets like this
one), and **Kyle's lambda** (price-impact-based informed-trading measure).
These are already noted elsewhere in this project's broader detection plan
as the right fit for sustained high-frequency activity; building that
detector is a separate, future component, not attempted here. Stage 1's
job for the High-frequency population, for now, is simply to say so
clearly in the output rather than pretend a scorer built for sparse
human-scale trading applies.

## Output schema

One CSV row per trade fill, header included, UTF-8, RFC4180-ish quoting
(quotes doubled, fields with `,`/`"`/newlines quoted):

| Column | Meaning |
|---|---|
| `recv_wall_iso` | WS receipt time, UTC, ISO-8601 |
| `recv_wall_unix_ms` | same, as epoch milliseconds |
| `day_of_week` | `Mon`..`Sun`, UTC, derived from `recv_wall_iso` |
| `hour_of_day` | `0`-`23`, UTC |
| `market` | condition/market id from the payload |
| `asset_id` | token id from the payload |
| `side` | `BUY`/`SELL` from the payload |
| `price`, `size` | as reported by Polymarket, kept as the original strings |
| `payload_timestamp` | Polymarket's own event timestamp, raw string (note: observed to sometimes be a couple seconds *before* the on-chain block timestamp -- see below) |
| `tx_hash` | resolved transaction hash (from the payload if tx-hash matching, or the matched log's hash if fuzzy) |
| `match_method` | `tx_hash` \| `fuzzy_log_scan` \| `unmatched` |
| `resolved` | `true`/`false` -- whether on-chain data was actually found |
| `block_number`, `block_timestamp_unix` | empty if unresolved |
| `lag_ms` | `recv_wall_unix_ms - block_timestamp_unix*1000`; empty if unresolved. **Can be negative** -- in live testing, Polymarket routinely delivered the WS trade notification 1-3 seconds *before* the settlement transaction's block was mined, i.e. the off-chain match/notify happens before on-chain settlement confirms. That's real signal for the baseline this tool exists to produce, not a bug. |
| `note` | human-readable reason when unresolved (pending confirmation, no fuzzy match in range, etc.) |
| `raw_json` | the verbatim WS payload for this one trade object, for reprocessing if the schema needs revisiting |
| `wallet_address` | trader's wallet, decoded from the `OrderFilled` log's maker/taker field, empty if unresolved -- see "Wallet address resolution" above |
| `wallet_resolved` | `true`/`false` |
| `wallet_resolve_note` | reason when unresolved, or how it was resolved (matching log count) |
| `anomaly_size_score`, `anomaly_age_score`, `anomaly_concentration_score` | Stage 1 component scores, each `[0,1]`; empty if not scored (see `anomaly_note`) -- see "Anomaly scoring (Stage 1)" above |
| `anomaly_total_score` | equally-weighted average of the three components; empty if not scored |
| `anomaly_flagged` | `true`/`false`; empty if not scored |
| `anomaly_note` | why unscored (e.g. wallet not resolved, backfill failed, excluded by tier), or a short diagnostic (prior trade count, resolved category) when scored |
| `wallet_frequency_tier` | `low`/`medium`/`high`, empty if not classified (wallet unresolved) -- see "Wallet frequency segmentation" above |
| `anomaly_scope` | `scored` \| `excluded_medium_frequency` \| `excluded_high_frequency` \| `unscored` \| `wallet_unresolved` -- filter on this column to isolate the population the score columns actually apply to |

The wallet, anomaly-scoring, and segmentation columns were appended at the
end of the pre-existing schema in the order they were added, keeping every
prior column unchanged and in position. If you point `--output` at a CSV
written before these features existed, its older rows won't have these
trailing fields -- start a fresh output file when adopting them rather
than resuming into an old one.

## Notes / limitations

- Running lag stats logged periodically are exact for min/max/count over
  the whole run, but median is approximated from the most recent 50,000
  resolved samples (a rolling window, not the full history) so memory
  stays bounded across a multi-day run. The CSV has every row, so exact
  stats over any period are a `pandas.read_csv` + `describe()` away.
- The bounded trade queue between the WS thread and RPC workers drops the
  oldest queued item (logged) if RPC resolution can't keep up, rather than
  blocking the WS callback thread -- that thread needs to stay responsive
  for reconnects/pings. Watch `queue_dropped` in the periodic health log;
  a nonzero, growing value means `--worker-threads` or `--rpc-timeout-ms`
  need tuning for your trade volume. Wallet resolution no longer adds any
  separate network call or retry budget to this pipeline (it's decoded
  from the receipt already fetched for on-chain confirmation), so it's not
  an independent source of backlog the way the earlier Data-API lookup was.
- A tx pending past `rpc_poll_max_attempts` polls is recorded as
  unresolved (`note` explains why) rather than retried indefinitely or in
  a later pass -- there's no offline reprocessing step in this version.
  `raw_json` and `tx_hash` are kept in the row specifically so one could be
  written later if needed.
- Stage 1 wallet-history backfill (a new wallet's first lookup) does add a
  new source of worker-pool latency: confirmed live that this can back up
  `queue_depth` significantly if the network-fetching category lookup were
  called per historical trade during backfill (see "Wallet history store"
  above for the fix that was needed and applied). After that fix, a
  ~13-minute live test against ~40 currently-active high-volume markets
  kept `queue_depth` flat/near-zero throughout -- but a deployment
  subscribed to a much larger or colder (many first-time-seen wallets)
  asset set should still watch `queue_depth` the same way as for RPC
  resolution above.
- The `maxPages`-bounded backfill (10,000 trades) and the non-atomic
  per-wallet cache update under concurrent workers (see "Wallet history
  store" above) are both known, accepted simplifications for this
  exploratory Stage 1 scoring version, not bugs to be surprised by if
  encountered while tuning against real output.
- Segmentation fixed the specific artifacts a manual sanity check found
  (`age_score`/`concentration_score` clustering near ceiling), confirmed
  live (see "Testing" in the project history / commit notes for the actual
  before/after distributions) -- but a live run against
  `--auto-discover-assets`'s highest-volume market set naturally produces
  very few Low-tier trades to score (Low-tier wallets are a small minority
  of what trades on the busiest markets), so any single run's flagged-trade
  sample is small and conclusions from it should be treated as directional,
  not statistically conclusive. Medium-frequency wallets are excluded from
  this scorer the same as High (see "Excluded populations" above) purely
  because the existing formulas were only confirmed meaningful for the Low
  population specifically -- Medium's own behavior under this scorer
  wasn't separately validated one way or the other, and remains an open
  question for future work rather than a settled "it's fine" or "it's
  broken."
