# Round 1 critique: Wildcard (vortex-io-redesign.md)

Target: v0.9, SHA-256 `a6325b19e99bdec6f7f6cb6724ed7a6b5ddecf1e7152e345194ae6872eb58535`.

Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of pre-freeze dialogue

- VQL confirms independent request identities/outcomes plus adapter-owned row association are
  sufficient; query limit or short-circuit stops future submissions but does not cancel admitted
  requests. No generic group guarantee is required for VQL.
- VPL agrees group identity is unnecessary for array shape/order. It needs lowering to preserve
  read-ahead suffix/source cursor when an array view or early termination consumes only part of a
  framed read; generic transport owns bounded suffix/cursor leases, and VPL owns pure decode/lexer
  state.
- The technical writer independently agrees with the request-ref-rank approach: introduce generic
  group identity only for genuinely shared deadline/accounting/bulk-selection semantics, retaining
  per-member results and no atomicity/undo implication. The writer also called out prompt
  flush/read, duplex progress, and flush/sync/ack distinctions; these are explicit in v0.9.

## BLOCKING

None.

## MAJOR

### 1. (§6.5, lines 556–616; §11.2, line 948) Specify message transport boundaries

The protocol separates an empty payload from EOF and requires a direct/future-frontend trace for
“zero-length datagrams versus stream EOF” (§6.2, lines 388–391; §6.5, lines 613–616). However, the
normative operation/framing description is byte-oriented: `Read { offset, maxBytes }`,
unbuffered/fixed/line-delimited reads, and an “adapter-supported frame policy” (§6.5, lines 469–482,
556–562). The required experiment lists “framed reads” and “raw/frame/seek/reset mixing” (§11.2,
line 948), but does not define whether a message/datagram endpoint produces exactly one message per
request or whether boundary metadata is retained. The current generic outcome can carry an empty
binary payload, but the spec does not say that an empty datagram is one successful message, that
message transports need not produce EOF, or that a send request names one message. An implementer
could flatten messages into byte chunks or interpret a zero-byte receive as stream termination while
still satisfying the listed byte framing rules.

**Concrete correction:** Define a message-oriented endpoint/request capability alongside the byte
stream modes. One successful receive yields exactly one message payload, including a valid
zero-length message; message boundaries and per-message request identity survive buffering and
coalescing; stream EOF is not synthesized for message transports. One explicit message-send request
names one message and retains its own progress/outcome. This need not make every backend support
messages: adapters lacking the requested message semantics return a typed unsupported-capability
refusal. Add a trace with two receives (first zero-length, second nonempty) and two sends to the
acceptance gate. Keep transport framing generic and grammar/token parsing frontend-local.

## MINOR

None.

## Cross-role dialogue

The VQL reviewer asked whether the direct-Vortex dashboard cohort implies a generic group guarantee.
It does not: an owner cell/rank of request references, per-request cancellation, and bounded
per-request retention suffice unless a later host proves a shared deadline/accounting operation is
needed. This matches the v0.9 rule at §6.5, lines 540–544. VPL confirmed this does not affect its
position mapping and clarified that early array consumption releases VPL's own view/decoder state,
not transport-owned read-ahead suffixes. These exchanges confirm no group-identity finding.

I asked the VProlog reviewer whether the draft's generic framing vocabulary makes message boundaries
normative. The reviewer agrees that a zero-length datagram is one successful message, not EOF, and
that message endpoints should declare the capability while stream-only endpoints typed-refuse it.
This confirms the proposed correction and keeps Prolog grammar outside the service. I also asked
VIcon, VLace, VAgent, and the technical writer; their responses were pending at report write time.

## VERDICT: NOT CONVERGED

One MAJOR remains: message/datagram boundary semantics need an explicit generic contract or explicit
capability refusal, even though support need not be universal.
