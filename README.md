# BidForge

BidForge is a local C++17 bidder with a Go exchange simulator. It selects a fixed-price
banner campaign for an advertising opportunity and reserves its possible cost without
letting concurrent requests overcommit the campaign budget. The Go tool runs a synthetic
first-price auction and reports its outcome. The C++ service does **not** run the exchange's auction.

This is a documented **subset of OpenRTB**, with our own local `/outcome` testing API.
It is not a complete Google Authorized Buyers integration and has not been certified
for live advertising traffic. There is no user tracking, database, frontend, or external exchange connection.

## Build and run

Requirements: a C++17 compiler, CMake 3.20+, Go 1.23+, and Python 3 for integration tests
and benchmark orchestration. CMake downloads pinned, SHA-256-verified C++ dependencies
into the ignored build directory on first configuration, which requires internet access.
Subsequent builds reuse those downloads. Go uses only its standard library.
See [dependency versions and licenses](docs/dependencies.md).

```sh
# In this workspace, local CMake 3.31.6 and Go 1.23.6 are already installed:
source scripts/env.sh

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
go -C tools/loadgen build -o loadgen .
./build/bidforge --campaigns examples/campaigns.json --mode indexed
```

On another machine, install the requirements normally and omit `source scripts/env.sh`.
The server binds to **127.0.0.1**. In another terminal:

```sh
curl -i http://127.0.0.1:8080/health
curl -i -H 'Content-Type: application/json' \
  --data-binary @examples/request.json http://127.0.0.1:8080/bid
curl -i -H 'Content-Type: application/json' \
  --data-binary @examples/no-match.json http://127.0.0.1:8080/bid
curl -i -H 'Content-Type: application/json' \
  -d '{"bid_id":"bid-example-1","outcome":"win"}' http://127.0.0.1:8080/outcome
curl http://127.0.0.1:8080/stats
```

The first bid returns HTTP 200, campaign `sports-300`, and price `3.0`; the size-mismatched
request returns HTTP 204. The win moves 3,000 microdollars from reserved to committed.
Repeating that win leaves the accounting unchanged. Stop the server with Ctrl-C.

Server options (all require values):

| Option | Default | Meaning |
|---|---:|---|
| `--campaigns` | `examples/campaigns.json` | Startup configuration |
| `--mode` | `indexed` | `baseline` or `indexed` |
| `--port` | `8080` | Local port |
| `--workers` | `8` | HTTP library workers, 1–256 |
| `--queue` | `128` | Maximum queued connections, positive |
| `--history` | `100000` | Maximum unique request IDs, positive |

## Campaign configuration and money

The configuration is a nonempty JSON array; every field below is required:

```json
[{"id":"sports-300","active":true,"categories":["IAB17"],
  "width":300,"height":250,"bid_milli_cpm":3000,
  "budget_micros":50000000,"creative_id":"banner-sports"}]
```

Budgets, reservations, and committed spend use signed 64-bit **integer microdollars**.
Bids use integer **thousandths of a dollar CPM**. A bid of `3000` means $3.000 per
1,000 impressions: $3 / 1,000 = $0.003 per impression = **3,000 microdollars**.
A $50 budget is **50,000,000 microdollars**. Thus the bid integer is also the exact
per-impression cost in our first-price model; budget accounting never uses floating point.

Campaign IDs must be unique. IDs and category strings are 1–128 bytes; categories
contain 1–64 entries, normalized to a sorted set. Dimensions are integers 1–10,000.
Bids are 1–1,000,000,000; budgets are 0–INT64_MAX and must cover at least one bid.
Negative values, fractional integers, missing fields, invalid booleans, and overflow
are rejected at startup. This intentionally rejects a zero-budget campaign even if inactive.

## HTTP contract

`POST /bid` accepts exactly one banner impression, one size, USD, fixed bids, and
contextual category overlap. See [examples/request.json](examples/request.json):

```json
{"id":"example-1","tmax":100,"at":1,"cur":["USD"],
 "imp":[{"id":"imp-1","banner":{"w":300,"h":250},
         "bidfloor":2.500,"bidfloorcur":"USD"}],
 "site":{"cat":["IAB17"]}}
```

Required: `id`, integer `tmax` (1–60,000 milliseconds), `imp[0].id`, integer banner
`w` and `h`, and nonempty `site.cat`. Currency defaults to USD; explicit `cur` must
be `["USD"]` and `bidfloorcur` must be `"USD"`. Optional `at` must be `1`;
this local API always uses first price even when omitted. Other OpenRTB auction types
are outside this subset. Video, audio, and native impressions are rejected.

Optional `bidfloor` defaults to zero. It is a JSON number in USD CPM, range
0–1,000,000, supporting at most three decimal places. `2.501` normalizes to `2501`;
`2.5001` is rejected, not rounded down. Decimal-to-binary conversion gets a tolerance
of four double-precision epsilons scaled by the value. Values closer than this cannot
be distinguished reliably by the JSON number representation. This conversion is only
at the protocol boundary; all eligibility comparisons and accounting use integers.

Campaigns must be active, share **any** category, match both dimensions, meet the floor,
and have available budget. Highest bid wins selection; equal bids use lexicographically
smallest campaign ID. If its budget is exhausted, selection tries the next eligible campaign.
A response has this shape:

```json
{"id":"example-1","cur":"USD","seatbid":[{"bid":[{
 "id":"bid-example-1","impid":"imp-1","price":3.0,
 "cid":"sports-300","crid":"banner-sports","w":300,"h":250
}]}]}
```

Unknown fields are ignored. They do not affect targeting or idempotency. This does not
implement creative markup, billing IDs, deals, privacy signals, content blocking, or
other constraints a live integration would require.

| Status | Meaning |
|---|---|
| 200 | Bid, successful outcome, health, or statistics |
| 204 | Saved no-bid decision: no eligible budget or elapsed handler deadline |
| 400 | Invalid supported input or outcome |
| 404 | Unknown bid ID (or route) |
| 409 | Request ID reused with different normalized content, or conflicting terminal outcome |
| 413 | HTTP library payload limit exceeded (64 KiB) |
| 503 | New ID rejected because request history is full; retry is not a saved decision |

`POST /outcome` accepts `{"bid_id":"bid-example-1","outcome":"win"}` or `"loss"`.
A pending win transfers the reservation into committed spend. A loss releases it.
Repeated identical outcomes return the same terminal state; conflicting outcomes return 409.
The returned state is `won` or `lost`. Outcomes charge the submitted bid, not a supplied price.

`GET /health` returns `{"status":"ok"}`. `GET /stats` returns received/valid-new/duplicate
request counts, bid/no-bid responses, validation failures, deadline no-bids, history overload,
history size, pending/won/lost counts, matching time totals, and each campaign's budget,
committed, and reserved amounts. Budget/state snapshots share the accounting lock.
HTTP handler timing counters are independent atomic samples and may be one request apart.
Oversized bodies and socket-queue drops do not enter the handler counters.

## Reservations, duplicates, and deadlines

For every campaign, `committed + reserved <= budget`. One mutex protects accounting
and request records together. Candidate matching happens outside it; budget availability
is checked again under the lock. No network operation occurs while holding the lock.

The same normalized request ID returns its saved bid or no-bid, even after settlement
or after another campaign becomes affordable. Normalization sorts/deduplicates categories,
converts the floor to integers, and fills supported defaults. `tmax`, impression ID, dimensions,
floor, and categories participate in equality. Concurrent duplicates cannot both reserve.
Full history rejects new IDs rather than evicting decisions; existing IDs and outcomes
still work. Records and bids have no arbitrary expiration timer.

Missing feedback and failed response writes keep funds reserved: the bidder cannot know
whether the exchange received the bid. Restarting the service **resets all accounting and
idempotency history**. Guarantees apply only within one running server process.

A monotonic clock starts at handler entry, before JSON parsing. The deadline is checked
under the accounting mutex immediately before a new reservation. It does not cover body
reading, the library's connection queue, response delivery, or client/network delays.
A thread can also be descheduled after the final check. Replaying a saved bid creates no
reservation and may occur after the retry's deadline. `X-Processing-Micros` measures valid
handler work through response serialization, excluding network delivery. Client latency
is measured separately; timely handler completion does not guarantee timely auction arrival.

## Go exchange and required scenarios

```sh
./tools/loadgen/loadgen generate -campaigns 1000 -categories 100 -seed 17 -output /tmp/campaigns.json
./build/bidforge --campaigns /tmp/campaigns.json --mode indexed
# In another terminal:
./tools/loadgen/loadgen run -requests 1000 -concurrency 8 -seed 17 -categories 100 \
  -duplicate-percent 20 -duplicate-outcome-percent 20 -missing-percent 10 \
  -outcome-delay 2ms -output /tmp/results.json
```

The tool also supports `-url`, `-prefix`, `-tmax`, `-timeout`, and
`-distribution uniform|hot`. Hot puts 80% of draws directly in category 0 and uses
uniform draws for the remaining 20%. Generated campaigns use `cat-0`, `cat-1`, etc.;
they intentionally differ from the IAB-named hand-written examples. Use the generator
configuration when using loadgen, and restart the server or change `-prefix` between runs.

Competing bids are discrete uniform integers **0–6000** in the same bid units.
Generated campaign bids are uniform **1000–5000**, with round-robin categories.
Ties go to the competitor. All random inputs and duplicate jobs are generated from a seed
before starting bounded workers; concurrent timing and budget competition are not deterministic.
Each unique returned auction is evaluated once, and outcomes are sent only for actual bids.
Duplicate percentage is the chance of one extra attempt per unique input, not the fraction
of all attempts. A bounded worker remains occupied during its outcome delay.

| Scenario | Reproduce |
|---|---|
| Basic / no-match | The curl commands above |
| Budget pressure | `python3 scripts/integration.py` uses 9,000 micros against 64 concurrent opportunities |
| Concurrent duplicate IDs | Integration sends 32 identical attempts, then checks one reservation |
| Duplicate wins | Integration sends 32 identical notifications and checks one charge |
| Missing feedback | Integration retains two pending bids; loadgen supports `-missing-percent 50` |
| Matching equivalence | C++ tests compare rankings and decisions; `match_bench` checks rankings before timing |
| Matching / HTTP comparison | `python3 scripts/benchmark.py` resets each server, checks final accounting equivalence |
| Overload | Same benchmark uses 128 clients with 1 server worker / 2 queue slots, then a history capacity of 20 |

The library queue is bounded in **connections**, not HTTP requests. Queue overflow
closes connections; the tool reports transport errors, not invented HTTP 503s. Keep-alive
connections occupy library workers and can cause queued connections to wait even when
matching is fast. History overload returns explicit HTTP 503. HTTP client timeouts and
notification failures have separate counters. Lost feedback remains conservative.

## Tests and measurements

```sh
ctest --test-dir build --output-on-failure
python3 scripts/integration.py
go -C tools/loadgen test -race ./...
go -C tools/loadgen vet ./...

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DASAN=ON
cmake --build build-asan -j 4
ctest --test-dir build-asan --output-on-failure
python3 scripts/integration.py build-asan/bidforge

# Separate builds: -DTSAN=ON for ThreadSanitizer, -DUBSAN=ON for UBSan alone.
python3 scripts/benchmark.py
```

The Release C++ tests, real HTTP integration in both modes, Go race tests and vet,
and UBSan tests/integration were executed successfully. Apple Clang 16's ASan and
TSan runtimes fail before running even an empty program on this macOS environment;
those builds compile, but their C++ checks are **not verified**. See
[the measurement report](docs/benchmarks.md) for exact results and limitations.

Measured matching-only and HTTP results are in [docs/measurements.json](docs/measurements.json).
The report includes p50/p95/p99 latency, completed HTTP requests/s, bid/no-bid counts,
client and server deadline misses, transport failures, notifications, duplicate mismatches,
and invariant violations. Matching-only runs use warm-up and five repetitions; HTTP runs
use separate warm-up servers and three fresh-state repetitions. The benchmark saves raw
workloads and results under ignored `results/`.

This is a **closed-loop, fixed-concurrency** workload. Throughput includes time workers
spend on notifications and delays; it is not a maximum sustainable fixed-arrival-rate
capacity measurement. Synthetic wins and spend are not predictions of advertising performance.

## Code map and limitations

- `src/bidder.cpp`, `include/bidder.h`: validation, both matchers, reservations, outcomes, snapshots.
- `src/main.cpp`: startup options and HTTP routes using cpp-httplib's worker pool.
- `tools/loadgen/main.go`: campaign generation, exchange simulation, measurements.
- `tests/`: GoogleTest cases and a matching-only benchmark.
- `scripts/`: real HTTP checks and repeatable measurement orchestration.
- [Design](docs/design.md), [walkthrough](docs/walkthrough.md), and [benchmark report](docs/benchmarks.md).

In-memory history eventually fills; restart loses all history and spend. There is no
recovery from crashes, durable outcome delivery, multiple-instance budget coordination,
authentication, graceful shutdown protocol, or live creative rendering. Statistics scan
history while holding the mutex; very frequent polling can affect latency. Request bodies
and queues are bounded, but campaign files are trusted local input. These are explicit
limits of the local project, not claims of production readiness.

Protocol references checked during implementation:
[RTB overview](https://developers.google.com/authorized-buyers/rtb/get-started/start),
[request guide](https://developers.google.com/authorized-buyers/rtb/get-started/request-guide),
[response guide](https://developers.google.com/authorized-buyers/rtb/get-started/response-guide),
and [OpenRTB guide](https://developers.google.com/authorized-buyers/rtb/openrtb-guide).
These establish the bidder/exchange roles, CPM floor units, response fields, and empty
204 no-bid response; the supported subset and local outcomes above remain our contract.
