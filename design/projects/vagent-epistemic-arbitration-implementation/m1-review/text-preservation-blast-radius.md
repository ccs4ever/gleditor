# Typed-cell text preservation: blast radius

Read-only investigation, branch `design/vagent-epistemic-arbitration`. Fixed by the owner: text
inference at mint and update (`inferTextValue`, `scalar.cpp:40-79`) stays. The question is whether a
typed cell keeps the user's text, and what it would take to make it always do so.

## Verdict

**Confirmed, and narrower than feared.** The store never re-renders user text. The loss happens in
three copy-to-store roads that read a typed cell's bits and call `makeScalarCell`, discarding the
cell's spans: `zigzag::promote` and `promoteOverlay` (core), VQL `exportToStore`, and the slice DTO
round trip. `promote` is the one that matters to vagent: every checkpoint goes through it.

R6 already rules for preservation (`store-slice-convergence.md:239-252`: "User-entered scalar text
retains its exact spelling"; "the original bytes remain in the scroll"). This is a conformance fix,
not a new ruling. No on-disk format changes.

## 1. Where text is and is not replaced

Probe: `probe.cpp` in the session scratchpad (`.../scratchpad/blast/`), linked against the existing
`build/apps/common/xanadu` objects, run headless with private XDG directories. Column 1 is
`Store::makeCell(text)`. Column 2 is an arena cell holding the text and the inferred bits (the shape
`copyCell`, `copyCellData` and mesh import produce), then `promote`.

| input  | `Store::makeCell` | arena + `promote` |
| ------ | ----------------- | ----------------- |
| `1e3`  | `1e3` Double      | `1000` Double     |
| `007`  | `007` Int64       | `7` Int64         |
| `on`   | `on` Bool         | `true` Bool       |
| `yes`  | `yes` Bool        | `true` Bool       |
| `-0.0` | `-0.0` Double     | `0` Double        |
| `+5`   | `+5` Int64        | `5` Int64         |
| `1.50` | `1.50` Double     | `1.5` Double      |
| `NaN`  | `NaN` Double      | `nan` Double      |
| `42`   | `42` Int64 (same) | `42` Int64 (same) |

**Preserving (no change needed):**

- `Store::makeCell(text)` `store.cpp:761-777` and `makeCell(span)` `:736-759`: `op.span` is the
  text, `op.value`/`flags` the inferred bits. `CompactOpNode` already carries both halves.
- `setCellText` `store.cpp:955-961`, `spliceCellSpan`/`spliceCell` `:885-953`: spool the typed
  bytes, infer, keep them. Pinned by `scalar_test.cpp` `userTextEditsRefresh...` (`"0012"` stays).
- Arena `shadow` `arena_manifold.cpp:1496-1533`, `compact` `:1733-1762`, `mark`/`release`: copy
  spans verbatim, never render.
- Copiers: vagent `copyCell` `epistemic_translate.cpp:600-625`, `copyCellData`
  `reconciler.cpp:59-85`, VPL copy `vpl_engine.cpp:779-797`, mesh import `mesh_value.cpp:481-492`.
  All keep text and bits. Their output is what `promote` then flattens.

**Replacing:**

1. `zigzag::promote` `arena_manifold.cpp:1956-1966`, and the twin in `promoteOverlay` `:2366-2375`.
   For `Double`/`Bool`/`Int64` it calls `store.makeScalarCell(bits)` and never looks at `spans`. Two
   losses: the spelling, and the *address*. A typed cell whose span quoted store bytes (VPL copy,
   mesh import) is promoted as a fresh append, so the transclusion of its source is broken too.
   Callers: checkpoint (`agent_executor.cpp:5366` to `publishValue`, `mesh_value.cpp:168`), program
   staging (`program.cpp:82`), Vortex host (`vortex_host.cpp:394,617,797,856`), SQL promotion
   (`sql_promotion.cpp:142`), mesh import (`mesh_value.cpp:538`).
1. VQL `VQLCompiler::exportToStore` `vql/compiler.cpp:1043-1051`: same pattern, own copy.
1. Slice DTO: `storeToSlice` `xanadu_projector.cpp:735-741` keeps only `double`/`bool` in
   `CellData`, `sliceToStore` `:604-608` re-renders. `Int64` survives (read as text, re-inferred).
   Reached by `ZigzagVisualizer::document()` and adoption (`zigzag_visualizer.cpp:248,452`).
1. Not user text, so out of scope: annotation timestamps are canonicalised to RFC 3339
   (`store.cpp:3666-3675`); `system_docs.cpp:469-473`, `reading_place.cpp:56` mint programmatic
   scalars; `VortexCore::writeValue` (`vortex_core.cpp:205-219`) clears content when a program
   writes a typed value, so the cell is a computed scalar with no text.

**The vagent consequence is a second-hop bug.** One checkpoint is harmless: a live string `"007"` is
promoted by `Store::makeCell(text)`, keeps `"007"`, gains Int64. Resume copies it into an arena with
its text and bits. The *next* checkpoint promotes that typed ephemeral cell through `makeScalarCell`
and writes `"7"`. After the second resume `textEq("007", cell)` fails both arms (variant `Eq`, and
the whole-text slice reads `"7"`), so an exact retry of event `"007"`, `"1e3"`, `"on"` or `"yes"` is
not a duplicate, and a key spelled so stops grouping. The digest in `rendered()`
(`epistemic_translate.cpp:847-870`) also drifts between a live upgrade and a twice resumed image.
`NumericAndBooleanLooking*` (`vagent_epistemic_resume_test.cpp:242-321`) do one hop only, which is
why they pass. This end-to-end failure is inferred from the probe plus the code path. It was not run
through the executor.

## 2. Is canonical text load-bearing anywhere

No reader found that needs a *user-entered* typed cell's text to be canonical.

- Printers render from bits, not text: `formatCellBrief` `vql/ascii_visualizer.cpp:44-60`, vpl
  `formatCell`/`persistedCellText` `apps/vpl/main.cpp:45-68`. Result rows quote the source spans
  when they exist (`result_slice.cpp:42-53`). They render canonically only for computed values.
- VM comparison, arithmetic and unification read bits (`vlog-logic-extension.md:508-514`).
- Text readers of typed cells change for the better: VQL substring search (`vql_engine.cpp:1447`)
  matches what the user typed, `rendered()` and `textEq` become stable across hops.
- Publication, wire and `contentHash` hash ops and spans. They do not parse text. Bytes differ after
  the fix but stay deterministic, and no test pins a checkpoint hash value.
- Tests that pin canonical text pin *programmatic* scalars and stay valid:
  `scalar_test.cpp:205,257, 261` (`makeScalarCell`/`setScalar`) and
  `arena_manifold_test.cpp:621-638` (computed `42`, no spans). No test was found asserting that a
  promoted typed cell *with* text is re-rendered.
- Fixtures under `tests/samples/` do not change format. If a generator promotes typed cells with
  non-canonical text, regenerating them would change permascroll bytes, but they need not be
  regenerated and they still load.

## 3. The change, per road

Rule for every copy-to-store road: **spool the cell's own text whenever its text still reads as its
value; otherwise render.** "Still reads as" means `inferTextValue(text)` gives the same kind, and
the same bits after `canonicalDoubleBits`. That guard covers a cell whose bits were rewritten
without clearing content (`mesh_evaluator.cpp:652` and test seams). It also covers the raw `-0.0`
pattern that `DoublesCrossByBitPattern` plants (`vagent_epistemic_translate_test.cpp:362-379`).

- **Computed scalar, no spans** (VM, VPL, compiler constants): unchanged, `makeScalarCell`. The
  canonical rendering is the only text there is, and R6 names it as the programmatic text.
- **Typed cell, store spans** (`promote`, `promoteOverlay`): mint from the spans exactly as the
  untyped branch does (`makeCell(span)`, then `spliceCellSpan` for the rest).
  `Store::makeCell(span)` re-infers, so the value matches without further work. This also restores
  the transclusion.
- **Typed cell, scratch spans**: `makeCell(scratchTextOf(...))`, the untyped branch again. Inference
  restores the kind.
- **Text and bits disagree**: either render (today's behaviour), or keep the text and follow with
  `setValue(cell, span, kind, bits)`. Rendering is the smaller change. A single-op
  `Store::makeCell(parent, span, kind, bits)` overload would avoid the second op, but it needs no
  format change since `MakeCell` already carries a span and a value.
- **`setValue` with text**: already correct (`setCellText`). Nothing to do.
- **VQL `exportToStore`**: same guard. Use `contentOf(c)` spans when they are real.
- **Slice DTO**: carry the text beside the double/bool in `CellData`, or let `sliceToStore` mint
  `makeCell(text)` whenever the text re-infers to the value. It is a UI path and can be a follow-up.

**Format impact: none.** A typed `MakeCell` already holds span and value (`store.cpp:767-772`).
`sizeof(CompactOpNode)`, the store version and the tables are untouched, and no fixture must be
regenerated.

## 4. Blast radius

| part                                   | core?                | size                      |
| -------------------------------------- | -------------------- | ------------------------- |
| `promote` + `promoteOverlay` helper    | **core** (arena)     | ~40 lines, net shrink     |
| optional `Store::makeCell` overload    | **core** (store API) | ~20 lines; skip if render |
| VQL `exportToStore`                    | engine, not core     | ~10 lines                 |
| slice DTO text carry                   | engine/UI            | ~30 lines, follow-up      |
| vagent comments (`rendered`, `textEq`) | vagent               | wording only              |
| tests                                  | —                    | ~6 tests, ~120 lines      |

The tests to add:

- `arena_manifold_test`: promote keeps `1e3`/`007`/`on`/`-0.0` with their kinds.
- `arena_manifold_test`: a typed store-span copy promotes to the *same* address.
- `arena_manifold_test`: disagreeing text and bits fall back to canonical.
- `arena_manifold_test`: the computed `42` test is unchanged.
- `vagent_epistemic_resume_test`: a two-hop variant of both `NumericAndBooleanLooking*`.
- VQL: export of a typed quoted cell.

**Expectations that change:** none found among existing tests. Run the full `xuzz_test` and
`zigzag_test` anyway: `mesh_value_test`, `vortex_scoped_evaluation_test`,
`vagent_epistemic_translate_test` (`ResumeAndUpgradeAgree...`) and `test_xudu_convergence` touch
these roads.

**Performance.** Store-span typed cells now append *nothing* where today they append a rendering of
up to 24 bytes. Scratch typed cells append their own text instead of a rendering, about the same
length. The added cost is one `inferTextValue` per promoted typed cell (at most 128 bytes, plus a
`read(span)` for store spans), against the existing per-cell `apply`. Negligible.

**Semantic risk to Vortex/VQL/VPL.** Bits, and therefore arithmetic, comparison and unification, are
unchanged. Only `textOf` of a promoted typed cell changes, from canonical to the spelling it held. A
computed `1000` still prints `1000`. A user's `1e3` persists as `1e3` and still equals 1000 by
value. Two cells, one typed `1e3` and one computed `1000`, now differ by text and agree by value,
which is what the owner's ruling and R6 ask for. Nothing in the tree was found to text-compare a
promoted typed cell against a canonical literal.

## 5. Effect on the vagent M1 workaround

`textEq`/`typedTextEq`/`isText` (`compiler_gen.hpp:892-939`) **stay**. They handle a *kind* change
(the live string `"42"` against the resumed Int64 42), which comes from inference at checkpoint and
which the owner keeps. The fix makes the workaround *correct* rather than only correct for one hop.
Today its slice arm reads `"7"` after the second checkpoint. After the fix it reads `"007"` forever.
Two comments become stale and should be reworded, with no code change:

- `rendered()`, `epistemic_translate.cpp:847-853`: "renders a computed scalar canonically" stays
  true. Add that a typed cell's text survives any number of hops.
- `textEq`, `compiler_gen.hpp:905-913`: "The cell keeps its text" becomes true across hops.

There is a cheaper vagent-only alternative: canonicalise at ingress so every hop agrees. It would
contradict the owner's ruling (text stays the user's text), so it is not recommended.

## Recommendation

Fix `promote`/`promoteOverlay` with the "spool own text when it reads as the value, else render"
rule, using the render fallback for disagreement so that no Store API is added. Add the two-hop
resume tests in the same change, since they are the observable vagent defect. Do VQL `exportToStore`
alongside (same helper idea, not core). Leave the slice DTO for a UI follow-up.

- **Core, needs separate owner approval:** `apps/common/xanadu/zigzag/arena_manifold.cpp`
  (`promote`, `promoteOverlay`). The optional `Store` overload would be core too. It is not
  recommended unless disagreement must keep text.
- **Not core:** VQL `exportToStore`, the vagent comment updates, the tests, and a short note in
  `vlog-logic-extension.md` §5.5 ("deferred" applies to computed scalars only; a typed cell with
  text spools its text).
- **Size:** small. About 60 lines of production code (about 40 core) and about 120 lines of tests.
  No format bump and no fixture regeneration. Roughly half a day including a full `make test` run.
