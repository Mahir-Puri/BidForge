# Design notes

## One mutex for decisions and money

The immutable campaign configuration and category index are safe to read concurrently.
`match()` returns eligible campaign indexes sorted by descending bid, then campaign ID.
It never reads or changes a budget. The baseline scans all campaigns; the index gathers
only campaigns in the requested categories, deduplicates them, and applies the same predicate.
The sorted candidate order is identical in both modes. An index helps selective categories;
when most campaigns match, vector gathering and deduplication can add work.

`Bidder` is a small owning class because the mutex, accounting vectors, and request history
must live and be accessed together. It prevents routes from mutating those structures without
their lock. It is not a polymorphic service abstraction.

The accounting mutex serializes ID lookup, budget recheck, decision storage, and reservation.
Suppose a campaign has 3,000 micros left and two requests match it. The first lock holder
reserves 3,000; the second sees zero available. Checking budget only during matching would
allow both to promise the same money. The final selection can fall back to another campaign.

Budget checks subtract `budget - committed - reserved`, after maintaining nonnegative
accounts, rather than adding untrusted amounts. The selected bid is at most that difference,
so adding it cannot overflow. Moving a reservation to committed spend preserves the sum.
Independent budget and request locks would complicate the atomicity of duplicate handling;
there is no measured reason for that complexity here.

## Request and outcome records

A normalized request includes its ID, impression ID, tmax, dimensions, integer floor,
and sorted category set. Currency and auction type have only one supported normalized
value. Ignored OpenRTB fields do not affect equality. Saving no-bids matters: releasing
funds later must not change an existing request's answer.

Records are inserted before money changes. Bid lookup insertion failures erase the new
record; the final deadline check happens after these allocations. A later response-copy
or network failure leaves a recorded reservation, which is the conservative behavior.

Bid IDs are `bid-` plus the request ID and are unique within this process. A record is
`none` for no-bid, otherwise `pending -> won` or `pending -> lost`. Terminal transitions
are irreversible. A repeat of the same terminal notification succeeds without moving money.
The inverse terminal notification returns a conflict. Unknown outcomes return 404.

No timer releases pending funds. A missing notification might mean a real win whose feedback
was lost. Automatic release would make those funds available for another bid and could
overspend. Capacity bounds all history, including no-bids and settled bids; rejecting new
IDs is simpler and more honest than silently expiring the idempotency guarantee.

## Protocol and timing boundaries

Integer units make a submitted CPM bid exactly equal to its single-impression microdollar
cost. JSON CPM is converted only at the edge. Floors with unsupported precision are rejected,
with a small floating representation tolerance; this project does not promise arbitrary
precision decimal JSON parsing.

The handler's monotonic clock includes validation, matching, lock waiting, record allocation,
and serialization. It excludes connection acceptance, body reading, library queueing, and
socket delivery. The deadline check stops new reservations when it observes expiration.
The final machine instructions or a scheduling pause can still cross a deadline. Duplicate
replays preserve an existing decision, even if the retry is late. Go separately records
client-observed elapsed time, including attempts that fail.

The supported library pool owns its threads. Bounded socket queue overflow is a transport
failure; history capacity produces HTTP 503. Keep-alive connections retain a worker until
close/timeout, an important limitation of this straightforward blocking HTTP library.

## Crashes, persistence, and multiple processes

A crash loses records, reservations, and committed spend. It may occur after returning a bid
but before seeing its outcome. Restarting from campaign configuration cannot reconstruct
that liability. This is why the guarantee is explicitly one running process.

A durable version would need transactional storage for request decisions, reservations,
and outcome transitions, with the decision durable before replying. It would also need
reconciliation for uncertain feedback and a documented ID retention period. Multiple
instances would require a shared transactional authority or exclusive budget ownership;
putting this same local mutex in several servers would not protect a shared campaign budget.
These mechanisms are deliberately outside this first version.
