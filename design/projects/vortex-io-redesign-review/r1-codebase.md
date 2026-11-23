# Round 1 critique: Codebase Expert (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 2 MAJOR, 0 MINOR.

## BLOCKING

### 1. (§7.1, lines 316–327; §11.1, line 526) Existing runtime changes have no Proposal Register

**Problem:** The design requires extending the VM/host stepping protocol with a Pending result and
await continuation, and its migration plan explicitly says to extend VM/host stepping. It also
requires replacing byte-returning payload resolution (§5, lines 203–205; §8.1, lines 368–371) and
adapting existing language/runtime bridges (§4, lines 176–189; §6.4, lines 304–310). These
necessarily touch existing runtime/core/stdlib files, but the draft has no numbered Proposal
Register. The canonical workflow makes an unregistered existing C++ runtime change BLOCKING. The
design correctly identifies that `VortexCore` is not the VM: `VortexHost` coordinates both
(`apps/common/xanadu/vortex/vortex_host.hpp:66–70`), while execution methods are on `VortexVM`
(`apps/common/xanadu/vortex/vortex_vm.hpp:672–676`). The register should name exact touched files
rather than an ambiguous “VM/host” scope.

**Evidence:** The proposal says, “The VM must retain its cursor, frames, output bindings, guards,
and execution phase needed to resume the same await” (`design/vortex-io-redesign.md:318–319`) and
“Extend VM/host stepping for Pending and marked await” (`design/vortex-io-redesign.md:526`). The
current public execution surface is `ExecutionResult step(...)`, `run(...)`, and
`stepScheduler(...)` (`apps/common/xanadu/vortex/vortex_vm.hpp:672–676`); `run` steps synchronously
until completion, failure, cancellation, or cycle exhaustion
(`apps/common/xanadu/vortex/vortex_vm.cpp:4854–4940`). `ExecutionResult` has no pending state: it
contains only success, contract-violation, failing-clause, and error-message fields
(`apps/common/xanadu/vortex/vortex_core.hpp:72–77`). Payload reads currently use
`textOf(..., SpanReader*)` and skip non-scratch spans absent a reader
(`apps/common/xanadu/zigzag/arena_manifold.hpp:734–740`), so transient resolver-aware payload cells
need a concrete existing-file proposal too. No Proposal Register or numbered P entry appears in the
target.

**Fix:** Add numbered proposal(s) before implementation. At minimum, explicitly enumerate the
`VortexVM` result/continuation and owner-pump changes, the `VortexHost` integration, and the typed
cell payload resolver/lifetime boundary, with exact files, why the behavior cannot live in a caller
or Vortex, estimated state/byte and scheduling costs, and a fallback if declined. Add separate
entries for existing language bridges only where migration changes their code; distinguish the
VAgent adapter from Evidence policy. Keep the public program interface cell-only as already required
in §1.

## MAJOR

### 1. (§7.2) Allocation incarnation has no current arena hook

**Problem:** The ABA warning is correct, but this section only requires a future spike to prove
hooks. Existing refs encode an ephemeral bit plus a dense allocation index, and `contains` checks
whether that slot exists. `release` truncates the slots created under the mark, so the same dense
index can be minted again. Existing `ArenaManifold` has no general finalizer/lease notification
contract established by these APIs. The document should make the missing lifecycle hook and an
implementation decision a gate, rather than leaving the design requirement at “a spike must prove
the hooks and exception paths.”

**Evidence:** “A result cache needs exact allocation incarnation or release-aware invalidation” and
“A spike must prove the hooks and exception paths” (`design/vortex-io-redesign.md:335–343`). `refOf`
constructs refs from `ephemeralBit | dense` and `contains` delegates to `slot`
(`apps/common/xanadu/zigzag/arena_manifold.hpp:654–664`). `release` is explicitly documented to
remove marked cells (`apps/common/xanadu/zigzag/arena_manifold.hpp:843–849`) and truncates slots,
links, content, and scratch to the saved mark
(`apps/common/xanadu/zigzag/arena_manifold.cpp:2582–2605`). The public arena surface exposes
mark/release/discard but no lease-finalizer callback
(`apps/common/xanadu/zigzag/arena_manifold.hpp:840–877`).

**Fix:** Make the transient integration gate choose and price a concrete ownership design: e.g.
generation-bearing managed handles with release-aware roots, or an external lease registry keyed by
arena identity plus allocation incarnation and updated from every mutation/rollback path. Enumerate
all relevant existing files as a numbered proposal if arena changes are required; spell out how
overwritten cells, trail snapshots, saved slots, frame ports, and exceptions transfer/release
ownership. Do not let `contains(ref)` alone stand for incarnation validity.

### 2. (§8.1, §9.2) Capture route through promotion and storage is unresolved

**Problem:** The draft correctly says transient ranges cannot be treated as scratch/permanent spans
and capture must preserve overlap identity. However, it does not specify how selected transient
bytes enter the one permascroll, how translation metadata survives a crash between append and
Structure publication, or how `ArenaManifold::promote` consumes that new span kind. The callout to
choose a journal representation is good, but the journal and atomicity order are prerequisites to
claiming a capture result is safe to publish. This is a material gap in the current storage API, not
an existing API the proposal can call.

**Evidence:** “The payload resolver must make these ranges usable as cell content” and “Do not
overload scratchScroll” (`design/vortex-io-redesign.md:368–371`); “The spike must choose the capture
journal/recovery representation and prove ordering between permascroll append, translation
commitment, and Structure publication” (`design/vortex-io-redesign.md:467–469`). Current arena
content is a run of `PrimediaSpan`s; `makeCell(PrimediaSpan)` quotes an already-addressed span,
while `makeCell(string_view)` stores scratch bytes
(`apps/common/xanadu/zigzag/arena_manifold.hpp:747–758`). Promotion reads spans, carries quoted
content across stores, and emits persistent Structure operations
(`apps/common/xanadu/zigzag/arena_manifold.cpp:2891–2955`); it has no transient source range or
translation-journal contract. The store does not contain primedia; the canonical repository
instructions state it uses the author's `UserPermascroll` and stores edit decisions.

**Fix:** Add a capture design gate that names the permascroll writer/owner and gives the durable
ordering/recovery protocol (including append-success/map-save failure and ambiguous publication),
then specifies how the resolver or promotion maps transient intervals to durable spans without
content deduplication. Register exact existing storage/promotion files in a numbered proposal if
they must change, with byte/metadata cost and the no-capture fallback on journal ambiguity. Preserve
the draft's no-format-change claim only if the selected journal and mapping need no new on-disk
shape.

VERDICT: NOT CONVERGED
