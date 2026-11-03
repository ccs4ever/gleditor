# Round 2 critique — Codebase Expert lane

Document under review: `design/vagent-metaprogramming.md` (v1.1, confirmed at line 5).
Verified against the live tree under `/data/git/gleditor-restore/apps/common/xanadu/` and the
sibling specs `design/vagent-agent-language.md` (v3.9, confirmed at line 5), `design/
vortex-hyperstructural-runtime.md`, `design/vagent-runtime-grounding.md`, and `design/
vagent-implementation.md`. Round-1 baseline: `r1-codebase.md` (ten findings) and `r1-disposition.md`
(the orchestrator's rulings, plus the `VAGENT_FRONTEND_SRCS` build fix).

v1.1 is substantially better grounded than v1.0. Every load-bearing mechanism re-checked this round
lands where the doc says it does: the head-indirection/`d.entry` convention is consistent with
`link`'s one-neighbour eviction (`arena_manifold.cpp:1347-1377`); all four P15 write areas exist at
the exact cited lines; `promote`'s verbatim `Int64` copy (`:1934-1936`) and its internal old→new
`real` map (`:1858`) make P16's premise and named association real; the `+d.recipe` reify story,
`cap$N` binding-layer renaming, `d.synapse` junction cells, backlink occupancy tests, and
heads-by-name patch records are all mechanically sound. The genuinely new claims — `std:meta`, the
`d.*` minted dimensions, `expand`/`cloneExpr`, `code_named`/`plan_named` — are uniformly and
honestly marked **owed/new code**; nothing this round presents proposed machinery as if it already
existed. Three real problems remain (one blocking, two major) plus a batch of minors.

______________________________________________________________________

## Round-1 findings: disposition

1. **r1-1 (BLOCKING — P15 write-site enumeration): RESOLVED.** §6.5 (lines 942-953) and the P15
   register row (:1368) now site the refusal at the `VortexCore::writeValue`/`splice` funnel
   (`vortex_core.cpp:196-254`) behind a VM-installed `opcodeMap_`-membership predicate — the funnel
   option r1 recommended — and enumerate every site that restates a non-fresh cell's content:
   `executeOpcodeBody`'s `Value`/`Splice` (`vortex_vm.cpp:771-806`; verified: `Value` :771-791,
   `Splice` :792-806), `step()`'s memo-hit writeback (:1269-1273; verified), `step()`'s generic
   output writeback (:1287-1294; verified, including the `{New,Clone,Link,Break,Linked}` exclusion
   at :1288-1290), and both `runPipeline` writes (`:337` paramCell, `:343-346` outputs; verified
   inside :310-352 with its own mirror of the exclusion list at :340-342). The map-membership
   predicate also fixes the meta-1 over-reach — a decodability test would refuse ordinary scalar
   writes, since `getOpcodeKind` decodes any int cell via `asInt64` (:107-112). The fresh-cell
   exemption is explicit and correct: `Bind`'s `valCell` (:903-904), `Call`'s frame (:860-863),
   `New`/`Linked` mints, and cursor writes are all `opcodeMap_`-absent targets. The residual
   bypass — the arena-level `setValueBits`/`setContent` APIs (used host-side at :139-143 and
   `mesh_evaluator.cpp:733`) — is named honestly at §7.3 (:1091-1094) as C++-caller discipline.

2. **r1-2 (MAJOR — `ensureDimension`/"marks in agreement"): RESOLVED.** §1 (:106-111) now says the
   dim machinery "is **not** mark-covered (`Mark` records no dimension field,
   `arena_manifold.hpp:170-186`), so dimensions are minted only when no mark is outstanding (the
   adapter's genesis path), the same discipline §4.2 rules for opcode minting." §4.2 (:408-412)
   spells the `arenaDims_`→`dimensionNamed` aliasing hazard; §7.3 (:1093-1094) prices it as caller
   discipline; P17 (:1371-1374) records the resync option as deferred-not-registered. Verified:
   `Mark` :170-186 has no dim field; `ensureDimension` :78-105 has no mark check; `release`
   :1612-1700 never touches `arenaDims_`. (Corroboration the doc could cite: the language spec's own
   V10 already describes the name-table aliasing hazard — `vagent-agent-language.md:229-234`.)
   `ensureDimension` still cannot *enforce* the rule — the doc presents it as discipline, which is
   the honest framing.

3. **r1-3 (MAJOR — cross-arena aliasing): RESOLVED.** §4.5's handle table (:468-489), §6.7
   (:1007-1018), and §12's V1 row now state that `resolveCell` is a dense-range test
   (`vortex_vm.cpp:50-67`, `arena_manifold.cpp:256-267`), that in-range foreign refs *alias* live
   local cells, and that received `Code`/`Cell` values are opaque until re-resolved by `d.name`;
   U-M2 and §7.3 (:1088-1090) carry the residual honestly. Verified `resolveCell`/`denseOf` and
   `Linked`'s contains-only follow (:668-671).

4. **r1-4 (MINOR — `parseExpression` exists): RESOLVED.** M2's price now reads that
   `Parser::parseExpression()` already exists and refuses trailing input (`parser.cpp:271-281`);
   the genuinely new work is the `code` prefix case plus the `CodeParams`/`#label` header on a
   `parseCodeBody` entry; `parser.cpp` additions are listed in §10.1 (:1331+) and the prefix gate
   (`parser.cpp:848-855`, currently refusing `!= "cells"`) is cited.

5. **r1-5 (MINOR — converged into blocking-2, graft attach ambiguity): RESOLVED.** The convention is
   now explicit everywhere: clause `+d.grab`/`+d.spin`/`+d.values` point at **head** cells, never op
   cells (:850-853); head→entry uses dedicated `d.entry` (:729-730); the reader convention
   ("resolve the linked cell directly when it is an op, follow `+d.entry` once when it is a head")
   is stated at :744-752 and correctly booked as part of the call ABI vagent §11.1 already owes
   (:1356-1359). `unlearn`'s "prior head" is now well-defined — a head cell recorded by name. The
   eviction premise is verified (`bindInput` occupies `+d.grab` POS, `bindOutput` `-d.grab` NEG —
   `vortex_core.cpp:354-414`; `link` evicts — `arena_manifold.cpp:1347-1377`).

6. **r1-6 (MINOR — `run` variadic surface): RESOLVED as far as it went — the adopted fix introduced
   a new inconsistency, finding 2 below.** `run(c: Code<T>, args: [Cell]): T` is declared and the
   no-variadic-surface rationale is correct (`parser.cpp:316-339`).

7. **r1-7 (MINOR — `Code` arity chain): RESOLVED.** M3 now names both edits — "`scalars` gains
   `Code` *and* its arity chain maps `Code → 1` (`checker.cpp:555-562`)" — verified at :554-562.

8. **r1-8 (MINOR — `reify` mechanism unpriced): RESOLVED.** `+d.recipe` is in the head layout
   (:740); the mechanism is spelled (:799-806 — census, `(from,dim,dir,to)` link tuples, literal
   contents, old→new); `fragment.{hpp,cpp}` lists the emission duty (:1335-1336); §13 prices it
   (:1467-1468); U-M7 keeps the `dimensionsOf` enumeration gap deferred. The `Clone` refusal is
   correctly grounded (`masterOf` sharing — `vortex_vm.cpp:807-836`, `vortex_core.cpp:181,202`).

9. **r1-9 (MINOR — citation precision): PARTIALLY RESOLVED.** Fixed: the `r10` path now reads
   `projects/vagent-review/r10-vortex.md`; the `Call` frame cite is `:860`; the `promote` claim now
   cites both the BFS (`arena_manifold.cpp:1744-1788`) and the `Promoted` struct
   (`arena_manifold.hpp:235-240`). One cite regressed and two related ranges remain short — see
   finding 4.

10. **r1-10 (MINOR — flattened tables): RESOLVED.** Every normative table is now single-line-per-row
    GFM (§1 status, §2 glossary, §4.5, §6.1 `std:meta`, §10.2 register, §11 alternatives).

Cross-lane items that landed on this lane's evidence and were re-verified: **blocking-1** (Int64
refs die at `promote`) is now P16 (:1369) — grounded: verbatim `Int64` copy at
`arena_manifold.cpp:1934-1936`, link/`OpHandle`-only relocation, the `mesh_evaluator.cpp:720-736`
publication precedent exists (OpHandle write :733), `promote`'s `real` map (:1858) is the old→new
association named, and no opcode can write `OpHandle` kind (`vortex_core.cpp:203-227`); conditional
durability is stated everywhere it matters (:90, :94, :779, :975-979, :1427). **blocking-2**
(`+d.grab` cannot carry graft links) is resolved by the head-indirection convention above.
U-M9 (`specialize`) is registered at :1553; `splice`-`def` expansion order is logged at :1097.

______________________________________________________________________

## Round-2 findings

1. **Severity: BLOCKING. Location: §7.2 `validate`, lines 1068-1071.**

   **Problem.** The seen-set is written backwards: "`marker +d.validate → visited`, membership
   tested as `visited -d.validate == marker`". `link` enforces one `+d` neighbour per cell per
   dimension by eviction — `displacedUs = mine->neighbor(dir)` (`arena_manifold.cpp:1349-1351`)
   and the displaced cell's reciprocal edge is cleared (`setOneSide(*other, dim, -dir, noCell)`,
   `:1364-1367`). A single marker cell's `+d.validate` therefore names **at most one** visited
   cell: marking `v2` displaces `v1`'s backlink, so `v1 -d.validate == marker` fails immediately
   after `v2` is marked. The "set" holds ≤1 element. Any fragment containing a back-edge — a
   `Jump`/`Call` target, a `+d.spin` loop, i.e. any cycle — revisits a cell that tests unvisited;
   the walk cycles until its per-op budget dies, so `validate` fails exactly the cyclic
   `run`-reachable graphs it exists to check, and `graft` runs it on every install (:1074). The
   trailing justification — "a second pass cannot be confused by a stale mark (its own links evict
   the first pass's backlinks)" — is vacuous under the written direction: eviction is not what
   saves a second pass; the set itself is broken.

   **Evidence.** `arena_manifold.cpp:1347-1377` (one-neighbour-per-direction with eviction and
   reciprocal clear); `vagent-metaprogramming.md:1068-1073` (the stated topology and membership
   test); `vortex_vm.cpp:839-873`,`:1150-1168` (the int-encoded `Jump`/`Branch`/`Call`/`Choice`
   targets that create the back-edges `validate` must traverse).

   **Fix.** Reverse the arrow: mark `visited +d.validate → marker` and test membership as
   `visited +d.validate == marker`. Each visited cell then carries its own mark slot; a stale
   mark names the previous pass's marker and fails equality — which preserves every property the
   text claims (per-pass fresh marker, stale-mark immunity, benign eviction) at the same one-link-
   per-cell cost. (Alternative: a marker-headed `+d.step` rank with a rank-membership walk — the
   `Linked` idiom already in the toolchest — but the reversed mark-link is the one-line fix.)

2. **Severity: MAJOR. Location: §6.1 `std:meta` table (`run` row, :771); §5.4 (:623-629); §6.2
   (:822-826).**

   **Problem.** The adopted signature `run(c: Code<T>, args: [Cell]): T` is inconsistent with the
   doc's own example and with the stated spill semantics. The example `run(c, [3, flag])` (:627)
   cannot typecheck as written: `[3, flag]` is a heterogeneous list literal and the checker
   requires homogeneous elements (`expectType` per item, `checker.cpp:1399-1409`), so it has no
   `List<T>` type at all; and even a homogeneous `[3]` is `[Int]`, not `[Cell]`. Meanwhile the
   semantics are value-spill — "the argument list is spilled onto the `#Call` frame exactly as
   method arguments are" — and under spec §5.8 (`vagent-agent-language.md:643-655`) the frame's
   `d.vars`/`d.values` hold argument *values*, not cells. Three mutually exclusive readings exist:
   (a) `args` is literally `[Cell]` handles to pre-boxed value cells — then the example needs a
   boxing surface no verb provides; (b) the checker-special `run` form auto-boxes each list element
   into a cell — new machinery, unpriced; (c) `args` is a value list typed per the descriptor —
   then `[Cell]` is the wrong declared type and homogeneity still rejects `[3, flag]` unless the
   checker-special form relaxes it. The doc picks none.

   **Evidence.** `checker.cpp:1399-1409` (list-literal homogeneity), `vagent-metaprogramming.md:771`
   (the declared `[Cell]`), `:627` (the example), `vagent-agent-language.md:643-655` (§5.8 args are
   values on the frame).

   **Fix.** State the typing rule. Reading (c) fits the stated semantics: declare `run`'s second
   parameter checker-special — a list literal whose elements the checker accepts individually (the
   `any`-wildcard the `builtin` table already uses, `checker.cpp:27,771`, per element) and whose
   types are checked against the fragment descriptor at `run` time — the §7.2 descriptor
   comparison already exists for this. Then `args: [Cell]` becomes an explicitly named
   wildcard-list form (or `[<any>]`-style) and §10.1's checker row notes the relaxed homogeneity
   for this one form. If (a) is intended, fix the example and name the boxing verb.

3. **Severity: MAJOR. Location: §6.1 head layout (:729-742) and descriptor (:631-635, :733);
   M6 (:215-216); §6.2 (:822-826).**

   **Problem.** `run` must "spill the fragment's `result`/`produced` cells onto the current frame"
   before the `#Call`, restore them after, and read `result` when `produced` is true — but nothing
   on the head or in the descriptor names those two cells. The head layout records `+d.entry`,
   `+d.values`→descriptor, `+d.name`, `+d.refs`, `+d.recipe`; the descriptor's `+d.grab` rank is
   the parameter/signature table and its `+d.vars` entries are type *names* (`["result"] ─
   (+d.values) ─► "Int"` at :733 — a string, not the cell). `CompiledStatement{entry,result,
   produced}` proves both cells exist at emission (`compiler.hpp:28-34`; `emitStatement` mints
   `output`/`produced` at `compiler.cpp:858-859`), but the emitted head keeps no path to them:
   they are interior cells reachable only as some op's bound outputs. `run` holding the head cell
   cannot find them — an implementer following this doc hits the gap on day one.

   **Evidence.** `vagent-metaprogramming.md:729-742` (head layout — no result/produced link), :733
   (the `["result"]` record is a type name), :822-826 (the spill contract that needs the cells),
   `compiler.hpp:28-34`, `compiler.cpp:858-859`.

   **Fix.** Record the two cells where `run` can reach them — e.g., `+d.grab` wing slots on the
   head itself (its POS side is free; only `-d.grab` is the occupancy marker) or descriptor record
   entries that link the *cells* rather than the type names — and add them to the §6.1 diagram and
   the `+d.refs` rank (they are graph cells holding no ref-ints, so they need no P16 handling;
   say that too).

4. **Severity: MINOR. Location: citation precision, second batch.**

   - §4.3 "`Cut` discards it (`:253`)" — `discard(cp.mark)` is `vortex_vm.cpp:255`; `:253` is
     `ChoicePoint cp = choiceStack_.back()`. (r1's suggested correction said `:253`; the correction
     was itself off — cite `:255`.)
   - §4.3 "`release(m)` … truncates cells minted after `m` (`arena_manifold.cpp:1612-1660`)" — the
     truncation (`slots_`/`links_`/`content_`/`scratch_` resizes) is `:1671-1674`; the cited range
     covers only the shadow/trail restore. Cite `:1612-1700` or `:1657-1674`. Same shape at §1's
     mechanism cite `:1506-1612`, which still ends on `release`'s first line.
   - §6.4 "arena cells are freed only by `release` truncation — which also rewinds the PC" —
     `release` does not rewind the PC; `backtrack` does (`setCursorOpcode` at `vortex_vm.cpp:
     238-243`, which the doc itself cites two sentences later for precisely that). A bare
     `release` leaves `cursorPC_` stale — the doc's own §4.2/drain-bullet point. Reword to
     attribute the rewind to `backtrack`.
   - §4.2 "`enableMemoization` is the only writer (`vortex_vm.cpp:170-172`)" — the write is `:170`
     (function :169-171).
   - §6.4 "`ActorDown` (`mesh_actor.cpp:473-479`)" — `ActorDown` is selected at `:484-486`;
     `:473-479` is the try/catch.
   - §6.4 postcondition cite `:1306-1318` — postcondition evaluation is `:1313-1322`; `:1306-1311`
     is the `finalOutputs` snapshot.
   - §1 arena-navigation row cites `cell_path.cpp:147-183` — the `parseCellsPath` entry; the
     subset's substance is the validators `:17-143` (which §4.4 cites correctly).
   - §5.3 `compiler.cpp:854-856` for the `localCells_` mint — the loop is `:853-855`; the
     `output`/`produced` temporaries are `:858-859`.
   - M4 "`Checker::infer` is const-in/void-out (`checker.cpp:1121`)" — `infer` returns `Type`,
     not void; the claim it supports (the checker cannot rewrite the `Program`) is right.
   - `compiler.cpp:500-503` for the scalar-subset enumeration — lands on the `default:` refuse;
     the set itself is the case labels `:225-498`.

   **Fix.** Adjust the ten cites as listed; no design change.

5. **Severity: MINOR. Location: §6.1 notational note (:785-786).**

   **Problem.** "A bare `Code` parameter … the same wildcard the checker gives builtins like
   `text(x)`" — `text` takes `{any}` (`checker.cpp:771`) where `any = Type{"<any>"}` (:27) accepts
   *any* type, `Int` included. A bare-`Code` parameter is a *parameterized* wildcard — accept
   `Code` with any argument, reject non-`Code` — which `any` cannot express. New checker machinery
   the doc treats as existing-by-analogy.

   **Fix.** One clause in M3/§10.1 naming the wildcard form (e.g. "`Code` with elided argument is a
   new checker wildcard accepting `Code<T>` for any `T`").

6. **Severity: MINOR. Location: §9.2 example.**

   **Problem.** `plan_named("Guard.cautious")` is two-part `Agent.plan`; the convention the doc
   itself cites (spec §5.7, `vagent-agent-language.md:640`) is three-part `module:Agent.travel`.

   **Fix.** Qualify the example (`mars:Guard.cautious`-style) or state module elision once beside
   the `plan_named`/`code_named` table entries.

7. **Severity: MINOR. Location: §11 refused-alternatives (:1387).**

   **Problem.** "new C++ must justify why it is not Vortex (vagent-runtime-grounding.md's rule)" —
   the grounding file records the boundary audit the rule produces; the rule itself is project
   policy (AGENTS.md: "New C++ must justify why it cannot live in Vortex").

   **Fix.** Cite both, or "the project rule recorded in the grounding audit".

______________________________________________________________________

## Verified-without-finding spot checks (round-2 additions)

For the record — the specific claims this round was asked to verify, checked and confirmed as
stated (the one exception is finding 1):

- **`+d.refs` rank / `d.entry`**: head→rank of the fragment's `int64` ref cells, marked for P16;
  `promote` skips only `d.dims` (`arena_manifold.cpp:1765-1767`), so `d.refs`/`d.entry`/`d.recipe`/
  `d.name`/`d.patch-head` links are all followed and remapped (:1781-1783). `d.entry` gives the
  entry op a `-d.entry` backlink — no collision with wings or `+d.spin`.
- **`d.patch-head`/`d.patch`/`d.recipe`**: `+d.patch-head`→record cells `+d.step`-linked, each
  `+d.values`→a *name* cell — heads-by-name is correctly forced by the eviction rule (:968-970's
  own reasoning); genesis replay resolves names through `code_named`/`plan_named`, which is sound
  because labels are `Str` content (durable) and targets re-resolve in the new program.
- **`d.validate` fresh dim vs the no-marks-minting discipline**: `d.validate` is *minted* by the
  adapter at genesis (no marks outstanding); *links* written on it during a `validate` pass are
  ordinary trailed writes — no conflict. The discipline governs `ensureDimension` (`:78-105`),
  not `link`.
- **`expand`/`cloneExpr`/`expand.{hpp,cpp}`/`ast_clone.{hpp,cpp}`**: correctly labelled new code
  (:1331-1336); `Checker::infer` is const-in (`checker.cpp:1121`) and `Expr` children are
  `unique_ptr` (`ast.hpp:90-109`) — no clone exists today.
- **`cap$N` and `CompilerBindings` renaming**: sound as specified — `$` is rejected by the lexer
  (`lexer.cpp:333-335`) so the names are unlexable-by-construction; `snake()` accepts `cap$0`
  anyway (first-char class + no-uppercase only, `checker.cpp:30-36`); `CompilerBindings`
  (`compiler.hpp:15-19`) is a `std::less<>`-transparent string map, so the synthesized names never
  round-trip through the lexer. The two-channel mechanism (synthesized `let` for checker typing +
  `CompilerBindings` entry for kernel cell identity) is coherent with the emitter's
  `bindings_`-first lookup and the `localReferences_`/`localCells_` side tables
  (`compiler.cpp:884-891`).
- **`+d.synapse` junction cells**: sound — a rank of junction cells side-steps the one-neighbour
  star exactly as claimed; endpoints as qualified-name label cells and weight on `d.weight` are
  ordinary cell ops.
- **Graft occupancy through `-d.grab`/`-d.spin`/`-d.values` backlinks**: the backlinks are the
  reciprocal ends the install links write (`link` maintains both sides, `arena_manifold.hpp:
  594-618`); the test is three `linked(head, dim, NEG)` reads, or `cells"$h/-d.x"` one-step paths —
  inside the `cells` subset (`cell_path.cpp:107` records `-d` steps). M14's consume-on-install is
  correctly derived from the eviction semantics.
- **`run` as same-cursor `#Call`**: consistent with `Call`/`Return` (`vortex_vm.cpp:856-895` —
  frame `makeCell` :860, `setValueBits` spill :861-863, pop :880-895) and spec §5.8 (:643-655);
  the `Halt`-tail hazard is correctly diagnosed (`:1220-1225` parks the cursor; `run` reports
  "Halted" without a frame pop). Finding 3 covers the one missing piece (locating
  `result`/`produced`).
- **All four P15 write sites** at the exact cited lines — `Value`/`Splice` `vortex_vm.cpp:771-806`;
  memo-hit `:1269-1273`; generic writeback `:1287-1294` (exclusion `:1288-1290`); `runPipeline`
  `:337` and `:343-346` — and `runPipeline`'s own exclusion `:340-342` mirrors `step`'s. The
  `Value`-into-op desync path the doc describes (bind an op cell to an output wing via
  `Link`/`setLink`, re-execute) is real: `resolveCell` returns the operand cell itself
  (`vortex_vm.cpp:50-67`) and `outputsOf` resolves wing slots to arbitrary cells
  (`vortex_core.cpp:354-414`).
- **`reify` recipe**: census + link tuples + literal contents + old→new is feasible through
  `New`/`Link`/`Value` replay for literal-born graphs; the `dimensionsOf` enumeration gap for
  foreign graphs is honestly deferred (U-M7). `Clone`'s `masterOf` sharing (`vortex_vm.cpp:
  807-836`; `vortex_core.cpp:181,202`) correctly disqualifies it.
- **"Dimensions minted only when no marks are outstanding"**: `ensureDimension` has no mark check
  (`arena_manifold.cpp:78-105`) — correctly presented as adapter/caller discipline and priced at
  §7.3, not claimed as a mechanism.
- **Makefile fix**: `VAGENT_FRONTEND_SRCS` includes `apps/common/xanadu/vagent/…/cell_path.cpp`,
  `apps/common/xanadu/vql/{lexer,parser}.cpp` and `apps/common/xanadu/scanner_base.cpp`
  (`Makefile:688-690`) — the r1 build defect is fixed in the tree.
- Fresh re-verifications beyond the r1 list: `setCursorOpcode` dual write (:131-146);
  `spawnCursor`/`+d.cursors` rank (:148-161); `enableMemoization` (:169-171); `backtrack` foreign
  choice-point popping + PC/stack restore (:215-248); `Choice` alternative op-kind enforcement
  (:1150-1158) and mark push (:1168); `getCursorOpcode` map-first (:116-128); `Halt` early return
  (:1220-1225); contract evaluation (`preconditionsOf`/`postconditionsOf`, :1229-1239, :1313-1322);
  step advance (`:1333-1335`); `run` signature (`:1344-1345`); `Link`/`Break` `resolveCell`-
  mediated subject/target (:616-653); `Mul` overflow checking (:457-463); `New` bound-output
  refusal (:736-743); `Choice`/`Branch`/`Jump`/`Call` int-encoded targets (:839-873, :1150-1168);
  the drain idiom (`vortex_stdlib.cpp:3800-3802`,`:3841-3843`); `consumeWork` throws
  (`arena_manifold.hpp:420-426`); "marks are assumed well nested" (`arena_manifold.hpp:773`);
  `checkpointView` refusal (`mesh_actor.cpp:669-674`); `pureBody` + mutating-cells impurity
  (`checker.cpp:827,861-865`); import allowlist `std:agent`/`std:goal` only (`checker.cpp:
  2061-2067` — `std:meta` is correctly "gains"); `any` wildcard + `snake` (`checker.cpp:21-36`);
  `parseExpression` (`parser.cpp:271-281`); prefix gate (`parser.cpp:848-855`); adjacent-prefix
  merge (`lexer.cpp:379-398`); `PrefixedStringLiteral` (`token.hpp:22`); `cells` subset validators
  (`cell_path.cpp:17-143`) and `parseCellsPath` (:147-183); vql whole-rank refusal
  (`vql/compiler.cpp:36-80`,`:71`); `check`-only CLI (`apps/vagent/main.cpp:77`);
  `CompilerBindings`/`ScalarCompilerLimits`/`CompiledStatement` (`compiler.hpp:15-34,51-60`);
  `Emitter` no-marks precondition (`compiler.cpp:96-110`); `requireSame` (:188-193);
  reference/jump/bounded/emitStatement lowering (:512-588, :849-872); `localCells_` mint
  (:853-855); side tables (:884-891); `CellValue` (`vortex_core.hpp:29`); `noCell == 0`
  (`dim_vector.hpp:23`); opcode enum ceiling (`vortex_vm.hpp:27-93`, `Linked` last); `opcodeMap_`/
  `cursorPC_` untrailed (`vortex_vm.hpp:186-188`); `d.name`/`findModule`/`exportSymbol`
  (`vortex_core.cpp:61,100-127`); spec anchors — V10 :224-234 (including its own `arenaDims_`
  caveat :229-234), §5.7 :625-641 (`module:Agent.travel` :640), §5.8 :643-655, §5.9 :660+, §8.7
  :1239-1272, §8.1 :1046 ("any other prefix on a string literal is a syntax error"), §5.1
  `d.clause` :491-494, V12 :244, V13 :252, §11.1 :1600+, `std:agent` prelude :1536-1544, `Opt`
  presence test :1103-1104, `ConditionFault` :791/:928/:1555, U-A12 :3149-3160, P1-P14 :1667+;
  the "agents that can modify their own code are agents that can learn" quote
  (`vortex-hyperstructural-runtime.md:1047-1049`); the grounding audit's one-cursor rule and
  second-cursor bug (`vortex-runtime-grounding.md:100,133-136`).

______________________________________________________________________

VERDICT: NOT CONVERGED
