# Round 2 critique: Xanadu & ZigZag Purist (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.B1 (R1 AST topology contradicts canonical 2-degree schema): RESOLVED — Section 3 canonical
  topology and Section 4 Track 1.2 standardize node kind in cell text (`arena.makeCell(kindTag)`),
  first child on `+d.grab`, siblings on `+d.step`, type annotations on `+d.values`, and no spurious
  `+d.kind` dimension.
- R1.B2 (Symbol resolution lacks clone views, causing fan-in): RESOLVED — Section 3 (lines 149–151)
  and Track 2.1 (line 205) explicitly mandate dedicated clone views on `Decl +d.clone` or `+d.refs`
  chains to prevent 2-degree fan-in eviction.
- R1.B3 (Permascroll provenance and transclusion omitted): RESOLVED — Grounded durable artifacts and
  execution state in `LineageStore` descriptors and Merkle roots; source tokens carry
  `(line, col, length)` coordinate spans for text slicing.
- R1.M1 (Goal plan gathering confuses +d.step with clause rank): RESOLVED — Track 4.1 (line 249)
  replaces `+d.step` with `+d.clause` for goal symbol plan anchoring.
- R1.M2 (Hallucinated assertRankInvariants API cited as verification): RESOLVED — Replaced
  non-existent API in Validation Gate 1 with `zigzag::crdt::CrdtEngine::verifyManifoldInvariants()`.
- R1.M3 (Foreign serialization of compound types in strings): RESOLVED — Section 1 and Section 3
  specify homogeneous ZigZag cell graphs for types without C++ object pointers or foreign string
  blobs, connected via `+d.values`.
- R1.M4 (Fragment packaging omits d.mesh.relocation markers): RESOLVED — Section 3 (lines 152–154)
  and Track 3.1 (lines 231–232) explicitly mandate `+d.mesh.relocation` markers on all fragment
  reference cells to support `ArenaManifold::promote()`.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§4 Phase 1, line 181) Source byte offset on Token records

**Problem:** Track 1.1 states `Token` records are created with coordinates `(line, col, length)`. In
Nelsonian hypermedia and ZigZag space, line and column numbers are ephemeral display projections;
the primary coordinate is the character or byte offset within the source subject (`offset`).
Tracking `offset` enables direct conversion to `Span` (`offset`, `length`) and bi-directional
transclusion beams back to `UserPermascroll` or `LineageStore` text.

**Evidence:**

- `design/vagent-self-hosting-migration.md:181`:

  > *"Instantiates `Token` records with source coordinates (`line`, `col`, `length`) via dynamic
  > record expressions (`P22`)."*

- `apps/common/xanadu/enfilade/spanfilade.hpp:24–35`: Span representations rely on
  `(offset, length)` tuples over permascroll character memory.

**Fix:** Augment `Token` coordinates to include byte `offset`:
`Token{kind, text, offset, length, line, col}`, ensuring a seamless bridge between lexer tokens and
Nelsonian `PrimediaSpan` transclusion.

______________________________________________________________________

### 2. (§4 Phase 1, lines 194–198) In-manifold AST rank invariant validator

**Problem:** Validation Gate 1 cites `zigzag::crdt::CrdtEngine::verifyManifoldInvariants()`, which
checks low-level bilateral link integrity ($A.\text{pos}(d) = B \iff B.\text{neg}(d) = A$). However,
an in-language validation helper in `stdlib/ast.vagent` (e.g.
`ast_verify_invariants(root: Cell): Bool`) that traverses `+d.grab`, `+d.step`, `+d.values`, and
`+d.name` to verify that 2-degree bounds hold and that no cycles exist would allow self-hosting
compiler passes to assert AST integrity natively within the VM before lowering.

**Evidence:**

- `design/vagent-self-hosting-migration.md:195–198`:

  > *"verifies that the emitted R1 AST cell graph is structurally isomorphic under
  > `zigzag::crdt::CrdtEngine::verifyManifoldInvariants()`."*

- `apps/common/xanadu/zigzag/crdt_zz.hpp:100–101`: `verifyManifoldInvariants` verifies manifold rank
  reciprocity, but domain-specific AST tree invariants are checked by separate traversal.

**Fix:** Specify that `stdlib/ast.vagent` exports an `ast_verify_invariants(root: Cell): Bool`
routine that validates 2-degree bounds and acyclicity natively across R1 AST ranks before lowering.

______________________________________________________________________

VERDICT: CONVERGED
