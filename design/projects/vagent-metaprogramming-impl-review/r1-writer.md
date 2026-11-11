# r1-writer — document-coherence critique of `design/vagent-metaprogramming-implementation.md`

Verdict: **NOT CONVERGED**. Counts: **1 BLOCKING / 6 MAJOR / 13 MINOR / 5 NIT**.

Scope per `brief-common.md`: internal consistency and completeness *as a document* — traceability of
every work item to a spec section, phase-map vs. phase-body agreement, exit criteria that cite
machinery the phase doesn't deliver, uniform terminology, actionable cross-cutting requirements,
decodable shorthand, and `mdl -i`. Read the whole plan against `vagent-metaprogramming.md` v1.19 and
spot-checked the load-bearing `file:line` cites against the tree.

## What verifies clean (checked, not assumed)

- **`file:line` cites land within ±2 lines everywhere spot-checked** — the plan's citation
  discipline is real: `!= "cells"` gate `parser.cpp:851` (plan: `:848-855`); `setOpcodeKind`
  `vortex_vm.cpp:98`; `Value`/`Splice` cases :772/:789 (plan: `:771-806`); memo-hit writeback
  :1269-1272; generic writeback :1288-1293; `runPipeline` writes :337/:343-345; `writeValue`
  `vortex_core.cpp:196`; the 17 `mintNamedDimension` calls :50-66 (plan: `:50-69`); `WorkerContext`
  two-arg ctor `worker_context.hpp:52-54`; `checkpoint.store` guard `mesh_actor.cpp:188`,
  `userPermascrollPtr()` :261, `setDefaultSpanReader` :271-273; `historyFromSeal`
  `publication.cpp:1093-1103` w/ `Store()` minted :1097; `restorePublication`'s pair :1129-1132;
  `Store::Store()` `store.cpp:122`; `addScroll`/`sameContentAs` `store.cpp:1390-1404`;
  `VAGENT_FRONTEND_SRCS` `Makefile:688-690`; `VAGENT_SYNTAX_OBJS` :695; `ScalarCompilerLimits`
  `compiler.hpp:23-25`; `CheckLimits` `checker.hpp:12-27`.
- **§-references point at the right spec sections** wherever spot-checked (§5.1/§5.3/§5.6,
  §6.1–§6.4, §6.6, §7.1, §8, §9, §10.1/§10.2, §15) — defects itemized below.
- **§10.2 register fully accounted for**: P15 (phase 4), P16 (phase 6), all four owed adapter seams
  plus the `documentId` arm (phase 6, item 1), P17 deferred-not-registered (out-of-scope). All
  eleven `U-M*` items named in "Explicitly out of scope" matching §15.
- **Phase-map table is internally consistent**: linear deps match the bodies; risk column is
  defensible.
- **`mdl -i` clean** (exit 0). `mdformat --wrap 100` fails — MINOR-11.

## BLOCKING

- **B-1 — Phase 5 states the genesis pass order backwards, and mislabels the spec's steps** (plan
  :144-146). The bullet reads "Genesis (e)/(f) split: (e) name-record rebind for registered heads
  *and* replay-installed copies; (f) `d.patch` replay after rebind". In §6.6's lettered sequence —
  the only lettered genesis sequence in either document — the `d.patch` replay is step **(d)**
  (:2346), the `+d.refs` rehydration/name-record rebind is step **(e)** running *last* (:2381), and
  **(f) is "turns"** (:2440), never mentioned anywhere in the plan. Two defects follow. (i) The
  plan's "(f) `d.patch` replay" names spec step (d) by the letter the spec gives "turns" — a reader
  tracing either document cannot reconcile them. (ii) "replay *after* rebind" states the inverse of
  the spec's mandatory ordering: the spec spends a paragraph on why the sweep runs last — "a
  name-record's rebind must see **post-installation** symbols — under the naive pre-replay position
  a `learn_method`/`learn_plan` install-or-repoint replayed at (d) was invisible to it … the
  misbind/drop class the reorder removes" (:2435-2439), and "the `+d.refs` sweep runs after replay
  (step (e) below)" (:2102-2103). The bullet's own payoff clause — "patch-installed symbols resolve
  to post-replacement bindings" — is the spec's reason for rebind running *after* replay, so the
  bullet contradicts itself as well as the spec. Followed literally, phase 5 implements exactly the
  patch-drop/misbind class the ordering exists to remove, and the phase's stated exit
  ("learn→re-genesis→learn cycles") can still pass for fragments carrying no name-records — the bug
  ships silently. Fix: relabel to the spec's letters and order — "(d) `d.patch` replay in ordinal
  order (verb re-enactment + alias dedupe); (e) `+d.refs` rehydration last: ref-records
  `Int64(*handleTarget)`, name-records role-split rebind — runs after (d) precisely so
  patch-installed symbols resolve to post-replacement bindings."

## MAJOR

- **M-1 — Phase 5 lists as new work the durable apparatus phase 4's verbs cannot run without**
  (:128-129 vs :139-149). Phase 4 delivers `learn_*`/`unlearn` "at session scope"; but `unlearn`
  undoes "the latest patch whose target's resolved name equals the argument's" — it needs `d.patch`
  records to exist at call time; the installed clause's `+d.name` *is* the verb-minted `patch:<n>`
  (how `unlearn` "picks each twin precisely by its minted label", :1357); `learn` registers a fresh
  `reify` on `d.code` in-session (the two-copy rule, :1852-1854); `closed` re-verification is
  `learn`'s own accept-time gate (:1996); and capture-slot re-clone is `reify`'s
  `New`/`Link`/`Value` replay mechanism (:1264-1270), owed by phase 3's `reify` bullet already.
  Phase 5's first and third bullets — `patch:<n>` serials, `d.code` separation, `closed` taxonomy,
  capture-slot re-clone — therefore re-claim machinery earlier phases' exits silently require; phase
  5's genuinely-new work is narrower: genesis (d) replay, alias dedupe at replay, the (e)
  name-record arm, and serial non-recycling *across replay-drops*. "Alias minting" (:128) also
  conflates two different cells the spec keeps disjoint — the verb-minted `patch:<n>` *label* (on
  record and installed clause) vs. the user `name` *alias* field (on the record only). Fix: in phase
  4, name the durable writes the verbs make (`d.code` registration, `d.patch` record shape,
  `patch:<n>` mint + serial counter); in phase 5, scope the bullets to the replay-time semantics.
- **M-2 — The §6.6 (e) rehydration pass is owned incoherently by two phases, and its schema has no
  owner at all** (:144-146 vs :177-181). Phase 5 claims "(e) name-record rebind for registered heads
  *and* replay-installed copies" — i.e. the name-record/role-split arm, which needs no P16 machinery
  (name-records carry no markers). Phase 6's P16 bullet then claims "genesis rehydration by
  record-slot role (`call` vs `run` writeback split, `int64(entryOp)` vs `int64(head)`)" — which
  *is* the name-record arm again — while the arm that actually requires P16 (per-ref-record
  `Int64(*handleTarget)`, the `OpHandle`→`Int64` restore, :2384-2417) goes unnamed. Meanwhile the
  `+d.refs` unified record schema the whole ceremony keys on — the `+d.name` discriminator, the
  `+d.grab → "call"/"run"` role labels, the `+d.refFor` pairing — is emitted at fragment-emission
  time (phase 3, which says only "`+d.refs` mark rank") and is never spelled as a work item
  anywhere. Fix: phase 5 owns the name-record arm explicitly; phase 6 names the
  ref-record/handleTarget arm it actually adds; phase 3 names the record schema (name-record vs
  ref-record, role labels, `+d.refFor`).
- **M-3 — `learn_where`/`learn_body` are never scheduled, but a phase-7 exit needs `learn_where`**
  (:128-129, :198). The phase-4 goal says `learn_*` generically (:112), but its work-item bullet
  enumerates only `learn_plan`/`learn_method`/`learn_handler`/`unlearn`. The two re-point verbs are
  not covered by the named three — they differ semantically on every axis the bullets name (no
  alias, no clause-label mint; the patch records a target descriptor re-point; `unlearn` matches by
  recorded target name; replay re-points). And phase 7's exit — "§9.2 … run green" — exercises
  `learn_where` directly (:2825). Fix: write "each `graft_*` verb's durable twin" or name all five.
- **M-4 — The §10.1 adapter block is consumed by phases 3–6 but decomposed nowhere** (:94-129,
  :161-181). Three families of spec inventory have no owning work item: (i) the adapter-supplied
  bindings — program root, `agent_cell()`, the twelve data-minted dims (`d.code`, `d.patch`,
  `d.weight`, `d.synapse`, `d.entry`, `d.patch-head`, `d.validate`, `d.refs`, `d.ref`, `d.recipe`,
  `d.refFor`, `d.mesh.relocation`), and the `validate` constants (Linked-ceiling cell, arity table,
  impure-kind set) — all "minted with no marks outstanding" via V10's mechanism (:1054-1060).
  Phase-3 resolvers walk `d.code`/`+d.entry`/`+d.refs`/`+d.recipe` (dims that must exist), phase-4
  `validate`/`closed` need the constants and `program()`/ `agent_cell()`, and the phase-6 filing
  list (:173-176) omits the bind-set dims §10.1 puts first in the fixed vocabulary (:3017-3027).
  (ii) The `programRef` bind record's recorded fields — `E_ops` extent + instruction enumeration —
  which phase-4's P15 bullet *consumes* ("the recorded instruction enumeration"), phase-5's
  re-genesis needs for (b), and phase-6's (a) comparand bounds on; produced by nothing in the plan
  (:3057-3058). (iii) Genesis step (c) (`programRef → program root` bind) and the (a)–(f) ceremony
  *driver* itself — the ordered passes the phases contribute piecemeal — have no owner; nor does the
  `std:agent` genesis hook §1 names (`std:meta` + `std:agent` genesis hook, :91). Fix: add an
  adapter-bindings work item to phase 4 (or phase 0 if the harness needs it earlier), the
  bind-record fields to phase 5/6, and name the genesis driver + hook explicitly.
- **M-5 — External prerequisites are never stated** (phase map :17-26). Spec §1 inherits the vagent
  runtime state: "the call ABI, actor turns and `std:agent` are owed there and so are owed here"
  (:75-76), and §10.1 repeats it — "`run`/`graft_*` verbs need the call ABI (`#Call` + frame + spill
  \+ the `d.entry` head resolution) — owed by vagent §11.1 itself" (:3069-3070). The plan consumes
  all of it: `run` needs the frame discipline (phase 3), resolvers and `closed` need V10's adapter
  bindings, P16's flag lives inside `pumpOne`'s `Executor` callback, re-genesis needs the first-turn
  genesis hook. Nothing says "requires `vagent-agent-language.md`'s V10/call-ABI/actor-turns landed"
  — not even a Depends-on note. Relatedly, `$(OBJDIR)/vagent` links only
  `$(VAGENT_OBJS) $(VAGENT_FRONTEND_OBJS)` today (`Makefile:1147`) — no kernel — so the host through
  which phases 3–6 are exercised (adapter TU home, binary link set) is itself unaddressed;
  `vagent_runtime_test` additionally filters *out* all `vagent/` objects (`Makefile:697`), so where
  engine-level fragment/mutation tests link from deserves one line.
- **M-6 — `std:meta`'s import surface lands a phase after the members that need it, and the member
  surface is never enumerated** (:101-104, :106-108 vs :123-124). Phase 3 ships resolvers,
  `reflect`/`reify`, and `run` — all `std:meta` members — but `import std:meta` (checker
  import-allowlist `checker.cpp:2061-2067` + builtin member-signature table `:765-779`) is a phase-4
  bullet, so phase-3's "resolver tests" can't spell the import through the language surface the
  phase's exit implies. Either move the two checker-table entries to phase 3 or say the phase-3
  tests drive the prelude routines engine-side. Second, the member list is never stated: `validate`
  (graft's acceptance gate, §7.2 — with its own adapter-bound constants),
  `set_weight`/`sprout_synapse`/`weight_of`/`next_rand`/`test_of`/`body_of`/`program`/
  `agent_cell`/`module_named` attach to phases only by inference under the "prelude sources" blanket
  — and phase 7 then exits on `set_weight` arms that no bullet built.

## MINOR

- **m-1** (:3, :217): companion version cited as "v1.18, converged at review round 18" and "verified
  at v1.18"; the spec is **v1.19** (:4119 — approvals recorded, no mechanism changes). Stale cite,
  no mechanism risk.
- **m-2** (:43-44 vs :57-58, :73-76): the Makefile wiring is stated only for phase 0's three files;
  `code_path.cpp` (phase 1) and `expand.cpp` (phase 2) also join `VAGENT_FRONTEND_SRCS` per §10.1's
  Makefile line (:3061-3063) — the exact omission class the plan invokes as precedent. Say "joins
  `VAGENT_FRONTEND_SRCS`" on each file.
- **m-3** (phase 2, :79-82): `staticFuel` in `ScalarCompilerLimits` (`compiler.hpp:23-26`, verified
  :22-25) — spec §10.1's trailing note names it (:3070-3072); the plan's `static` bullet doesn't.
- **m-4** (:119-122): P15's re-registration is scoped to "post-resume"; spec genesis (b) runs the
  `setOpcodeKind` pass "at the spawn-time injection as after `resume`" (:2334-2337) — needed once
  phase-6's base-injection lands, and exercised by phase-5's own re-genesis.
- **m-5** (:56 vs :35-37, :41-42): phase-0's `resolve` "stamps `Identifier` binding identities on
  `Expr`", but the `Expr` scope-stamp field is a phase-1 `ast.hpp` item — the consumer precedes its
  field (or phase-0's resolve lands not stamping, unstated). Same phase-0 seam: `frontend.cpp`
  "sequences parse → resolve → expand → check" while `expand` doesn't exist until phase 2 — the
  stub/seam isn't named.
- **m-6** (:53-56, :64-65): phase 1 parses `splice`/`static` and adds their `ExprKind`s, but the
  phase-1 checker's behavior on those nodes pre-phase-2 is unstated (refuse outright?); the exit's
  "accepts/rejects `code"…"` programs per §5/§7.1" is loose about which §5 rules phase 1 can
  actually enforce.
- **m-7** (:143): "`learn_method` replace-then-unlink ordering" reads backwards — the spec's
  `graft_method` is unlink-first-*then*-mint (the `d.vars` first-match walk,
  `vortex_core.cpp:100-127`), so "replace" *is* "unlink-then-install".
- **m-8** (several): missing cites — the resolvers bullet (:101-104) needs §6.7 (qualified-name
  space, first-`:` split, `code_named`'s two-space rule) beside its implicit §6.1/§6.6; phase-4's
  "sideness/`closed` guards; `run`-target rebind dead-end refusal" (:126-127) is §6.6, uncited; the
  dim-filing bullet (:173-176) carries no §-ref at all (it is §10.1/genesis (b)).
- **m-9** (:53-62): §5.1's reserved-name refusals are unscheduled — user decls/bindings named
  `splice`/`static` refused outright; `run`/`value_of` refused when `std:meta` is imported
  (:733-736); and §6.7's `src:`/`reify:`/`patch:` reserved-prefix refusal at module declaration
  (:2500-2504).
- **m-10** (:142-143): "so a `learn_*` can unlink its declared/grafted match again *after resume*" —
  phase 5 has no resume (that's phase 6's whole point); "on re-genesis" is the claim the phase's own
  exit can test.
- **m-11** (whole file): fails `mdformat --wrap 100` (the CI `format-check` gate — it rewraps to 100
  cols and normalizes the `---` rules and the phase-map table) though `mdl -i` is clean;
  `make format` before commit.
- **m-12** (:101-104): "`reflect`/`reify` over literal-born fragments via `+d.recipe`" reads as if
  `reflect` replays recipes — `test_of`/`body_of` wrap *program-side* clause graphs in memoized
  session-side `Code<Cell>` wrappers carrying `+d.entry` (M8, :1157-1158, :1835-1846); only `reify`
  replays `+d.recipe`, and only on literal-born fragments. Tighten the bullet.
- **m-13** (:126): "`run`-target rebind dead-end refusal" is a coinage ambiguous between two
  spec-named refusal classes — §6.6's emission-fixed unwritten-slot refusal (fail-until-resume vs
  fail-forever, :2009-2023) and §6.2's headless-`run` descriptor refusal (already named in phase-3's
  exit). Name which.

## NIT

- **n-1** (:173): "`r18-fixed`" decodes only via the spec's changelog/review record (v1.18's
  disposition "Corrects the filed-set enumeration"); one parenthetical like "(the r18-fixed
  vocabulary)" → "(the v1.18-fixed vocabulary)" would self-describe.
- **n-2** (:161, :168, :173, :177): phase-6's four sub-steps are all numbered `1.` in source
  (renders 1–4, but "Land in the listed sub-order" references numbers the source doesn't show).
- **n-3** (:43-44, :180, :185): external references without cites — "the `cell_path.cpp` omission
  bug" (no pointer), "P4's approved wake" (the spec carries `vagent-agent-language.md:1670`; the
  plan could too), "the V10 test the language spec already describes".
- **n-4** (:36 vs :39): `cloneExpr`/`cloneDecl` (resolve bullet) vs `cloneExpr`/`cloneProgram`
  (ast_clone bullet) — naming drift inherited from the spec itself (§5.6 vs §10.1); pick the
  implementation names and use them once.
- **n-5** (:60): "fragment table keyed by `SourceLocation`" drops the spec's "`SourceLocation`/
  ordinal stamped on the `Code` node" qualifier (:1019) — the `/ordinal` is what lets `cloneExpr`'d
  literals share a record.

## Overall assessment

The plan is unusually faithful to the spec at the citation level — every `file:line` spot-check
landed, every §-reference I verified points at the right section, and the §10.1 frontend inventory,
§10.2 register, and §15 deferred list are all accounted for. The defects concentrate in one region:
the phase-5/genesis boundary, where the document mislabels spec step letters, inverts the spec's
most carefully argued ordering constraint, double-claims the (e) pass's name-record arm across two
phases, and lists as "new" machinery that phase-4's own verbs need on day one — compounded by the
§10.1 adapter block (bindings, dims, `programRef` record fields, genesis driver) being consumed but
never decomposed into work items, and by the external prerequisites (call ABI, V10, actor turns,
genesis hook) going unstated. Fix B-1 and the phase-4/5/6 boundary items (M-1–M-4), state the
prerequisites (M-5), and the document reads as a sound build-order.
