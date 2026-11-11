# Round 2 critique: Xanadu / ZigZag Purist (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (Cross-Arena Ephemeral CellRef Leak in Swarm Coordinator): RESOLVED — §4.1 confines
  intra-file compilation passes to single-turn `behavior StagedCompiler` within a single arena,
  while §4.2 repositions multi-agent swarm coordination to whole-compilation-unit exchanges of
  immutable `MicroversionId` strings.
- R1.2 (Active 2-Degree Eviction Bug in Cited zzCloneToChain): RESOLVED — §3.1 and Proposal P27
  explicitly mandate finding the posward tail of the master's `d.clone` rank
  (`rankEnd(symbolOp, core_.dims().clone, DimVector::POS)`) before appending new clone view cells,
  preserving the 2-regular invariant.
- R1.3 (Under-Specified promote Skips d.dims and Relocations): RESOLVED — §3.2 and Proposal P27 bind
  mutable `Store*`, derive `parentVersion` from `primaryCurrentVersion()`, include `d.dims` and
  `+d.mesh.relocation -> "cell"` targets in `extraRoots`, execute post-commit P16 slot restatement,
  and return `Result<Str, ZigzagError>`.
- R1.4 (Undefined Dimension Topology for Pattern Matching): RESOLVED — §6.3 defines exact 2-degree
  manifold topology (sequential cell ranks along `+d.step` for tuples; `+d.grab` to head child and
  `+d.step` across sibling arguments for constructor AST nodes) and eliminates foreign array
  indexing.
- R1.5 (Violation of No-Sentinels Rule in promote Signature): RESOLVED — §3.2 and §8.4 update the
  surface signature to return typed `Result<Str, ZigzagError>` with explicit `budget: Opt<Int>`,
  eliminating forbidden sentinel strings or integer trap conventions.
- R1.6 (Conflation of ActorGenesis and ActorLineage in spawn_image): RESOLVED — §5.2 and Proposal
  P29 specify dispatching `MeshActorRuntime::spawn` with `ActorLineage{cachedFold.get(), store}`
  anchored in the runtime image cache, guaranteeing `WorkerContext::baseManifold` never dangles.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§4.2, lines 344, 364) Event Name Alignment Between Swarm Sequence and Plan Header

In the sequence diagram (§4.2, line 344), the worker actor sends
`tell UnitCompiled("main.vagent", ver)`, while the coordinator implementation (§4.2, line 364)
declares `plan HandleResultPlan for HandleCompiledUnit(path, version)`. Align the message / percept
identifier to `UnitCompiled(path, version)` in both places for strict consistency.

### 2. (§3.2, line 230) Explicit Primitive Mapping for CellValue Kind in set_value

Section 3.2 specifies `set_value(cell: CellRef, value: CellValue) : Result<CellRef, ZigzagError>`
and cites `ArenaManifold::setValueBits`. To ensure implementers do not attempt to encode complex
aggregate structures through `set_value`, clarify in the text that `CellValue` represents the
primitive scalar variant (`Int64`, `Double`, `Bool`, `Timestamp`, `OpHandle`, `None`), while
aggregate structures must be constructed via ZigZag cells and dimensions.

______________________________________________________________________

VERDICT: CONVERGED
