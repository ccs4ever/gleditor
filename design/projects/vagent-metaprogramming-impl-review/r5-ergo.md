# R5 critique — Implementer usability (ergo lens)

**Target:** `design/vagent-metaprogramming-implementation.md` **v5** against spec
`design/vagent-metaprogramming.md` v1.19 and the tree at HEAD.

**Verdict: CONVERGED** — my r4 m1 (the promote-root contradiction, the same clause as the
three-lane major) is fixed per the R4-A ruling, and the fix is *stronger* than the ruling
required: the silent-drop worry in my r4 suggested fix is now answered affirmatively in the
text. Every r4-disposition minor-bundle item I re-checked landed at the cited coordinates with
accurate mechanics. A fresh end-to-end walk finds no remaining executability hole: an engineer
can ship every phase from the plan alone, with spec cites as optional depth.

Counts: **0 BLOCKING / 0 MAJOR / 0 MINOR / 1 NIT**

## M1 verification — the promote-root fix (R4-A)

**v4 text** (:429-434): "the cycle promotes once per runtime root, or under a harness session
root linked to both" — an instruction that, read literally, licensed `promote(programRoot)`,
double-applying grafts that (d) replay re-enacts.

**v5 text** (:441-451): "`root` is the **agent cell** alone (spec :375, :634-635): program-side
state is *never* a promote root — bare grafts must die with the incarnation (spec :375-377,
:1791-1799 — that boundary is what makes `learn` = graft + record a distinction), graft-adds
would carry without prune-removes, and replay-installed copies would double under (d), whose
alias re-match can't see a promoted `patch:<n>` label (spec :1322-1324). `promote` folds only
its `root`'s reachable subgraph (`arena_manifold.hpp:833-836`) — but nothing durable drops
silently: all durable state already hangs on the agent cell's ranks
(`d.code`/`d.patch`/`d.synapse`/`d.vars`, spec :1870-1871, :2465), and spec :2265-2266's
two-root vocabulary belongs to (a)'s classification walk, not to promote. The only
program-rooted promote is the one-time image seed named below."

Ruling conformance, point by point:

| R4-A element | v5 evidence |
| ------------ | ----------- |
| program-side never a promote root | :442, unqualified — the "once per runtime root" and "harness session root linked to both" alternatives are **gone** (grep-clean) |
| grafts die with incarnation | :442-443 → spec :375-377 ✓ (:375 "checkpoint promotes what is reachable from the agent cell … a session graft is gone at"), :1791-1799 ✓ ("they die with the incarnation at `resume`") |
| asymmetric carry (adds promote, removes don't) | :444 — coherent with the promote-visible class at :431-433 (link-writes on `holdsOwn` promote; removals never un-emit, `arena_manifold.cpp:2020-2053` verified r4) |
| replay double + `patch:<n>` alias blindness | :444-445 → spec :1322-1324 ✓ ("a learned clause's `patch:<n>` label is unreachable — `patch:` is refused as a `name` argument") |
| :2265-2266 reassigned to (a), not promote | :449 — verified: spec :2265-2266 sits in §6.6's per-cell classification region (:2257 "per-cell classification, not refusal … the `program()` structural walk"), i.e. (a)'s vocabulary |
| durable state agent-anchored | :447-448 → spec :1870-1871 ✓ ("the top-level ranks themselves — `d.code`, `d.patch`, `d.synapse` on the agent cell"), :2465 ✓ (d.code checkpoint-reachability) |
| sole program-rooted promote = image seed | :449-451 — "the initial image band comes from `promote` of the compile arena or `adoptOpRecords` (`store.cpp:3836-4018`)", named separately exactly as the ruling allowed |

The upgrade over the ruling: "nothing durable drops silently" (:446-447) *proves* the
agent-cell root is sufficient, not just mandatory — the failure mode my r4 fix text flagged
("anything unreached by the chosen root drops silently") is disposed of in-plan.

Coherence sweep: every other `promote` mention in v5 is compatible — :426 (signature,
`arena_manifold.hpp:844-846` verified — `promote(Store&, parent, from, root, budget)` at
:844-846), :413 (`discover`→`A'` orphan mint — a different mechanism, fold-internal), :479/:492/
:553 (the "promoted twin" vocabulary for slot content). No site instructs a program-rooted
cycle promote.

## Minor-bundle spot-verification

- **`d.vars` repoint precision** (:434-435): "bumps a *fresh ephemeral* the keying record's
  `+d.values` edge repoints at (spec :1300), never an in-place `writeValue`" — spec :1300
  verified (`+d.vars → [key] +d.values → scalar` record shape). The rank-vs-edge fix landed.
- **`unlearn` drop classes** (:438-441): mid-rank repoints vs tail-drop tombstone, with
  `unlink = link(…,nullopt)` at `arena_manifold.hpp:616-618` — verified verbatim (`unlink`
  calls `link(from, dim, dir, std::nullopt)`, no chain repair). The `learn_plan` mis-pairing is
  gone; (d) replay at :469-470 now describes the verbatim alias re-match correctly.
- **Residual re-scope** (:410-418): "one `dimensionNamed`-invisible orphan at its *first* fold"
  with the `B`/`A`/`A'` first-use split — `store.cpp:1066` verified (`makeCell` inside
  `makeDimension`), `arena_manifold.cpp:1781` verified (`discover(edge.dim, …)` in promote's
  walk), `:2030-2042` verified (the `real.find(edge.dim)` → `setLink` emission arm that carries
  edges onto `A'`). "Not a per-checkpoint accrual" retained.
- **"nineteen cells, eighteen filed names"** (:399-401): 17 system dims + `home`/`d.dims` = 19
  cells, `home` excluded from names — correct and self-explanatory.
- **`dimDims && home` conjunct** (:394-395): restored at the `:91-103` cite — verified r4.
- **Cite bundle**: `arena_manifold.hpp:833-836` ✓ (the `@param root` reachable-subgraph doc,
  exact lines); `unlink` :616-618 ✓; spec :634-635 ✓ ("`promote` walks only what the agent cell
  reaches"); spec :456-470 ✓ (recursive-template + membership bars in range); `splice c := 3`
  at spec :784-786 ✓ ("a `splice` is never a place … check error"); `static` baseline :952-960
  ✓ (result read → unlink → `literal()` → baseline check, mandatory operand at :960-961);
  `mesh_actor.cpp:473-477` ✓ (`executor_(…)` at :474 inside the try block — the widened range
  is more accurate than r4's :474 alone); `'splice' Postfix` peeling at spec :736-738 ✓ ("the
  arglist belongs to the `splice` (the peeling rule)").
- **`run`/`graft` arm split** (:177-179): the phase-2 promise now reads "the `run` refusal arm
  lands with the member surface in phase 3, the `graft` arm with the mutation verbs in phase
  4" — meta m9's requested attribution. (Phase 3's own bullet at :273-274 still names
  `run`/`graft` jointly; harmless — the named refusal is one checker row, and the
  evaluated-position bar from phase 2 already keeps splice-only literals out of call arguments
  before any verb runs. Not counted.)

No drift found in any re-checked cite.

## Fresh-eyes executability pass (majors only)

Walked all eight phases asking only "is there a hole an engineer must resolve by archaeology?":

- **Dependencies** — every consumed mechanism is produced earlier and the plan says where:
  `closed` flag (phase-2 tables → phase-3 descriptor → phase-4 re-verify), P15 (phase-4
  registration → phase-5 (b) re-fill → phase-6 windows), bind record (phase-3 → (a)/(b)/
  `learn` bound), filing→fold→pin→first-resolution (phase-5 → phase-6 step 1). No inversion.
- **Completeness** — §10.1 inventory all placed; Makefile wiring named per phase with the
  explicit-list precedent; test homes honestly split (`xuzz_test` glob vs
  `VAGENT_FRONTEND_TEST_SRCS` vs the `vagent_runtime_test` filter-out warning at :606-608).
- **Exit testability** — each Exit names machinery its phase delivers; kernel-simulable vs
  adapter-gated split stated per phase; phase 5's ≥2-re-geneses exit is the exact instrument
  that would catch a bad promote-root reading.
- **The fixed paragraph reads as instruction, not riddle** — an engineer now writes
  `promote(store, parent, arena, agentCell)` per cycle and `adoptOpRecords`/compile-arena
  promote once at seed. Unambiguous.

## Findings

### n1 — two micro-slips in the new promote paragraph (NIT)

**Plan:** :441-446 — "program-side state is *never* a promote root" (state/root category slip —
the root is a *cell*, and it is program-side *content* that is never promoted; unmistakable in
context but the sentence's own rigor invites tightening), and "`promote` folds only its
`root`'s reachable subgraph" (`promote` emits ops into the store; the *fold* is
`rebuildManifold`'s — `arena_manifold.hpp:833-836` says "is promoted", which is the word the
sentence wants).

**Suggested fix:** e.g. "no program-side root is ever `promote`'s `root` — program-side
content is never promoted — … `promote` carries only its `root`'s reachable subgraph".

## Cross-cutting note (not counted)

Five rounds, r1's ~12 structural findings down to one sentence-level wording nit inside the
round's own fix. The plan is executable end-to-end: phases 0–2 are self-contained compile-time
work with honest `vagent check` limits; phases 3–5 each state their emission contract, bind
set, promote discipline and filing enumeration before first use; phase 6 is sub-ordered and
conditional-aware; phase 7 is honestly gated. Ship it.
