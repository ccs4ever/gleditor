# Round 7 Disposition

Target: `design/vortex-stdlib-prebuilt-store.md`, revision 6 -> revision 7.

All six reports are filed as `r7-{writer,vortex,codebase,architect,ergo,zigzag}.md`. These files are
finding-preserving condensed records of the returned reports; their original severity labels remain
visible, including disputed ones. Raw tallies: 13 BLOCKING, 22 MAJOR, 38 MINOR. These are duplicated
critic findings, not counts of distinct accepted defects. No critic converged.

## Source Provenance

The target is **not** the terminal's default repository. The review worktree is:

```text
/home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan
```

HEAD remains `9d91bfdcbb86517b30a120ceac1d8fa6f9607489`, with substantial unrelated working-tree
implementation edits preserved. HEAD alone is not a source snapshot. The following working-tree blob
hashes identify the orchestrator's verification:

| Source                      | Git Blob Hash                              |
| :-------------------------- | :----------------------------------------- |
| `vortex/vortex_stdlib.cpp`  | `d6e435843e3ad72c91e0180c6de2ee2eaa29f52f` |
| `vortex/vortex_vm.hpp`      | `e8e805f41522dd8a1fb17cdfdc903c93cbdab98a` |
| `vortex/vortex_vm.cpp`      | `839db79d039cc618bd8a708c2890483e12003a1f` |
| `vortex/vortex_core.hpp`    | `3ebada232f51734d010c493eba41b3e45d3016a6` |
| `vortex/vortex_core.cpp`    | `153bb30c28fd232bf60d363960c5aa52a89ed80b` |
| `vortex/vortex_host.hpp`    | `e316111ed9f5a181531a2d8507139999b9ce1518` |
| `zigzag/arena_manifold.cpp` | `e71d307ace1eaaf895020e35790880677c45f540` |
| `zigzag/arena_manifold.hpp` | `d1521e316d4d213a7cd1f23e20c2945281b86fc2` |

All source paths in this table are under `apps/common/xanadu/`. The visualizer at
`apps/common/ui/slice/zigzag_visualizer.cpp` hashes to `728d8c5c762e14f546877b2082f3494c75b12abc`.

The codebase critic's module count, absent-helper claims, and citation map match the other checkout
at `/data/git/gleditor-restore`: its bootstrap begins at line 1459, whereas the target worktree's
bootstrap begins at 1507. Those findings cannot be applied to this design's baseline. Future critics
must use absolute target paths and verify the worktree before reviewing. This is a review-provenance
failure, not evidence that target-worktree symbols were fabricated.

## Accepted Clusters

### R7-1. Gate before document mutation, plus direct-attach and command checks

Sources: writer B1/M1, vortex B1, architect B1/minors 1/3/5/8, ergo B1.

Verified `bindXanaduStore` resets the engine at `zigzag_visualizer.cpp:364` and only ensures the
host at `:392`; `adoptDocument` replaces the owned store at `:244-247`, engine at `:267`, and
ensures at `:306`. Direct `attachVortexHost` assigns without ensuring (`:2844-2848`). An
ensure-arm-only check is too late.

Revision 7 requires checks before the first mutation in both document paths, before direct host
assignment, and at the command replacement arm. Wrappers, reload, pane switching, focus restoration,
and bind bookkeeping must honor refusal. The installed host, engine, store, focus, and document
remain consistent. External references after a successful replacement retain their separate,
explicitly non-owning-base lifetime restriction; that limitation cannot excuse a dangling installed
host after refusal.

**Policy correction:** revision 6's proceed-on-retained command execution is not retained. Commands
refuse and skip execution with their existing failure result types and visible feedback. This avoids
constructing a transient compiler/stdlib under the very suspended marks that prohibit registration.
Safe coexistence would need a separate isolation contract; a test asserting it without a mechanism
is not a fix. The suspended flow resumes through its owning runner/embedding API.

### R7-2. Host-side aggregate and explicit synthetic-floor state

Sources: writer M3, vortex M1/minor 2, architect M1/M4, ergo minor 3, zigzag M1/M2, codebase
B3/B6/M14/M15 (behavior accepted, wrong-tree cites rejected).

`extraContexts_` is private (`vortex_host.hpp:350`); the primary accessors do not enumerate it. P30
now proposes public `VortexHost::liveMarks()` with internal aggregation, not a new UI-side context
iterator.

The hydration seed is evictable (`arena_manifold.cpp:2196-2203`), and floor-stack size is not a
baseline. Rather than leave an unenforced never-hydrated convention, P30 adds arena-internal
`hasHydrationFloor_` and `ArenaManifold::liveMarks()`. Hydration sets the flag; oldest-floor
eviction clears it; removing the sole remaining synthetic floor clears it; reset clears it; moves
transfer it with the stack. Ordinary real marks do not change it. The count excludes only a
still-present seed. This is allocator lifetime state, not manifold metadata. Tests cover seed
eviction under real marks, ordinary nesting, reset, and moves.

### R7-3. Real result channels and visible feedback with a named writer

Sources: writer M2/minor 2, architect M2/M3/minors 3/5/7/8, ergo M2/minors 2/3, vortex minor 3.

Private feedback fields have no external setter. Their draw is inside `if (commandBarVisible_)`
(`zigzag_visualizer.cpp:2393-2455`). Refusing visualizer members therefore write feedback/error
internally and open the command bar, including startup refusal. `bindZigzag` consumes expected and
returns expected; it does not assign private fields. Its lens branch, all callers, boot/adoption
sinks, reload wrappers, and tests are included in P30.

Commands keep bool/vector/message-bearing result types, not fictional expected signatures. Refused
replacement skips command execution, with an empty/false/error result plus visible cause and
recovery. Chainable fallible host/visualizer mutators return expected of the object pointer.

### R7-4. Complete VM state clearing and a concrete guard lifetime mechanism

Sources: vortex M2, architect M5, writer minor 6, zigzag minor 5, codebase B7/minor 16.

Verified `guardInstalled_` records historical installation, not current ownership. The destructor is
conditional, but a dying historical installer can clear a later VM's guard. Reinstalling only at
arena resync leaves mid-session gaps and ignores disjoint maps of simultaneously live VMs.

P30 now specifies owner-keyed live guard predicates inside `VortexCore`: registration is idempotent
per VM, destruction unregisters only that VM, and writes test all live predicates. This supersedes
the unresolved reinstall-or-token alternatives. A lone current-owner token cannot restore/protect
surviving predecessors by itself. The core header and implementation, VM lifetime code, costs,
lifetime/serialization contract, and two-destruction-order tests are explicitly accounted for.
`choiceStack_` and `oracleCursor_` join the swap clear list. No implementation is made in this
review.

### R7-5. Resolve diagnostics and overload contracts honestly

Sources: writer M4, ergo M3, architect minor 4.

The no-entry-throw statement now names solver methods only. `call(path,args)` resolves then
delegates or returns its existing empty result; `call(CellRef,args)` keeps its use-point throw.
Status remains solver-goal-only. Recovery no longer names nonexistent `std:goal/resume`; it names
the owning runner and actual embedding APIs `goalResume`/`goalResults`.

### R7-6. Selective generator contract, dependency checks, and output semantics

Sources: vortex M3, architect minor 6, ergo minors 4/7.

The self-contained-set restriction and nonzero unresolved-symbol failure already existed in revision
6; treating dependencies as unspecified was partly overstated. Revision 7 makes the mechanism
explicit: find/mint the module cell before dispatch, repeatable module flags, canonical bootstrap
build order, checked required-symbol lookup, closed callable/relocation referents, no implicit
shipped-library dependency closure, and failure before publication. Goal alone must fail if logic is
required; selecting both can satisfy the dependency. Included modules are represented by the image's
module rank.

The constructor tag becomes `Bootstrap::Manual` to avoid collision with local `suppressed` status.
Its optional source is `nullopt`, not an extra enum member. One invocation emits one image; a
nonempty output directory is refused as `OutputExists`, never overwritten or merged. Regeneration
uses a fresh directory.

### R7-7. Rollback, scan scope, citations, and configuration wording

Sources: writer minors 1/3/4/5/7/8/9; architect minors 2/4; vortex minors 1/4; ergo minors 1/5/6;
zigzag minors 1/2/3/4; codebase B2/M10/M12/minors 17/18/19.

- Separate six unguarded command ensure sites from null-guarded `compileVQL`; use
  `translateVQLAndAttachToFocus`, not `navigateCellVQL`.
- Describe `overlay_` as mark-restored through tracked shadow entries, not wholly untrailed.
  Positively exclude dimension binding, store attachment, and guard installation inside the graft
  mark; keep side-map assertions.
- Cite arena link/rank implementations for arena graft behavior.
- Limit `findOpHandle` ambiguity to folded `Manifold` scans, including later promoted/folded grafted
  content; there is no arena counterpart.
- Refresh executor/mesh-evaluator/helper and prefix ranges against the target; retain correct
  target-worktree stdlib and vagent ranges.
- Describe vicon's driver ownership by value; name the vquery VM correctly.
- Explicitly append the image leaf to asset-root chain levels.
- Emit startup decisions at INFO from `ensureStdlib` in all modes; only report an attempted local
  directory when its resolution chain actually ran.

## Rejected Wrong-Tree Findings and Severity Corrections

Codebase B1, B4, B5, B8, and B9 are refused against the target snapshot: it has 21 modules, both
named constants and the predicate helper, a lineage skip, and the cited vagent image-record range.
The helper's *definition* citation is clarified to the header; the cpp citation names its caller.

Codebase M11/M13/M14/M15 and minor 16/17 citation replacements are not applied wholesale: their line
map is for the other checkout. Genuine mechanism/ownership issues are accepted in the clusters
above; target lines are verified independently. Line-only drift, including codebase B9, is not
intrinsically a blocking architecture defect. Codebase minor 20 is a positive verification record
and needs no change.

No accepted implementation issue is deferred without a v1 contract. Command coexistence, external
retained-host ownership redesign, and interactive local-image authoring remain outside this
migration's promised behavior.

## Next Gate

Revision 7 requires fresh six-role review, particularly of the new explicit hydration bookkeeping,
owner-keyed guards, command skip policy, and caller ordering. Use absolute worktree paths. Report
only substantive BLOCKING/MAJOR findings plus at most five MINOR items; do not prolong review by
promoting citation polish.
