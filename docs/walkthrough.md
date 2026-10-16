# Walkthrough

Start with `examples/campaigns.json` and `examples/request.json`. The sports campaign
bids 3,000 and has 50,000,000 micros. The opportunity is a 300×250 banner on category IAB17,
with a floor of 2.500 CPM. It should get a 3.000 CPM bid.

1. **Startup — `src/main.cpp`.** Read the command-line options, parse the JSON file, and
   call `parse_campaigns`. Construct one `Bidder` and cpp-httplib server. The library owns
   the worker pool. Routes capture the bidder by reference; it outlives the server.
2. **Validation — `parse_request` in `src/bidder.cpp`.** Check the supported fields and
   bounds. Normalize category order and floor units. Bad inputs get 400 without a reservation.
3. **Matching — `Bidder::match`.** Gather all campaigns or category-index candidates. Use
   the same active/size/floor/category checks in either case. Sort by bid and campaign ID.
   This function is also what `tests/match_bench.cpp` measures without changing budgets.
4. **Decision — `Bidder::bid`.** Acquire the mutex. If the request ID already exists,
   compare normalized input and replay the stored answer or return 409. For a new ID,
   check history capacity, then each candidate's available budget. Prepare the response,
   insert the record, check time again, and reserve 3,000 micros. The server serializes
   and sends the answer after this function releases the mutex.
5. **Settlement — `Bidder::outcome`.** Find the record by bid ID. A win changes reserved
   from 3,000 to zero and committed from zero to 3,000. Another win returns success with
   no change. A loss instead would release the reservation. A later conflicting loss
   after a win returns 409.
6. **Exchange — `tools/loadgen/main.go`.** Generate all seeded inputs, shuffle attempt jobs,
   and run a fixed number of workers. `submit` sends a request and uses a shared decision
   map to ensure that duplicates do not run another auction. Compare the bid with its
   pre-generated competitor, then optionally delay, omit, or repeat the outcome.
7. **Measurement.** Each worker reports its attempt through a channel; one collector
   computes counters and percentiles. Bid HTTP time excludes outcome delay; the overall
   workload throughput includes the time workers spend delivering outcomes. Server stats
   expose reservations still pending after the workload.

For a useful experiment, change a campaign budget to exactly 3,000 and submit two different
request IDs. Only one can get a pending bid. Send it a loss, then try a third ID: that ID can
bid. Retry the second ID: it still returns its saved no-bid. This illustrates why idempotency
is a saved decision, not merely a duplicate charge check.

The concurrency tests use a condition-variable gate so all test threads are ready before
submitting. Read `ConcurrentBudgetAndDuplicates` and the HTTP integration script to see the
same invariants tested both directly and over real sockets. The `EquivalentAcrossManyInputs`
test checks both matchers while budgets are being consumed, including exhaustion.

For an interview, explain the budget invariant first, show the mutex-protected transition,
and then discuss the tradeoff: it is easy to reason about, but eventually serializes work.
Use the measured broad-category case to explain why adding an index is not always faster.
Finish by identifying the persistence boundary: losing memory loses the guarantee across restarts.
