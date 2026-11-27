# Round 1 disposition (M7-VQL-d2 code)

Target `5f973a10`. Reports: [`r1-zigzag.md`](r1-zigzag.md) (0/1/4, not converged),
[`r1-vortex.md`](r1-vortex.md) (0/0/2, converged). The zigzag MAJOR and the vortex MINOR 1 are the
same defect. All findings were confirmed and fixed or answered; no core file changed (only
`arena_manifold` is changed by P11 itself, and this round does not touch it), so no P13.

| finding              | verdict   | disposition                                                            |
| -------------------- | --------- | ---------------------------------------------------------------------- |
| zigzag M1 = vortex 1 | confirmed | fixed: `join_relation.cpp`, two tests                                  |
| zigzag m1            | confirmed | fixed: `quotedSpansOf` in `result_slice`, used by `vquery`; unit test  |
| zigzag m2            | confirmed | documented (not checkable at the join), header comment and design note |
| zigzag m3            | confirmed | fixed: `vql.join` debug line                                           |
| zigzag m4            | partly    | one-store CLI test added; two-store CLI shown unreachable (below)      |
| vortex m2            | confirmed | documented in the design note; test pins it                            |

**zigzag M1 / vortex 1.** The text fallback tested `piece.store != nullptr`, so a store-less foreign
or external-scroll piece kept raw spans, read afterwards in the arena's own namespace. It now types
every piece that names a byte and is not free (store-less, not foreign, all local). Tests:
`AStoreLessForeignRowIsTypedNotCopiedAsRawSpans` and
`AMixedDisagreeingJoinTypesEveryForeignRowAndKeepsLocalOnes` (a store-less space attached with only
a reader, as `overlay.cpp:406` allows); both fail before (the joined text read `ext-one` twice, and
a foreign scroll id survived into the cell).

**zigzag m1.** The fallback now lives in `quotedSpansOf(arena, master, captureStore)`: the
`quotedRun` run, else a store-less cell's spans against the capture store, scratch spans rebased
into `QuotedSpans::scratch` in both. A cell of nothing but scratch quotes nothing. Test:
`AJoinOfCapturedCellsKeepsTheCapturesAndTypesTheSeparator`; fails with the old none-of-scratch guard
restored. A command-line test needs a capture with two pieces, and a 300 kB read still yields one
cell, so this stays a unit test over the function `vquery` calls.

**zigzag m2.** A span has no permascroll identity, so "free" pieces sharing the anchor's permascroll
cannot be enforced at the join; the precondition is now stated beside `namespaceOf` and in the
design note (every store-less local cell a query meets is a capture of the author's one
permascroll).

**zigzag m3.** One `GLEDITOR_LOG_DEBUG("vql.join", ...)` line with the row count.

**zigzag m4.** `AJoinOfALoadedStoresRowsSavesTranscludedPlusSeparators` runs `vquery` on a copy of
the fixture and asserts exactly one span per separator lies past the old permascroll end. It is a
pin of d2's behaviour (it would also fail on slice d, which typed the join), not a regression test
of this round. With two stores loaded `##/+d.dims` answers the composite's own typed dimension cells
and no VQL path returns both stores' content cells, so the two-store case stays at `JoinHarness`.

**vortex m2.** Documented under "Gate round 1 fixes" in the design note;
`APlainMixedCellIsCarriedLikeAQuoteByItsOwnStore` pins it: carried and transcluded to a store over
the same permascroll, `UnnameableContent` to another. Not a fail-before test (it pins the post-d2
behaviour; the pre-d2 raw write is the thing it forbids).

## Fail-before

Fix 1 tests: run with the old `join_relation.cpp`, 2 of 10 `JoinHarness` tests fail. Fix 2 test:
fails with the none-of-scratch guard. The remaining new tests pin behaviour already present.
