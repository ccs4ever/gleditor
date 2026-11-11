# R4 critique — Implementer usability (ergo lens)

**Target:** `design/vagent-metaprogramming-implementation.md` **v4** against spec
`design/vagent-metaprogramming.md` v1.19 and the tree at HEAD.

**Verdict: CONVERGED** — my r3 minor and both nits landed correctly, and the two r3 majors
(`splice` call-by-value, the promote-visible mutation class) are fixed with mechanically
accurate mechanics I re-verified against code and spec. One MINOR wording tension remains in
the new root sentence; nothing is a sequencing or executability hole. Every phase's Exit
remains self-checkable against machinery its own phase delivers.

Counts: **0 BLOCKING / 0 MAJOR / 1 MINOR / 0 NIT**

## R3 per-item verification

| r3 item | Status | v4 evidence |
| ------- | ------ | ----------- |
| m1 genesis-driver TU home | **closed** | :412-414 — "The driver lives in a kernel-glob TU under `vagent/` — the `genesis.{hpp,cpp}` shape, glob-picked like `fragment.cpp` — so phase 6's adapter executor TU invokes the same artifact these tests instantiate (spec :3110 puts the ceremony in the adapter's own executor TU)". Exactly the suggested fix, file name and rationale both stated. Confirmed `VAGENT_SRCS` globs only `apps/vagent/` (Makefile:686-687), so `apps/common/xanadu/vagent/genesis.cpp` lands in `COMMON_XANADU_SRCS` (Makefile:643) → linkable by `xuzz_test`, invisible to the `vagent` check binary, filtered out of `vagent_runtime_test` (Makefile:698) like `fragment.cpp`. |
| n1 "nineteen cells" | **closed** | :394 now reads "nineteen names all told", consistent with the bullet's own "filing is name-table entries, not cells". |
| n2 `WorkerContext(base, reader)` | **closed** | :418-421 — "re-attach through a fresh `WorkerContext` over the rebuilt fold (`worker_context.hpp:52-54` — fresh because the context's `arena` pins `base` at construction and the bundle is non-movable; the fresh VM's empty `opcodeMap_` is what (b) then re-fills)". `base` ambiguity gone, and the freshness justification is better than what I asked for — verified the ctor (`arena(baseManifold)` pins at construction; copy ops deleted per the header comment). |

## R3-disposition spot-verification (new v4 text)

- **R3-A `splice` call-by-value** (:159-163): "each argument binds a fresh `let` local at the
  expanded body's head, evaluated once in argument order — the `{ let p0 := count; p0 + 1 }`
  shape §9.1 spells — with parameter references re-pointed at the synthesized bindings;
  M13's non-`Code` args marshal identically, and the synthesized `let` is the design's only
  one — `cap$N` remains a pure alias (§5.3 — textual substitution would yield observable
  multi-eval on impure actuals)". All three required elements present, plus the dead-`let`
  erasure handling.
- **R3-B promote-visible class** (:421-434): verified against code — `promote` mint-loop maps
  non-ephemeral cells to themselves with no content op (`arena_manifold.cpp:1867-1870`), the
  link loop emits only for `holdsOwn` cells and never emits removals (:2020-2053, esp.
  `:2021`); `root` reachable-subgraph doc at `arena_manifold.hpp:833-845`, signature
  `:845-846` matching the corrected `:844-846` cite. The mint-and-repoint counter form and
  per-root/session-root promotion are stated. Spec cites :375/:635-636/:2265-2266 verified
  verbatim ("program cells do not *republish*", "distinct adapter-bound roots never
  `+d`-reached from each other").
- **vortex m1 `pending(pid)`** (:540-544): now "mailbox-empty-at-turn-end is
  `pending(pid)==0` sampled inside the callback; `pending(pid)==1` is the *flag-time*
  queue-depth test" — verified `mailbox.pop_front()` at `mesh_actor.cpp:468`, `executor_` at
  `:474`, `pending` at `:659`, and spec's own "flag-driven: `pending(pid)==1` arms the
  window" (:2165-2169) + no-interleave `publishValue` discipline (:2171-2179).
- **vortex m2 `d.dims` guard** (:387-390): "names read off each dim cell's interned content …
  *never* the arena-local `d.dims` rank, which appends only under a resolvable `home`
  (`:92-103`)" — verified `dimensions()` at :34-46, `ensureDimension`'s rank append gated on
  `dimDims && home` at :91-103.
- **zigzag m3-2 `+d.name` exemption** (:535-538): "per-ref-record `Int64(*handleTarget)`
  writebacks *minus the `+d.name`-bearing records the dim arm owns* (spec :2394-2396)" —
  verified spec :2393-2400.
- **zigzag m3-3 filing per cycle** (:397-400): "Filing re-enumerates per cycle … before each
  `rebuildManifold` … idempotent by name".
- **codebase m1 "include"** (:405-409): "today's unfiled session mints include …" with the
  seven new mints — verified `dim_cap.cpp:83-85` (d.actor/d.ticket/d.signature),
  `command_transaction.cpp:16-18` (d.target/d.action/d.reply), `sql_promotion.cpp:140`
  (d.promoted). Files live under `zigzag/`; basename-only cites are the doc's convention.
- **architect m-2 + vortex n `OpHandle`** (:481-486): corrected to "emission writes `Int64`
  ref slots and `reify`'s `New`/`Link`/`Value` palette cannot mint handle-kind cells …
  `OpHandle` cells do exist today, `mesh_evaluator.cpp:733`, just never in a fragment ref
  slot … must *stay* kind-guarded post-replay" — verified `mesh_evaluator.cpp:733` mints
  `OpHandle` and `:735` is the `link(cell, relocation, "cell")` marker precedent.
- **architect m-3 (e) resolvers** (:470-473): `symbol_named`'s module-`d.vars` walk for
  `call`-role, label→head for `run`-role — verified spec :3110.
- **writer m-1 dim-arm two record kinds** (:237-238, :473-474): "`dim`-field members and
  `+d.refFor`-paired ref-capture dim records alike" — verified spec :1964-1967.
- **Cite drifts**: `arena_manifold.hpp:844-846` ✓; `Makefile:698` alone ✓;
  `or_parallel.cpp:181-184` ✓ (verified — the four `ensureDimension` mints are exactly at
  181-184, better than the disposition's suggested :182-185); `store.cpp:3836-4018` ✓
  (`adoptOpRecords` opens at :3836; the spec's own :3836-3940 is the stale range);
  `vagent_compiler_test.cpp:22-53` ✓ (ScalarHarness verified); `Makefile:1330-1334`
  `test/ux-journey-plans` ✓; "§2 glossary" :189 ✓ (`closed fragment` entry at spec :158);
  ~25 check sites :129/:184 ✓; `int64` ref-box class restored :187-188 ✓; `vagent/prelude/`
  worded as convention :253 ✓; "recipients/deadline" typo fixed :77-78 ✓; "user alias"
  residue removed — the phrase now sits correctly in phase 4 (:351) and durable semantics
  (:489) only; meta residuals (prefix productions :103-104, `$cap$N` interim :293-294,
  splice-only run/graft arm :267-268, `static`-in-emitted-body :231-233, descriptor
  parameter-rank vocab :225-226, `static` result-read/`literal()` :197-199) all present.

No drift found in any re-checked cite.

## Semantic coherence check on the new promote-class text

Traced the two worry cases against code: (1) `set_weight` — clause-arm records live on the
*agent's* `d.vars` keyed by qualified clause name (spec :2647-2650) and junctions live on the
agent's `+d.synapse` rank, so both stay agent-root-reachable; mint-and-repoint is the right
form, and the repoint's new `SetLink` op displaces the stale base edge at fold time
(`Manifold::link`'s `displacedUs`/`displacedIt` eviction, `manifold.cpp:664-681`) — only pure
removals are lost, exactly as the plan scopes to (d) replay/tombstones. (2) The `d.vars`
counter rewind — an in-place bump on a post-first-fold counter would silently ABA
`patch:<n>`; phase 5 now names the mint-and-repoint form where the counter's durability is
specified, and the ≥2-re-geneses + no-ABA exit cases catch any miss loudly. Stated where
consumed; acceptable.

## Fresh-eyes executability pass

Walked all phases again asking "can an engineer who hasn't read the spec ship this green?":
phases 0–2 self-contained with honest `vagent check` limits; phase 3's emission contract,
bind-set enumeration and call floor are stated before use; phase 4's guards and records are
priced; phase 5 now names the driver TU, the loop machinery, the promote-visible class and
the filing re-enumeration — every Exit item maps to a stated mechanism; phase 6's sub-order
and P16 windows are concrete; phase 7 is honestly adapter-gated. One wording tension found:

## Findings

### m1 — "promotes once per runtime root" sits at odds with the sentence before it (MINOR)

**Plan:** :429-434 — "`root` is the **agent cell** (spec :375, :635-636 — program cells never
republish; they ride the folded prefix and (d) re-enacts), and session mutations span two
disjoint roots — `program()`/`agent_cell()` are distinct adapter-bound roots never
`+d`-reached from each other (spec :2265-2266) — so the cycle promotes once per runtime
root, or under a harness session root linked to both".

Read literally, "promotes once per runtime root" tells the implementer to call
`promote(programRoot)` too — which directly contradicts the parenthetical two clauses
earlier ("program cells never republish … (d) re-enacts") and would double-apply grafts
already re-enacted by (d). The parenthetical does carry the disambiguation, so an attentive
reader resolves it — but this is the load-bearing instruction for the re-genesis loop and it
pays for being unambiguous.

**Suggested fix:** tighten to e.g. "the cycle promotes from the **agent-cell root** only —
program-side mutation rides the folded prefix and (d) re-enacts it — or promotes once under
a harness session root linked to both roots (session state that must promote is agent-side;
anything unreached by the chosen root drops silently, `arena_manifold.hpp:837-846`)".

## Cross-cutting note (not counted)

v4's R3-B fix is right at every hop I checked — the promote audit, the fold-time link
eviction that makes repoint work, the agent-side weight placement, and the corrected
`OpHandle`-exists reasoning. The plan is executable start-to-finish; the remaining item is a
single-sentence tighten.
