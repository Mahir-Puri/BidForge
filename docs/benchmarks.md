# Measured results

Recorded on 2026-10-16 (America/Toronto). Raw per-run summaries are in
[measurements.json](measurements.json); `scripts/benchmark.py` regenerates them.

Machine: Apple M3, 16 GiB RAM, macOS-26.6.2-arm64-arm-64bit.
Compiler: Apple clang version 16.0.0 (clang-1600.0.26.6). CMake Release build; C++17; Go 1.23.6.
These are local development-machine observations, not controlled capacity claims.

## Matching only

Seed 17; 300×250 active campaigns; uniform request categories; zero floor; immutable
matching only, without accounting. Each mode gets 100 warm-up calls, then five runs of
2,000 calls. Exact ordered candidate equivalence is checked before timing. Medians:

| Campaigns | Categories | Baseline µs/match | Indexed µs/match |
|---:|---:|---:|---:|
| 100 | 1 | 3.338 | 3.523 |
| 100 | 100 | 1.764 | 0.139 |
| 1000 | 1 | 65.414 | 49.216 |
| 1000 | 100 | 23.420 | 0.601 |

The selective case reduces candidate work substantially. With one category, indexing
still gathers and sorts essentially the whole set and provides little benefit. Do not
apply the matching-only ratio to overall HTTP throughput.

## End-to-end HTTP

1,000 campaigns, 1,000 unique auctions, 1,210 bid attempts (20% duplicate injection),
8 clients and 8 server workers, queue 128, tmax 100 ms, 20% duplicate outcomes,
10% missing outcomes, no artificial outcome delay. Each mode/category pair has a
200-request warm-up on a separate server and three measured fresh-server runs.
Ample budgets avoid exhaustion. Final per-campaign accounting and unique win/loss
counts are asserted equal across modes and repetitions before comparison.

Medians of the three per-run values (percentiles are not pooled):

| Categories | Mode | Completed HTTP/s | p50 ms | p95 ms | p99 ms | Handler p50 ms |
|---:|---|---:|---:|---:|---:|---:|
| 1 | baseline | 12088 | 0.273 | 0.742 | 1.196 | 0.085 |
| 1 | indexed | 11245 | 0.298 | 0.935 | 1.674 | 0.086 |
| 100 | baseline | 7960 | 0.284 | 1.608 | 3.304 | 0.047 |
| 100 | indexed | 16257 | 0.222 | 0.555 | 1.071 | 0.025 |

Individual normal runs ranged from 5918 to 16575 completed requests/s.
The runs are short and visibly noisy; compilation, other applications, scheduling, and
power state were not isolated. These data do not establish a reliable HTTP speedup.
The earlier exploratory run differed substantially; the checked-in data are the final run.

Normal runs each returned 1,210 bid responses and no no-bids. They had zero bid transport
errors, timeouts, HTTP 503s, notification failures, invariant violations, or duplicate
decision mismatches. See JSON for exact notification and pending counts.

Throughput is completed bid HTTP requests divided by the entire workload duration,
including workers waiting for outcomes. 503 responses count as completed HTTP requests;
timeouts and transport errors do not. Client percentiles include failed attempts, but
exclude time waiting in the tool job queue and exclude outcome time. Handler percentiles
come from X-Processing-Micros on 200/204 responses; server totals also expose matching
time separately. No real-world revenue or win-rate improvement is inferred.

## Overload and missing feedback

| Case | Bid attempts | HTTP completed | 503 | Transport errors | Timeouts | Client over tmax | Handler deadline no-bids | Notification failures | p99 ms | Sampled RSS KiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| overload-queue | 1000 | 148 | 0 | 851 | 1 | 981 | 0 | 147 | 61.848 | 4368 |
| overload-history | 500 | 497 | 477 | 0 | 3 | 11 | 0 | 0 | 1028.282 | 4496 |
| delayed-missing | 400 | 400 | 0 | 0 | 0 | 0 | 0 | 0 | 2.382 | 4688 |

- Queue overload: 128 clients, one HTTP worker, two queued connections, tmax 1 ms.
  Connection drops are visible as transport errors. Outcomes can also fail; their
  reservations remain pending. Do not relabel these drops as HTTP 503.
- History overload: 32 clients, eight workers, capacity 20. Most new IDs get 503;
  keep-alive connection occupancy also produces long queueing and possible timeouts.
- Delayed/missing: 200 unique auctions, 400 attempts, eight clients, 2 ms notification
  delay, 50% missing outcomes and 100% duplicate outcomes. Pending funds remain visible.

All scenarios reported zero invariant violations and duplicate-decision mismatches.
Client-over-tmax can greatly exceed handler deadline no-bids because the handler clock
does not include socket queueing. Timely handler checks cannot enforce exchange arrival.
RSS is sampled with `ps` every 50 ms, not a guaranteed peak; short runs can miss peaks.
Both the client and server ran on the same machine. This is a closed-loop experiment,
not a fixed-arrival-rate capacity or sustained-resource study.

## Checks actually executed

- Release build with `-Wall -Wextra -Wpedantic -Wconversion`; nine GoogleTest cases passed.
- Real HTTP integration in both matching modes: valid bid, no-bid, malformed input,
  concurrent duplicates, repeated/conflicting/unknown outcomes, budget pressure,
  missing feedback, and history rejection.
- Go build, four tests with `go test -race`, and `go vet` passed.
- UBSan Debug build, all nine C++ tests, and real HTTP integration passed.
- ASan+UBSan and separate TSan builds compiled, but their runtimes failed before tests.
  ASan reports `sanitizer_malloc_mac.inc:189 (!asan_init_is_running)` even for an empty
  `int main() { return 0; }`; retrying outside the sandbox did not fix it.
  TSan also crashes on an empty program. C++ ASan/TSan results remain unverified.
- Matching equivalence checks preceded timing; HTTP final accounting equivalence passed.

## Reproduce

```sh
source scripts/env.sh  # only for the local tool installation in this workspace
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
go -C tools/loadgen build -o loadgen .
python3 scripts/benchmark.py
```

The script writes the generated campaigns and full per-run JSON under `results/`,
then replaces `docs/measurements.json`. This narrative records the run above; refresh
its tables when recording a new run. No server remains running after the script exits.
For longer experiments, increase requests and repetitions in the script; ensure history
capacity is large enough, and keep the generated campaign budgets equivalent.
