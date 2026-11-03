# R3 critique — Implementer usability (ergo lens)

**Target:** `design/vagent-metaprogramming-implementation.md` **v3** against spec
`design/vagent-metaprogramming.md` v1.19 and the tree at HEAD.

**Verdict: CONVERGED** — every r2 finding of mine closed with correct mechanics and accurate
cites; the r2-disposition minor bundle is fully applied. One MINOR placement omission (the
genesis driver's TU home) and two NITs remain — polish, not sequencing holes. The plan is
executable end-to-end by an engineer who hasn't read the spec: every phase's **Requires** is
honest about kernel-simulable vs adapter-gated, and every **Exit** names checkable behavior
against machinery its own phase delivers.

Counts: **0 BLOCKING / 0 MAJOR / 1 MINOR / 2 NIT**

## R2 per-item verification

| r2 item | Status | v3 evidence |
| ------- | ------ | ----------- |
| M1 re-genesis mechanism | **closed** | Phase 5 :378-386 names the full loop: "`promote(Store&, parent, arena, root)` (`arena_manifold.hpp:845-847`, into `arena.base()->store()` — the publish-destination shape) → `Store::rebuildManifold` over the new head → re-attach through a fresh `WorkerContext(base, reader)` (`worker_context.hpp:52-54` — fresh because (b)'s `setOpcodeKind` writes `opcodeMap_` on the context's `VortexVM`) → re-run (a)–(e)". Requires :436-438 lists the objects ("a local `Store` plus `promote`, `rebuildManifold` and a fresh `WorkerContext` per cycle — all in-tree, and `overlay.cpp:46-82` runs the identical mint→rebuild→re-pin loop with no actor executor"). All cites verified: `promote` signature at `arena_manifold.hpp:845`; `WorkerContext(const Manifold*, const SpanReader*)` at `worker_context.hpp:52-54`; `Manifold::store()` at `manifold.hpp:486` making `arena.base()->store()` valid API; `rebuildManifold` returns `Manifold` by value (`store.hpp:451`); `overlay.cpp:46-82` is exactly the rebuild/ensureDim/mint/re-pin loop described. |
| m1 `store.cpp` cite | **closed** | :355-356 now reads "the three bodies at `store.cpp:1388-1425` — the span-source pair itself sits at :1418/:1425". Verified: `addScroll` :1388, `bindPublishedLocalScroll` :1418, `setContentSource` :1425. |
| m2 validate-cells conflation | **closed** | :361-364 — "filing is name-table entries, not cells: **the bind set's name-bearing members** — the twelve apparatus dims — file here, while its bound non-dim members (program root, agent cell, `validate` constants) bind at (c) and never file"; phase 3 :257-259 carries the same split forward. Exactly the suggested fix. |
| m3 call-convention floor | **closed** | :264-268 names the floor in the work item — "nullary plus positional args bound name-keyed on the frame", plus which binding classes are frame-bound ("`def`/plan/handler parameters *and* `let` locals are frame-bound under §5.8 (spec :847-854)" — verified, the spec names exactly that class and the `cap$N` refusal edge). Exit :281-282 cites the floor by contract, not by implementation. |
| n1 seam ordering | **closed** | :469-471 — "(iii) the segment-apply surface — listed last because it is conditional: owed only if the kept-in-step twin arm is kept (the `base()->store()` verify arm stands alone)". No longer reads as a typo. |
| n2 "read group" label | **closed** | :229-231 — "the spec's *read group* proper is the pure top half; `run` (opaque-impure) and `reify` (impure) ride the same side of the *functional* split". Honest label + explanation. |

## R2-disposition bundle spot-verification (new v3 text)

- **(e)'s third arm** (:418-430): dim-record → `Int64(DimRef)` via `ensureDimension`'s name arm,
  "a filed-cell rebind, *never* the promoted twin `*handleTarget` names", into the `+d.refFor`-paired
  slot or the member cell — verified verbatim against spec :2390-2406 (the arm is real, keyed on
  `+d.name`, exempts the ref arm, rebinds off the filed rank). The ref-record arm's phase-6 deferral
  is now *justified*, not just stated: "a no-op here because nothing mints `OpHandle`-kind cells
  pre-P16 (`writeValue` covers Int64/Double/Bool/None only, `vortex_core.cpp:196-229`)" — verified,
  `writeValue` arms are exactly Double/Int64/Bool/None(string).
- **Replay does not re-record** (:407-409): no `d.patch` cells, no `patch:<n>` serial consumed,
  `reify:<src>:<serial>` still mints off the shared `d.vars` counter — verified spec :2346-2350.
- **Donor-slot derivation + dim-record exception** (:413-416) — verified spec :2364-2372.
- **(b) two arms** (:395-399), **(c) narrowed** (:400-402), **`learn_method` never-an-alias**
  (:323-325), **marker bit as new work** (:189-191 — "the struct is `{cell, type}` today,
  `compiler.hpp:15-18`; the bit is new work here"), **env-walk fallback** (:170-173),
  **skeleton-refusal scope** (:115-117), **`closed` off outside-reference classification**
  (:174-178), **descriptor schema** (:210-215), **memo record chain** (:238-242, spec :311-331),
  **prelude home `vagent/prelude/`** (:233-235), **`vortex_core.cpp:14` ctor cite** (:370 —
  verified, ctor calls `initGenesis()`), **17-dim + home/`d.dims` = 19** (:364-365 — counted 17
  `mintNamedDimension` calls `vortex_core.cpp:50-69`, pair at `:35`/`:44` verified),
  **`d.op-label`/`or_parallel` cites** (:374-375 — verified `mesh_value.cpp:473`,
  `or_parallel.cpp:181-184`), **Makefile filter-out :698-699** (:286 — verified),
  **`fragment.{hpp,cpp}` under `vagent/` + glob rationale** (:221-223 — `COMMON_XANADU_SRCS` glob
  at Makefile:643 verified, spec :2944 agrees), **`pending(pid)==1` proxy + self-send caveat**
  (:482-483), **(iv) wake tree-local** (:465-467, :488-490), **ref-record arm moved inside the P16
  step** (:477-479 — resolves the r2 step-ordering dependency), **(i) `const Manifold*` not `Store`**
  (:461-462 — verified ctor signature), **`Call` operand wording** (:261-262 — entry-valued, never
  a `Call`-opcode operand), **emit-side filing as owed substrate** (:369-371),
  **`adoptOpRecords` :3836** (verified entry; the :3836-3940 range is the spec's own cite),
  **`historyFromSeal`/`restorePublication` cites** (:353-356 — `Store()` no-id mint at
  `publication.cpp:1097`, the `setContentSource`+`addScroll`+`bindPublishedLocalScroll` triple at
  `:1129-1131`), **`getOrCreate` fold** (`dimension_registry.cpp:194-217` verified).

No drift found in any new cite.

## End-to-end executability pass (fresh-eyes read)

Walked every phase asking "can an engineer who hasn't read the spec ship this green?":

- Phase 0/1 — self-contained; exits measure `vagent check` through the rewired driver, not a stale
  path (:133-135).
- Phase 2 — honest that `vagent check` cannot exercise `static` (no kernel linked,
  Makefile:1147 cited); ScalarHarness shape named at `tests/xuzz/vagent_compiler_test.cpp`
  (struct verified at :22ff).
- Phase 3 — the emission contract is stated in full (discriminator, role labels, `+d.refFor`
  pairing, the `dim`-field channel); the `run` convention's floor is named before the exit cites
  it; bind-set members are enumerated individually, not by reference.
- Phase 4 — fail-closed `learn` arm is itself an exit case; the P15 funnel sites are enumerated
  with per-site cites.
- Phase 5 — the re-genesis loop is named machinery; the (a) comparand's single-store degeneracy
  is acknowledged and routed to a second-store fixture (:392-394); the exit's six items each map
  to a stated mechanism (the `dim`-field `Int64(DimRef)` test covers the new third arm; the §9.3
  rebind covers (e)-last ordering; ≥2 re-geneses weaponises the verbatim re-mint rule).
- Phase 6 — sub-order is now load-bearing and stated: seams feed the pin, wiring consumes it, P16
  last; (iv) correctly carved out as tree-local.
- Phase 7 — honestly adapter-gated.

## Findings

### m1 — the (a)–(e) genesis driver's production home is never named (MINOR)

**Plan:** phase 5 :376-377 — "the orchestrated pass, harness-invoked in this phase, wired to the
real spawn/resume paths in phase 6"; phase 6 step 2 :472-473 — "the phase-5 (a)–(e) driver
invoked on the real spawn and resume paths before turns".

Every other production artifact names its TU: `fragment.{hpp,cpp}` under `apps/common/xanadu/
vagent/` with the glob and filter-out rationale (:221-223), `expand`/`code_path`/`resolve`/
`ast_clone`/`frontend` with their list edits, the prelude skeleton with its `vagent/prelude/`
home. The driver — the single largest production artifact of phase 5 — has none. The
cross-cutting test-home guidance (:525-534) pushes everything kernel-simulable toward
`tests/xuzz/`; an engineer following it literally builds the driver inside
`vagent_meta_test.cpp`, and phase 6's "invoked on the real spawn and resume paths" then has no
linkable artifact to invoke — the adapter TU cannot reach into a test binary. Spec :3110 puts
"the flag/checkpoint/genesis ceremony" in "the adapter's own executor TU", so the driver must be
either a kernel TU both the harness and the adapter link, or test scaffolding the plan should
say moves. As written the intent (reuse) is inferable but the placement is the one unnamed home
in the plan.

**Suggested fix:** one clause in the driver bullet — "the driver lives in a kernel-glob TU under
`vagent/` (picked up like `fragment.cpp`, so the phase-6 adapter TU invokes the same artifact;
phase 5's tests instantiate it)" — or state explicitly that the harness driver moves to the
adapter TU at phase 6.

### n1 — "nineteen cells all told" inside a bullet that says "filing is name-table entries, not cells" (NIT)

**Plan:** :364-365 — "the **17 system dims** `initGenesis` mints … plus the `home`/`d.dims`
genesis pair beside (`:35`,`:44`) — nineteen cells all told". The arithmetic is right and the
d.dims rank does hold dim cells, but mid-bullet it collides with the bullet's own correction
("filing is name-table entries, not cells" :361). "Nineteen names" (or "nineteen dim cells on
the rank") reads cleaner. Pure wording.

### n2 — `WorkerContext(base, reader)` leaves `base` formally undefined (NIT)

**Plan:** :382 — "re-attach through a fresh `WorkerContext(base, reader)`". `base` here means
the *new fold* (`rebuildManifold`'s `Manifold` return, taken as `const Manifold*`) — the
antecedent "over the new head" carries it, and the ctor cite is adjacent, but `WorkerContext
(&fold, reader)` or "a fresh `WorkerContext` over the rebuilt fold" costs nothing and removes
the one wrong reading that compiles (reusing the stale fold — which the exit does catch, so the
failure is loud, not silent). Wording precision only.

## Cross-cutting note (not counted)

v3's fixes are mechanically correct, not just present: the re-genesis recipe's object chain
(`arena.base()->store()` → `rebuildManifold` → `WorkerContext`) matches real signatures at every
hop; the (e) third arm's deferral asymmetry (dim-record arm live pre-P16, ref-record arm dark)
is justified by an actual `writeValue` kind audit, not asserted. The plan now reads as
executable start-to-finish; the remaining items are single-clause edits.
