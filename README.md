# BidForge

A small real-time bidding project built with C++17 and Go. The C++ server picks an ad
campaign for each request, and the Go simulator acts as an exchange that runs auctions
and sends back wins or losses.

The main things I wanted to explore were concurrent budget updates, handling duplicate
requests, and comparing a simple campaign scan with a category index. It runs locally
and supports a small subset of OpenRTB for banner ads.

## Architecture

```text
+-------------------------+       +--------------------------------+
| Go exchange simulator   |       | C++ bidder                     |
|                         |       |                                |
| Generate requests       | /bid  | Validate request               |
|                         |------>|        |                       |
|                         |       | Match and rank campaigns       |
|                         |       | (full scan or category index)  |
|                         |       |        |                       |
|                         |       | Lock, check budget, reserve    |
| Run a simulated auction |<------| Return bid or no-bid           |
|                         |       |                                |
| Send win/loss           |------>| /outcome: settle budget        |
|                         |       |                                |
| Collect results         |------>| /stats                         |
+-------------------------+       +--------------------------------+
                                         ^
                                         |
                                   Campaign JSON
```

The simulator can generate campaign files; the bidder loads one at startup.
Campaigns must match the requested size and category, meet the price floor, and have
enough budget. The highest eligible bid is selected, with campaign ID breaking ties.

One mutex protects budgets and request history so concurrent requests cannot reserve
the same money twice. A win turns reserved money into spend; a loss releases it.
Duplicate requests reuse the saved decision, and repeated outcomes do not charge again.
Money is stored as integers to avoid rounding errors in budget calculations.

## Build and run

You need a C++17 compiler, CMake 3.20+, Go 1.23+, and Python 3. The first CMake run
needs internet access to download dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
go -C tools/loadgen build -o loadgen .

./build/bidforge --campaigns examples/campaigns.json --mode indexed
```

The server runs at `http://127.0.0.1:8080`. Use `--mode baseline` to scan all campaigns
instead of using the category index.

In another terminal, send an example request:

```sh
curl -i -H 'Content-Type: application/json' \
  --data-binary @examples/request.json http://127.0.0.1:8080/bid

curl -H 'Content-Type: application/json' \
  -d '{"bid_id":"bid-example-1","outcome":"win"}' http://127.0.0.1:8080/outcome

curl http://127.0.0.1:8080/stats
```

| Endpoint | Purpose |
| --- | --- |
| `POST /bid` | Request a bid; returns 200 for a bid or 204 for no-bid |
| `POST /outcome` | Report a win or loss using the local testing API |
| `GET /health` | Check that the server is running |
| `GET /stats` | View request counts, reservations, and spend |

## Run the simulator

Stop the example server first, then generate campaigns and start a fresh server:

```sh
./tools/loadgen/loadgen generate -campaigns 1000 -categories 100 \
  -seed 17 -output /tmp/campaigns.json
./build/bidforge --campaigns /tmp/campaigns.json --mode indexed
```

In another terminal:

```sh
./tools/loadgen/loadgen run -requests 1000 -concurrency 8 -categories 100 \
  -seed 17 -duplicate-percent 20 -missing-percent 10 -output /tmp/results.json
```

Use generated campaigns for these runs: their categories differ from the hand-written
examples. Restart the server between runs to reset budgets and request history.

## Tests and benchmarks

```sh
ctest --test-dir build --output-on-failure
python3 scripts/integration.py
go -C tools/loadgen test -race ./...
go -C tools/loadgen vet ./...
python3 scripts/benchmark.py
```

Tests cover input validation, matching, concurrent budget use, duplicate requests, and
outcomes. Benchmarks compare the two matching modes and measure HTTP latency and throughput.
See the [benchmark report](docs/benchmarks.md) for results and testing limitations.

## Project layout

```text
include/         Bidder types and interface
src/             Bidder logic and HTTP server
tools/loadgen/  Go exchange simulator
tests/          C++ tests and matching benchmark
examples/       Sample campaigns and requests
scripts/        Integration tests and benchmark runner
docs/           Design notes and measurement results
```

## Limitations

Everything is stored in memory, so restarting loses spend and request history. Missing
outcomes leave money reserved, and a full request history rejects new IDs. Deadline
checks cover handler work, not the full network round trip. This is a local learning
project with no database, authentication, or connection to a real ad exchange.

More detail: [design notes](docs/design.md), [walkthrough](docs/walkthrough.md),
and [dependencies](docs/dependencies.md).
