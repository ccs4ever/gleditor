# Round 3 critique: Vortex & Spool-Mesh Expert (`design/vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root.
Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R2.1 (Delta Store Parentage Contradicts Store): RESOLVED — Replaced parent spool assertion with
  segment-0 dimension seeding (44 `VortexCore` + 13 `vagent` dimensions) via
  `zigzag::promoteOverlay` (§1.3, §3.1.2, §5 P17.1), satisfying `Store::requireCellOp` locally
  without modifying `OpsSegmentHeader`.
- R2.2 (Contradictory Upgrade Context Mechanics): RESOLVED — In-place base pointer swapping
  eliminated; upgrade allocates a fresh `WorkerContext` over $V_2$ (§4.3, §5 P17.4, §8.2),
  deep-transcribes activation frame locals on `d.vars`/`d.values` (§4.3.4), and explicitly
  re-initializes `actor->cursor` to the continuation entry in $V_2$ (§5 P17.4).
- R2.3 (MeshActorRuntime::upgrade Signature Contradictions): RESOLVED — `wallNow` parameter aligned
  with `MeshActorRuntime` conventions as `std::chrono::system_clock::time_point` (§5 P17.4); grant
  transition executes atomically at a quiescent turn boundary.
- R2.4 (ProgramManifoldResolver Omits SpanReader): RESOLVED — §5 Proposal P17.3 specifies passing
  `spanReader` to `WorkerContext(programManifold, spanReader)` during spawn and resume, backed by
  the LRU program store cache in `LocalHost` (§2.1).
- R2.5 (Conflation of Actor Mailbox and In-Flight State): RESOLVED — §4.2 clarifies that message
  queuing is managed in `MeshActorRuntime::Actor::mailbox`, and §4.2 #3 / §4.3.4 mandate that
  replanned or aborted intentions drop pending replies, stage `$cancel` to remote delegates, and
  track `$stats.reply_stale` to prevent stale reply injection.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§1.2, line 81) Retained phantom opcode names in motivation section

Full title: Retained phantom opcode names #Unlink and #Step in §1.2

**Observation:** Section 1.2 line 81 states: "Every `#Link`, `#Unlink`, and `#Step` instruction
takes its dimension operand as `cInt(static_cast<std::int64_t>(dim))`." While canonical opcodes were
adopted across §1.1 and §2.2, `#Unlink` and `#Step` do not exist in `OpcodeKind`
(`apps/common/xanadu/vortex/vortex_vm.hpp:27–91`), which uses `OpcodeKind::Break` and VM cursor
progression.

**Recommendation:** Update the prose to reference `#Link` and `#Break` for lexical precision.

### 2. (§5, lines 477–483) ProgramManifoldResolver bundled SpanReader return

Full title: ProgramManifoldResolver could return a bundle with SpanReader

**Observation:** Proposal P17.3 defines:

```cpp
using ProgramManifoldResolver =
    std::function<const zigzag::Manifold *(std::string_view programRef)>;
```

while line 482 states that `spawn` and `resume` construct
`WorkerContext(programManifold, spanReader)`. Because `xanadu::Store` implements `SpanReader`, the
`LocalHost` cache holds both.

**Recommendation:** For C++ type clarity, consider returning a bundle:

```cpp
struct ProgramBundle {
  const zigzag::Manifold *manifold{nullptr};
  const xanadu::SpanReader *reader{nullptr};
};
```

so `spanReader` does not require out-of-band resolution.

______________________________________________________________________

VERDICT: CONVERGED
