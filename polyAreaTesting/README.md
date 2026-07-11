# poly_latency_bench

Standalone C++ tool to measure network latency from a given host to
Polymarket's public API surface. This is **not** the trading/detection bot —
it exists purely to compare candidate cloud regions before picking where to
deploy the real service.

## What it measures

| Endpoint | URL | What |
|---|---|---|
| CLOB REST | `GET https://clob.polymarket.com/time` | server time, cheapest possible CLOB call |
| Data API | `GET https://data-api.polymarket.com/trades?limit=1` | latest trade, also used to find a live token id for the WS test |
| Gamma API | `GET https://gamma-api.polymarket.com/events?limit=1` | market/event metadata |
| CLOB WebSocket | `wss://ws-subscriptions-clob.polymarket.com/ws/market` | handshake time, and time from subscribe to first `book` frame |

All four are public, unauthenticated, read-only. No API keys needed.

For the REST endpoints, every sample opens a **fresh connection**
(`CURLOPT_FRESH_CONNECT` + `CURLOPT_FORBID_REUSE`), so DNS/TCP-connect/TLS
are measured on every sample rather than being zeroed out by keep-alive
reuse after the first request. That's the number that matters for region
selection: the cost of a cold connection from that host. Each sample's
total time is broken into phases via libcurl's timing info
(`CURLINFO_NAMELOOKUP_TIME`, `CURLINFO_CONNECT_TIME`,
`CURLINFO_APPCONNECT_TIME`, `CURLINFO_STARTTRANSFER_TIME`,
`CURLINFO_TOTAL_TIME`) instead of one wall-clock number around the call.

For the WebSocket endpoint, every sample is also a fresh connect/subscribe/
disconnect cycle (not one persistent connection reused across samples),
matching the cost a bot would actually pay on a reconnect. Two numbers are
reported: handshake time (start() to Open event) and time-to-first-message
(send subscribe to first data frame — Polymarket pushes a full order book
snapshot immediately on subscribe, so this is a real "how fast do I get
usable data" measurement, not a wait for the next trade).

## Dependencies

- CMake >= 3.15
- A C++17 compiler
- libcurl (dev headers)
- OpenSSL (dev headers)
- Internet access at **configure time** (`cmake ..`) to fetch IXWebSocket
  via `FetchContent` — it isn't reliably available at a fixed version via
  apt/brew, so it's pulled from GitHub (pinned to `v12.0.1`) instead.

### Install system packages

macOS (Homebrew):
```
brew install cmake curl openssl@3
```

Ubuntu/Debian (typical cloud instance):
```
sudo apt update
sudo apt install -y build-essential cmake libcurl4-openssl-dev libssl-dev
```

## Build

```
cd polyAreaTesting
mkdir -p build && cd build
cmake ..
cmake --build . -j
```

Produces `./poly_latency_bench`.

## Run

```
BENCH_REGION_LABEL=aws-us-east-1 ./poly_latency_bench
```

Set `BENCH_REGION_LABEL` per deployment so results from different servers
are self-identifying once you collect the CSVs. Falls back to the machine
hostname if unset.

Options (all optional):

```
--samples N                 samples per endpoint after warmup (default 100)
--warmup N                  warmup samples discarded before recording (default 5)
--delay-ms N                delay between samples, ms (default 200)
--http-timeout-ms N         per-request timeout for REST calls (default 5000)
--ws-connect-timeout-ms N   handshake timeout for WS connect (default 5000)
--ws-message-timeout-ms N   timeout waiting for first WS message (default 10000)
--token-id ID                force the asset/token id used for WS subscribe
                             (default: auto-fetched from a live trade)
--output PATH                CSV output path (default ./results_<region>_<ts>.csv)
```

The warmup samples exist to avoid a cold DNS cache / cold TLS session
skewing the first couple of readings — they're discarded, not reported.

## Comparing regions

Run the same binary from each candidate cloud region with a distinct
`BENCH_REGION_LABEL`, collect the CSVs centrally, and diff/concat them
(e.g. in pandas) — every row already carries region, endpoint, and metric
so this is a straightforward `pd.concat` + `groupby`.

## Notes / limitations

- This measures *your* network path to Polymarket's edge, not their
  internal processing time — fine for the region-selection question, not a
  claim about Polymarket's own infra latency.
- The WS token id is re-resolved from a live trade each run by default, so
  the subscribed market may differ between runs (deliberate — avoids
  subscribing to a resolved/dead market). Pass `--token-id` to pin one for
  apples-to-apples comparisons across regions in the same run set.
- A single failed request/connection doesn't abort the run — it's logged
  to stderr and counted in the `failures` column of both the table and the
  CSV.
