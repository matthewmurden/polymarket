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
5. Logs connection health, message throughput, and running lag stats
   (min/median/max so far) on a configurable interval.

## Build

Dependencies: CMake >= 3.15, a C++17 compiler, libcurl + OpenSSL dev
headers, and internet access at **configure time** to fetch IXWebSocket and
nlohmann/json via `FetchContent` (same rationale as `polyAreaTesting`: not
reliably packaged at a fixed version via apt/brew).

```
sudo apt update && sudo apt install -y build-essential cmake libcurl4-openssl-dev libssl-dev chrony
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
| `--auto-discover-assets` | pick the subscription set from currently active trades (see below) |
| `--asset-ids` | comma-separated token ids to subscribe to instead of discovering them; wins if both are set |
| `--rpc-url` | Polygon JSON-RPC endpoint (required unless `--match-mode off`) |
| `--match-mode` | `auto`\|`txhash` (default)\|`fuzzy`\|`off` |
| `--output` | CSV path (default `poly_lag.csv`) |
| `--log-interval-sec` | periodic stats log cadence (default 300 = 5 min) |
| `--worker-threads` | RPC-resolution workers (default 4) |
| `--force-unsynced-clock` | override the NTP gate (not recommended) |

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
verify market/asset identity (decoding which token id was traded requires
a bignum-correct comparison this tool doesn't attempt), only price/size/
time proximity, and even the CTF-Exchange `OrderFilled` data-layout
assumption baked into the decoder needs re-verifying against the exact
event signature you configure. Rows resolved this way carry
`match_method = fuzzy_log_scan` in the CSV specifically so you can filter
them out (or weight them lower) in downstream analysis.

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
  need tuning for your trade volume.
- A tx pending past `rpc_poll_max_attempts` polls is recorded as
  unresolved (`note` explains why) rather than retried indefinitely or in
  a later pass -- there's no offline reprocessing step in this version.
  `raw_json` and `tx_hash` are kept in the row specifically so one could be
  written later if needed.
