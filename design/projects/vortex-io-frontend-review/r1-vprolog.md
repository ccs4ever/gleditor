# Round 1 critique: VProlog (vortex-io-redesign.md)

Target verified: v0.9, SHA-256 `a6325b19e99bdec6f7f6cb6724ed7a6b5ddecf1e7152e345194ae6872eb58535`.

Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Current VProlog implementation checked

The old recursive-resolver account in `design/vprolog-lens-bridge.md` is stale for the current query
path. `Compiler::solve` calls `VortexStdLib::solveQuery`
(`apps/common/xanadu/vprolog/compiler.cpp:243-250`), whose `solve` invokes
`logic::compileAndRunQuery` (`apps/common/xanadu/vortex/logic_solve.cpp:79-93`). The logic compiler
emits VM frames and represents backtracking with VM `Fail`/arena rollback
(`apps/common/xanadu/vortex/logic_compile_core.cpp:1-20`). Vlog still represents logical variables
and bindings using cells and clone ranks, with arena mark/release as undo
(`apps/common/xanadu/zigzag/vlog.hpp:11-20, 34-42`; `vlog.cpp:88-141`).

The frozen target correctly records this current VM path and places pending activation,
bindings/trail, and goal-placement policy in the VProlog compiler/adapter
(`design/vortex-io-redesign.md:135-143, 219-221`). This corrects the implementation-source
description; no finding remains about where VProlog continuation state belongs.

## MAJOR

### 1. (§6.2, §6.5, §11.2) Message boundary semantics are underspecified

**Proposal:** The cell-value contract says “An empty payload is a successful zero-length value; EOF
is a distinct typed outcome” (§6.2, lines 388-391). The request sketch is byte-range
`Read { offset, maxBytes }` (§6.5, lines 469-482), and the buffering section offers an
“adapter-supported frame policy” (§6.5, lines 556-562). The acceptance gate nevertheless requires
“zero-length datagrams versus stream EOF” (§6.5, lines 613-615) and tests buffering/framing (§11.2,
line 948).

**Problem:** Those provisions distinguish byte length from stream termination, but do not define a
message-oriented request. They do not say that one successful datagram receive returns exactly one
entire message, including a valid zero-length message; that message boundaries survive
read-ahead/coalescing; that message endpoints do not synthesize stream EOF; or that one send request
names one message. A frontend such as VProlog can observe an empty payload, but cannot tell from the
normative request/outcome contract whether it is an empty message, a successful zero-byte stream
read, or a boundary flattened across adjacent messages. An implementation could flatten messages or
report a zero-length receive as EOF while still following the byte-range and generic frame wording.
That breaks request identity and data framing, not merely a Prolog grammar choice.

**Evidence:** The v0.9 wording at §6.2 lines 388-391 defines only payload length and EOF; §6.5 lines
556-562 leaves frame policy adapter-supported; §11.2 line 948 tests generic framed reads, while §6.5
lines 613-615 names datagrams without requiring one-message semantics. The VProlog row exposes only
byte `read(0,4096)` at §6.5 line 550. The shared brief requires future/direct transport cases to be
covered but permits adapters to refuse unsupported guarantees.

**Correction:** Add a generic message-oriented capability/request contract, without requiring every
adapter to implement it: one successful `ReceiveMessage` request returns exactly one whole message
payload and boundary/length metadata, including a zero-length payload; it is not stream EOF. One
`SendMessage` request names one frozen message and retains its own identity, progress, and outcome
even when the backend coalesces native transfers. Message endpoints do not synthesize stream EOF.
Adapters lacking these semantics return a typed unsupported-capability refusal. Add an acceptance
trace with two receives (first a zero-length message, then a nonempty message) and two independently
identified sends. Keep message framing in the generic transport contract and Prolog term/DCG parsing
in the frontend.

Proposed VProlog-facing example, not frozen syntax:

```prolog
io_submit(Cap, receive_message(max_bytes(4096)), EmptyReq),
io_await(EmptyReq, EmptySeen),
io_outcome(EmptySeen, message(payload(EmptyBytes), length(0))),
io_submit(Cap, receive_message(max_bytes(4096)), NextReq),
io_await(NextReq, NextSeen),
io_outcome(NextSeen, message(payload(NextBytes), length(12))).
```

Both receive results are messages; neither is EOF. Matching these cells is observational. Reaching
`io_submit` again is new work, while re-awaiting either retained request observes its same message.
`SendMessage` should have the analogous one-request/one-message rule. Current sources provide no
claim that a datagram backend is already implemented; an adapter may refuse message capability until
it is.

## Cross-role dialogue

- **Wildcard:** I asked whether empty-vs-EOF and adapter-supported framing already establish message
  boundaries. Wildcard confirms they do not: its R1 counterexample is a zero-length datagram
  followed by another datagram, where flattening or EOF synthesis changes the message stream. We
  agree the correction is a generic message capability with typed refusal for stream-only adapters;
  no group/batch identity is required.
- **VIcon:** VIcon agrees with the same one-receive/one-message contract, including zero-length
  messages, preserved boundaries under coalescing, no stream EOF synthesis, and one message per send
  request. This is transport framing, not VIcon/VProlog grammar. That exchange confirms the finding
  is generic rather than a Prolog-specific continuation demand.
- **Technical writer:** The writer independently agrees that §6.2 only distinguishes payload length
  from EOF and §6.5 does not specify one message per request. The proposed correction makes
  admission, framing, and message identity explicit without freezing frontend syntax.
- **VProlog needs / other frontends:** The requirements for a parked await, typed terminal outcomes
  distinct from logical failure, submit-vs-observe identity, choice/trail lease ownership,
  endpoint-owned unread suffixes, and flush-before-read are stated by the frozen target (§4, §6.5,
  §§7-8). VQL/VPL's request-reference correlation and the VAgent/VLace distinction between staging,
  admission, transfer, flush, and close are preserved. No further VProlog finding arises from those
  shared concerns.

## MINOR

None.

VERDICT: NOT CONVERGED
