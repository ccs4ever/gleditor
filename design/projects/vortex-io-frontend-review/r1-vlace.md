# Round 1 critique: VLace (vortex-io-redesign.md)

Target verified: v0.9, SHA-256 `a6325b19e99bdec6f7f6cb6724ed7a6b5ddecf1e7152e345194ae6872eb58535`.

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Current VLace behavior checked

VLace's current `IO` is an action-description encoding with synchronous interpretation, not an
asynchronous request protocol. `lowerIoBuiltin` constructs `IO.Pure`, `IO.Bind`, `IO.Then`,
`IO.PutStr`, `IO.PutStrLn`, and `IO.GetLine` nodes (`apps/common/xanadu/vlace/lower_io.cpp:75-83`);
`lowerIoBind` forces each operand to WHNF as it builds the bind node (`:54-73`). `runIoOnVm` keeps a
local `std::vector<Continuation>` (`:96-142`), writes to the host stream inline (`:151-167`), calls
`std::getline` inline (`:168-185`), and invokes other effect leaves and their VM blocks inline
(`:186-212`). This is not a parked VM continuation.

The IO constructor tags are reserved and the opaque subset cannot be user-constructed, preventing a
forged `IO.*` node from smuggling an effect into `runIO`
(`apps/common/xanadu/vlace/ast.hpp:234-257`). The type checker gives `openLens` and the lens
gestures `IO (Result ...)` types (`apps/common/xanadu/vlace/types.cpp:170-209`), but there is no
static effect-placement pass in `Driver::run` (`apps/common/xanadu/vlace/driver.cpp:22-49`);
`vlace --no-typecheck` also exists (`apps/vlace/main.cpp:131-134`).

The runner detects an IO-typed result from `driver.run` and then calls `driver.runIO`
(`apps/vlace/main.cpp:34-57`). The REPL instead renders the result of `driver.run` per line and has
no resumable IO session (`:62-99`). `driverEvaluateIO` creates the arena, VM, and compiler as local
variables, then runs and interprets the complete program before returning
(`apps/common/xanadu/vlace/lower_io.cpp:284-350`). So the target is right to require a new resumable
driver state rather than treating today's handler call or trampoline as asynchronous.

Current lens operations also run synchronously: `IO.OpenLens` opens a URI and constructs a
`ProjectionSession` during `performEffectLeaf`
(`apps/common/xanadu/vlace/lower_effects.cpp:288-350`), and gesture leaves call session operations
inline (`:352-385, 401-430`). The target accurately labels this as an implemented but blocking
IO/lens bridge (`design/vortex-io-redesign.md:212`) and its migration row requires resumable driver
state (`:221`).

## BLOCKING

None.

## MAJOR

None.

## VLace coverage assessment

The target assigns syntax, typing, purity/effect placement, multiplicity, lowering, and
frontend-local continuations to each language while keeping core/VM policy language-neutral
(`design/vortex-io-redesign.md:344-355`). For VLace, the generic rules require explicit checked
submit sites and frontend-defined multiplicity (`:240-260`); the VLace sketch says description
construction/rendering is inert and its driver owns suspension (`:551`); the shared Pending section
requires the VM to park and resume the same await without re-entering submit (`:620-635`). That is
the right layer split.

The current host stack in `runIoOnVm` and short-lived compiler in `driverEvaluateIO` mean a future
VLace implementation must retain its IO bind/then suffix, forced values/thunks and language-local
driver session while Pending, in addition to the generic VM cursor/frame state. The target assigns
frontend continuation state to the adapter and includes pending continuation values and result roots
in resource accounting (`design/vortex-io-redesign.md:344-355, 681-694`). It also distinguishes
dropping an awaiter from cancelling admitted work and retains the admitted request under service
ownership (`:502-507, 647-665`). I therefore find the architecture sufficient for this round; the
later implementation gate must prove the VLace host actually keeps that state rather than
reparsing/re-running the action.

The language is call-by-need, with thunk memoization, and effects force to actions rather than
performance in its design (`design/vlace-functional-language.md:616-650`). The target's pure local
buffer and explicit `perform` rule (`design/vortex-io-redesign.md:564-571, 601-606`) protects that
property: force, render, or pure pattern-match must not admit or resend. Completion is typed data,
with EOF separate from empty payload and partial outcomes retained (`:502-507`); payload
ranges/leases and capture separation are specified for cells (`:363-390, 706-801`). Lens commits
remain declarative transactions with source version/conflict semantics rather than a sequence of
stream writes (`:446-458, 763-786`). These are sufficient design constraints; exact VLace outcome
constructors, buffer type, and effect syntax remain proposed and are not ABI commitments.

## Cross-role dialogue

- **VAgent:** VAgent answered the pre-review question by separating two freezes: an actor intent
  fixes semantic bytes and endpoint order before commit can expose it; after commit, the adapter
  transfers a bounded immutable lease and durably records attempt metadata before service work.
  VLace's explicit `perform` remains a different admission point. Asked how a lazy buffer
  distinguishes flush from close after the bind suffix is abandoned, I replied that
  `perform(flush localBuffer)` creates a write request; a service `FlushAfter` is a separate ordered
  barrier; `close` and `abort` are explicit later lifecycle operations. If a scope form promises
  cleanup, VLace-local finalizer state must survive Pending/abandonment; GC cannot block or imply a
  successful close. The admitted request's lease and partial/unknown outcome remain service-owned if
  its local continuation disappears. This exchange sharpened the report's MINOR wording finding; it
  did not uncover a missing shared lifecycle contract
  (`design/vortex-io-redesign.md:564-580, 601-606`).
- **Technical writer:** The writer asked whether `perform(flush buffer)` means a local-buffer write
  request or a service flush barrier. VAgent confirmed the frozen text’s intended mapping: the
  former is one fresh admitted buffered-write request; `FlushAfter` is a distinct barrier over
  already-admitted requests, and close/abort are separate lifecycle requests. Lines 564–580 and
  601–606 plus the prompt trace at 513–531 state those distinctions. This exchange changed my
  provisional wording concern into no finding; no target edit is required for the buffer boundary.
- **Wildcard / VProlog / VIcon:** Wildcard's zero-length-datagram case is a generic
  message-transport gap: an empty payload differs from EOF, but that alone does not define
  one-message-per-receive or preserve message boundaries. I endorse the generic correction: optional
  message-mode capability, one whole message per receive (including zero length), no synthetic
  stream EOF, one identity/outcome per message send, and typed refusal when unsupported. This is not
  a separate VLace finding because VLace framing/tokenizers remain frontend/adapter policy and no
  VLace example claims message support. VProlog and VIcon independently concur that token grammar
  stays frontend-local while message boundaries belong to the transport contract. No group identity
  is needed for this issue.
- **VQL/VPL:** The shared request-cell collection and frontend-owned row/array association preserve
  operation identity independent of completion order (`design/vortex-io-redesign.md:539-544`). This
  does not change VLace's sequential IO-bind semantics; it confirms no generic batch opcode is
  needed for VLace.

## Severity disposition

No unresolved VLace-specific issue remains. I endorse the wildcard’s generic message-boundary
finding as a shared transport requirement, not a separate VLace finding; optional capability/refusal
keeps stream-only adapters valid and token grammar frontend-local. No production ABI or syntax
commitment is implied.

VERDICT: CONVERGED
