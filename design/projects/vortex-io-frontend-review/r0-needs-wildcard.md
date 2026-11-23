# Wildcard needs and dialogue (pre-review)

This is a source-independent scenario brief for the editor, not a formal verdict. It tests generic
Vortex and future host/transport shapes beyond current frontends. No production ABI or users need
preserving. Generic requirements should be separated from adapter-specific guarantees and future
adapters; this does not demand all imagined transports in v1.

## Generic scenarios

### 1. Multiplexed full duplex and cyclic backpressure

A direct Vortex program owns one process/socket endpoint with independent read and write
capabilities. The child writes a large stdout record while awaiting stdin; the parent is also
writing stdin. If the parent waits for the full stdout record before draining it, the child's pipe
fills and it cannot progress. A single endpoint-wide FIFO/credit can create the same cycle if
blocked writes prevent reads. Bound bytes and work per endpoint/direction/request, reserve
completion/control capacity, and let independent directions progress. Cancelling one request must
not close sibling requests unless the endpoint policy explicitly makes close destructive.

### 2. Sparse mutable range observations

Request a bounded byte range at a 64-bit offset beyond 4 GiB, then the file truncates or changes
before another range is materialized. Preserve the locator, expected generation, and each successful
immutable read observation separately. A backend without atomic snapshot or compare-and-write must
report that limitation; it cannot promise coherent whole-file identity from stat checks. Keep the
read window and decoder/GPU leases bounded; never materialize the whole source merely to support
random access. A seek can cancel derived decode work while already retained raw observations remain
readable under their own leases.

### 3. Message framing, zero length, and replay

Submit two datagrams, one with a valid zero-length payload, then branch/rewind and await their
request cells again. Same-request await sees the same observations; executing Submit again is a new
effect. Datagram boundaries must survive; streams instead have an empty successful chunk distinct
from terminal EOF. This requires request identity and typed outcomes, not frontend iteration policy.

## Buffering and framing trace

```text
promptReq := submit(stdout, write(bytes("Name? ")))
flushReq  := submit(stdout, flush_after(promptReq))
flushSeen := await(flushReq)
if flushSeen is successful:
    lineReq  := submit(stdin, read_line(maxBytes = B))
    lineSeen := await(lineReq)
```

This must define that the flush fence orders after the prompt on the same output endpoint and that
the input request is not submitted until the required visibility outcome is observed. Admission,
accepted bytes, backend flush, durable sync, and remote acknowledgement are distinct; adapters may
refuse guarantees they cannot provide. A pending write/flush cannot block the owner thread.

Unbuffered, fixed-size, line, and generic frame/token-boundary modes need explicit bounded
read-ahead ownership. A fixed-size read can return short bytes and EOF. Lines/tokens/frames larger
than the configured budget need typed overflow/gap behavior without unbounded growth. Delimiters and
UTF-8 code points may split across chunks. Raw and framed consumers cannot both advance the same
stream cursor unless the contract says who owns prefetched suffix bytes and how they transfer to the
next read. Language grammar/token interpretation remains in the frontend or a pure adapter; the
shared layer need not parse Prolog/VIcon/VLace syntax.

A buffered output aborted before submission is local cleanup. After submission, abort/cancel cannot
undo external bytes and reports partial/unknown progress. Async close may fail and must remain
observable. Re-evaluating an explicit send is a fresh effect even if the backend coalesces native
writes; automatic resumption only observes the same request.

## Group identity dialogue (VPL and VQL)

Recommendation: generic protocol fundamentals are independent request identities and per-request
outcomes; VPL array shape/order and VQL row association belong to their respective lowering. A
non-array counterexample is a direct-Vortex supervisor that starts three unrelated endpoint reads
for one temporary dashboard snapshot and abandons the snapshot while one request is complete, one
running, and one backpressured. First model it as a language-neutral owner cell/rank of request
references with explicit per-request cancel and bounded member leases. This avoids a batch opcode
and retains each operation's effect identity.

Add a group identity only if common deadline/accounting/bulk-selection semantics cannot be expressed
with request refs and per-request service calls. Such a group must expose partial admission and
every member outcome; it cannot imply atomic admission, transport order, retry, cancellation
success, or external undo. Group membership/retention itself must be bounded and accounted. VPL
still maps each input position to its own request regardless of completion order.

## Classification

Generic must-support: unique request/effect identities, typed empty/EOF/partial/error/unknown
outcomes, await vs request cancellation, wide checked byte ranges, bounded independent multiplexing,
completion/control reserve, operation deadlines, explicit close/flush outcomes, and lease-aware
bounded payload views. Adapter-specific: message boundaries, source snapshot consistency, atomic
compare-write, durable sync, remote acknowledgement, line/frame support, and cancellation strength.
A backend that cannot meet a requested guarantee returns a typed refusal. Future adapters may add
capabilities without imposing their semantics on every current backend.
