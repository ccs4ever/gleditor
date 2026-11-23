# Round 1 critique: VIcon (vortex-io-redesign.md)

Paths below are relative to the repository root. Frozen target: v0.9, SHA-256
`a6325b19e99bdec6f7f6cb6724ed7a6b5ddecf1e7152e345194ae6872eb58535`.

Tally: 0 BLOCKING, 2 MAJOR, 0 MINOR.

## Current VICON behavior checked

VICON compiles generators to Vortex choice points. A later `#Fail` releases the mark and continues
at the saved alternative; `every` drives the generator, while a statement-level expression is
bounded (`design/vicon-goal-directed-language.md:194-263`). The compiler's `write(e)` builtin yields
its value at the driver's `Halt`; nested `write` tees a yield through that same halt before resuming
the consumer (`apps/common/xanadu/vicon/compiler.cpp:793-822`). It does not itself call `std::cout`.
The standalone runner prints each yielded value to stdout (`apps/vicon/main.cpp:41-46,138-152`), so
the VM yield and a runner's external output sink are distinct operations.

The current `Driver::resume()` returns `optional<CellRef>`; `nullopt` means no produced value, and
`every()` loops until that result (`apps/common/xanadu/vicon/driver.cpp:36-63`).
`goalProduce`/`goalResume` accept only the designated `haltOp` as a yield and otherwise unwind or
exhaust (`apps/common/xanadu/vortex/vortex_stdlib.cpp:2523-2572`). The frozen proposal correctly
adds a distinct Pending execution result and requires preserving cursor, frames, bindings, guards,
and execution phase across the same await (`design/vortex-io-redesign.md:620-633`). VICON's
choice/backtrack state belongs to its activation; it is not generic service request state.

The VICON lens bridge remains a proposal, not a landed async implementation. The frozen design
correctly places VICON syntax/effect policy in its frontend row while keeping capability, request,
outcome, and payload handling in the shared cell protocol (`design/vicon-lens-bridge.md:11-20`;
`design/vortex-io-redesign.md:462-511,552`). Generic async lens open/page/refresh/commit and bounded
file/media ranges can therefore be used by a VICON adapter; they do not require VICON-named VM
opcodes or compatibility shims.

## BLOCKING

None.

## MAJOR

### 1. (§6.5, lines 556–616; §11.2, line 948) Specify message-mode request boundaries

The VICON example exposes a byte `read(0,4096)` and says await observes the same request; the shared
rules distinguish an empty payload from EOF and permit an adapter-supported frame policy (§6.5,
lines 470-507, 552, 556-599). The only normative mention of datagrams is a required test scenario,
“zero-length datagrams versus stream EOF” (§6.5, lines 613-615; §11.2, line 948). These statements
do not specify that one successful message receive returns exactly one whole message, that a
zero-length datagram is successful data with no synthesized EOF, or that one send request names
exactly one message. If VICON uses a message endpoint, an adapter could flatten/coalesce messages or
map a zero-length receive to stream end while still satisfying the byte-oriented request and generic
empty/EOF distinction. This affects VICON code consuming arbitrary endpoints; it is a generic
transport contract gap, not a VICON grammar requirement.

**Concrete correction:** Add a message-oriented endpoint/request capability beside byte-stream
framing. Require one successful receive per whole message (including a zero-byte message), preserve
message boundaries and independent request identities across buffering/coalescing, and never
synthesize stream EOF for message endpoints. One explicit send request names one message and keeps
its own outcome/progress. An adapter without that capability should return a typed unsupported
refusal. Add a trace with a zero-byte message followed by nonempty data; do not require every
adapter to support message transport.

### 2. (§6.5, line 552) Separate VICON yield/render from stdout stream submission

The VICON row says “Yield/print alone performs no I/O.” Current source supports the narrower claim
that the VM `write(e)` builtin yields a value without itself writing bytes: the CLI sink then writes
the result to `std::cout` (`apps/common/xanadu/vicon/compiler.cpp:793-822`,
`apps/vicon/main.cpp:41-46,138-152`). The draft also says runner stdin/stdout and host renderers are
adapters (§6.5, lines 608-616), while the shared contract says render/display is observational (§4,
lines 240-250) and demonstrates explicit asynchronous `WriteBytes`, `FlushAfter`, and `ReadLine`
requests (§6.5, lines 513-531). As written, “print” conflates the VM's host yield or a pure UI
render with the runner's external stdout write. An implementer could retain synchronous stdout
output inside the resume/render callback and mistakenly treat it as outside the async contract, or
remove it as an unsupported effect.

**Concrete correction:** State that a VICON yield only offers a cell to its host and pure rendering
only formats/displays an already available value. When a runner chooses an external stream such as
stdout, it submits a frozen write request and observes its own completion/flush outcome through the
shared protocol; explicit `await` resumes the same VICON activation, while re-executing that write
submit after backtracking is new work. A short VICON-shaped proposed write/flush example would make
this boundary reviewable without freezing grammar or ABI.

## MINOR

None.

## Cross-role dialogue

- **VProlog:** I asked whether the frozen generic wording defines message boundaries. VProlog agrees
  it does not: one successful receive should be one whole message, including zero bytes; message
  boundaries must survive coalescing; message endpoints do not synthesize stream EOF; one send
  request names one message; unsupported adapters typed-refuse. VProlog also confirms the VICON
  Pending boundary: generic Pending carries no frontend stack, while VICON retains its cursor and
  choice state. The explicit unread-suffix/cursor rules in frozen §6.5 and the lease rules in §7
  resolve the earlier read-ahead/abandonment question; I found no additional VProlog-specific gap.
- **Wildcard:** Its R1 report records the same message-boundary MAJOR. I agree: the required
  datagram experiment is not itself a protocol definition. The wildcard reviewer asked whether
  adapter frame policy is sufficient; I answered that it is not normative without a message mode or
  a typed refusal contract. This confirms finding 1, not a separate VICON-specific issue.
- **Technical writer:** The writer asked the same sufficiency question. I answered that empty-vs-EOF
  and adapter-supported framing do not require one message per receive or preserve message
  boundaries; a generic message capability/refusal is needed. The writer agreed that message mode is
  unclear. On the separate yield/stdout issue, the writer confirmed that “Yield/print alone performs
  no I/O” is too broad: VICON `write` yields for host presentation, which may write stdout, but does
  not itself submit to IoService. Rendering an existing cell is observational; external stdout
  output must be named as host I/O. This dialogue confirms finding 2 and narrows its fix to spelling
  out the distinct yield, renderer, and stream-submit boundaries.

## Coverage checked; no remaining VICON finding

The frozen draft makes fresh submit per re-execution and same-request await explicit (§4, lines
230-252; §6.5, lines 484-491, 552), so it matches VICON's generator/backtracking model. A Pending
await is neither yield nor failure, preserves activation state, and resumes at the same request
(§7.1); materialized result cells may backtrack while the retained request observation survives and
can be rematerialized (§7.2). Typed EOF, empty payload, partial progress, cancellation, rejection,
overflow, failure, and unknown status are not collapsed into ordinary generator exhaustion (§6.5,
lines 473-507). Buffering modes, bounded line/frame/token sizes, read-ahead suffix ownership,
frontend-local tokenizers, flush barriers, close/abort outcomes, and payload leases are specified
generically (§6.5, lines 513-599; §§7-8). The owner has explicitly released ABI compatibility
constraints (§1, lines 52-60); neither finding asks for a shim or VICON-specific core opcode.

## VERDICT: NOT CONVERGED

Two MAJOR findings remain: message-oriented receive/send semantics are not normative, and the VICON
row blurs a yielded value/pure render with a runner's external stdout write.
