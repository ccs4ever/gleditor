---
name: vortex-design-critique
description: >-
  Multi-agent critique workflow for Vortex, ZigZag, Spool-Mesh, and surface-language design
  documents and proposals. Orchestrates six cheaper read-only subagent critics (Technical Writer,
  Vortex Expert, Codebase Expert, System Architect, Programming Language Design & Ergonomics
  Expert, and Xanadu/ZigZag Purist) across iterative rounds with disposition tracking until all six
  converge.
---

# Vortex & ZigZag Design Critique Workflow

This skill defines the multi-agent review protocol for stress-testing design documents and
architectural proposals across the Vortex hyperstructural runtime, ZigZag (`Manifold` /
`ArenaManifold`), Spool-Mesh distributed actors, and the Vortex surface languages (`VQL`, `VPL`,
`VProlog`, `Vicon`, `VLace`, `vagent`).

The orchestrating agent spawns **six specialized critics in parallel using cheaper subagent models**
(`flash` or `sonnet`), collects their structured critiques, writes an explicit disposition after
each round, revises the specification, and re-runs follow-up rounds until all six critics emit
`VERDICT: CONVERGED`.

```
       +---------------------------------------------------------------------+
       |                   Orchestrator (Lead Editor)                        |
       |  - Dispatches 6 cheaper read-only subagents in parallel             |
       |  - Verifies claims against C++23 source (file:line)                 |
       |  - Writes r<N>-disposition.md (accepted / deferred / refused)       |
       |  - Revises design doc & maintains numbered Proposal Register (P<k>) |
       +---------------------------------------------------------------------+
             |           |            |            |            |           |
             v           v            v            v            v           v
       +----------+ +----------+ +----------+ +-----------+ +--------+ +----------+
       |  writer  | |  vortex  | | codebase | | architect | |  ergo  | |  zigzag  |
       | Technical| |  Vortex  | | Codebase | |  System   | |PL & Erg| | Xanadu / |
       |  Writer  | |  Expert  | |  Expert  | | Architect | | Expert | |ZZ Purist |
       +----------+ +----------+ +----------+ +-----------+ +--------+ +----------+
             \           \            |            |            /           /
              +-----------+-----------+------------+-----------+-----------+
                                            |
                                            v
       +---------------------------------------------------------------------+
       |            Convergence Gate: 0 BLOCKING, 0 MAJOR across all 6       |
       |     Core changes isolated in Proposal Register for Owner Approval   |
       +---------------------------------------------------------------------+
```

______________________________________________________________________

## 1. House Style and Standing Rules for Design Documents

Every critic and the orchestrator enforce the repository's standing design rules:

1. **Argued, Not Asserted**: Designs justify every mechanism by citing exact repository files and
   line numbers (`file:line`), pricing the cost (bytes, cycles, links, messages, segments), and
   recording refused alternatives with the technical reason they were rejected.
1. **Cells and Dimensions Over Side Tables or Bit-Packing**: State, structure, and metadata live on
   orthogonal ZigZag ranks (`d.*`), never in ad-hoc C++ side tables, JSON/bencode blobs stuffed into
   scalar cell text, or bit-packed integer fields outside `CompactOpNode`.
1. **No Sentinels in APIs**: Absent values and fallible operations use `std::optional` or
   `std::expected`, never reserved sentinel integers or empty-string conventions.
1. **No Arbitrary Ceilings**: Do not invent fixed small caps that force user workarounds; when a
   bound is required for resource safety, price it and expose it as a documented setting or grant
   limit.
1. **Vortex Over C++**: New C++ code must justify why it cannot be expressed in Vortex bytecode or a
   Vortex standard library routine.
1. **Proposal Register for Existing Code**: Any change to existing C++ engine, core, stdlib, or
   runtime files must be isolated in a numbered Proposal Register (`P1`, `P2`, ...) stating the
   exact files touched, why the change cannot live in the caller/compiler, the cost, and the
   fallback if not approved.

______________________________________________________________________

## 2. The Six Critic Personas

Each critic runs as a **read-only** subagent using a cheaper model (`Model: "flash"` when invoking
subagents in Antigravity, or `model: "sonnet"` in Claude Code). Critics never edit repository files;
they write their numbered findings to their assigned round file `r<round>-<role>.md`.

### 2.1 Technical Writer (`writer`)

- **Lane**: Clarity, internal consistency, specification completeness, cross-reference accuracy,
  example conformance, and Markdown hygiene.
- **Checklist**:
  - Do terms, dimension names, event names, and record field names match identically across prose,
    tables, diagrams, EBNF grammar rules, and worked examples?
  - Are every section's forward and backward references (`§X.Y`) accurate, and do the worked
    examples actually parse and type-check under the document's own rules?
  - Does the document follow house style (priced rulings, cited `file:line` evidence, refused
    alternatives section, open questions table)?
  - Does the file satisfy `mdformat --wrap 100` (with `mdformat-gfm`, `mdformat-dollarmath`,
    `mdformat-frontmatter`) and `mdl -i` (`MD013` 100-column limit, fenced code blocks, no bare
    `#include` lines outside fences)?

### 2.2 Vortex & Spool-Mesh Expert (`vortex`)

- **Lane**: `VortexCore` execution semantics, bytecode graph lowering, trail/backtracking behavior,
  handle relocation, and `MeshActorRuntime` distributed actor invariants.
- **Primary Code & Docs to Cross-Check**:
  - `apps/common/xanadu/vortex/vortex_core.{hpp,cpp}`, `vortex_vm.{hpp,cpp}`,
    `vortex_host.{hpp,cpp}`, `vortex_stdlib.{hpp,cpp}`, `mesh_actor.{hpp,cpp}`,
    `mesh_evaluator.{hpp,cpp}`, `or_parallel.{hpp,cpp}`
  - `apps/common/xanadu/zigzag/vlog.{hpp,cpp}`
  - `design/vortex-hyperstructural-runtime.md`, `design/vlog-logic-extension.md`,
    `design/vortex-distribution.md`
- **Checklist**:
  - **Opcode & Cursor Semantics**: Are `#Call`/`#Return`, `#Jump`/`#Branch`, `#Choice`/`#Fail`,
    `#Isolate`, `#Bind`/`#Resolve`, and `#Point` (`OpHandle`, `P16`) used according to their real
    implementation in `VortexCore`? Remember: `#Call`'s internal activation frame is not addressable
    by user opcodes; `OpHandle` cells require a marker on `d.mesh.relocation` to survive
    `promote()`.
  - **Trail & Backtracking**: Does any loop or generator rely on `#Choice`/`#Fail` backtracking
    where the arena trail will silently undo side effects or cell mutations inside the body?
  - **Turn Atomicity & Resource Bounds**: Are `maxCycles`, `maxFuel`, `maxDepth`, `valueLimits`, and
    `maxOutputBytes` respected? What happens on trap, fuel exhaustion, or unfinished execution?
  - **Spool-Mesh Invariants**: Do actor messages, `ActorGrant` (`programRef`, `baseRef`), causal
    floors, `ActorDeliverValue`/`ActorDeliverWriteSet` replies, deadlines, and `promote()` /
    checkpoint / resume obey `MeshActorRuntime` rules without cross-arena raw `CellRef` leaks?

### 2.3 Codebase Expert (`codebase`)

- **Lane**: Ground-truth verification against the live C++23 repository, exact API signatures, data
  structure invariants, and proposal-register accounting.
- **Primary Code to Cross-Check**:
  - `apps/common/xanadu/store.{hpp,cpp}`, `ops.hpp`, `compact_op.hpp`,
    `segmented_ops_spool.{hpp,cpp}`, `store_tables.hpp`
  - `apps/common/xanadu/zigzag/manifold.{hpp,cpp}`, `arena_manifold.{hpp,cpp}`, `cell_views.hpp`,
    `dimension_registry.{hpp,cpp}`
  - Existing language implementations under `apps/common/xanadu/{vql,vpl,vprolog,vicon,vagent}/`
- **Checklist**:
  - Verify every claim the design makes about existing classes, methods, and behavior by reading the
    exact `file:line`. Flag any hallucinated method, wrong signature, or misstated behavior as
    `BLOCKING`.
  - Check whether `ArenaManifold` vs. `Manifold` semantics are respected (e.g., `ArenaManifold`
    dimension registration in `arenaDims_`, `isEphemeral(CellRef)`, `shadow()`, `promote()`).
  - Verify `CompactOpNode` 64-byte size/alignment invariants and `Store::replay()` /
    `rebuildManifold()` behavior whenever persistence is touched.
  - Audit the Proposal Register: does the design silently require modifying an existing file in
    `vortex/`, `zigzag/`, `identity/`, or `src/` without listing a numbered proposal `P<k>`?

### 2.4 System Architect (`architect`)

- **Lane**: Architectural layering, DRY consolidation, C++-vs-Vortex division of labor, resource
  lifecycle, performance/memory sympathy, and configuration hygiene.
- **Primary Docs & Rules to Cross-Check**:
  - `AGENTS.md`, `GEMINI.md`, `.claude/skills/system-architect/SKILL.md`
- **Checklist**:
  - **Layer Separation**: Does generic library code (`src/`, `include/gleditor/`) remain 100% free
    of Xanadu/ZigZag/Vortex references? Does headless engine code (`apps/common/xanadu/`) remain
    100% free of UI/graphics dependencies (`apps/common/ui/`, `apps/xuzz/`)?
  - **C++ Justification**: For every proposed C++ helper, adapter pass, or runtime table, ask: *why
    isn't this written in Vortex or represented as cells in the manifold?* Flag unjustified C++
    machinery as `MAJOR` or `BLOCKING`.
  - **DRY & Consolidation**: Does the proposal duplicate value encoding, rank walking, dimension
    registration, or compiler lowering that already exists in `vortex_stdlib`, `cell_views.hpp`, or
    a sibling compiler (`vql`, `vpl`, `vprolog`, `vicon`, `vagent`)?
  - **Mechanical Sympathy & Retention Bounds**: Are maps, outboxes, delivery tables, and ranks
    bounded or reaped so long-running actors/stores cannot leak memory unboundedly? Are hot paths
    free of unnecessary heap allocations and string parsing?

### 2.5 Programming Language Design & Ergonomics Expert (`ergo`)

- **Lane**: Surface syntax, grammar unambiguousness, static semantics, type/shape checking, failure
  handling, family consistency, and developer ergonomics.
- **Primary Docs to Cross-Check**:
  - `design/vql-agent-experiment.md`, `design/vicon-goal-directed-language.md`,
    `design/vlace-functional-language.md`, `design/vagent-agent-language.md`
- **Checklist**:
  - **Grammar & Parsing**: Test every EBNF production for shift/reduce and lookahead ambiguities
    (e.g., `StructLiteral` `Ident '{'` vs. block `{` in `if`/`for`/`while`/`on`/`scan` headers;
    range keywords like `to` colliding with statement clauses; optional trailing clauses; newline
    vs. semicolon statement termination).
  - **Static Type & Effect Discipline**: Are type rules, mutability/purity restrictions (e.g.,
    read-only `answer` or guard clauses), and suspension-point positions checkable statically with
    clear diagnostic codes?
  - **Scope, Binding & Lifetime**: Are variable binding, pattern matching (`?v` vs. bare
    identifiers), shadowing, and closure/frame capture rules unambiguous?
  - **Ergonomics & Cognitive Load**: Does the design force boilerplate, error-prone manual
    bookkeeping, or surprising defaults on the programmer? Are sibling Vortex languages (`VQL`,
    `VPL`, `VProlog`, `Vicon`, `VLace`, `vagent`) consistent in their lexical and structural
    conventions?

### 2.6 Xanadu / ZigZag Purist (`zigzag`)

- **Lane**: Nelsonian hypermedia and ZigZag topological purity, the strict 2-degree rank invariant,
  and `Store`–Slice convergence.
- **Primary Docs & Sources to Cross-Check**:
  - Ted Nelson's *Literary Machines*, *Possiplex*, and primary ZigZag specifications (ranks,
    dimensions, crums, enfilades, clone master/instance rules)
  - `design/store-slice-convergence.md` (R1–R12), `design/vortex-hyperstructural-runtime.md`
  - `apps/common/xanadu/zigzag/manifold.hpp`, `arena_manifold.hpp`
- **Checklist**:
  - **Strict 2-Degree Invariant (No Fan-In on One Dimension/Direction)**: In ZigZag,
    `A.pos(d) = B <=> B.neg(d) = A`. Two cells **cannot** both link to the same target cell `T`
    along `+d` (or both along `-d`); the second `ArenaManifold::link` evicts the first! Any
    many-to-one relationship must either chain elements as a linear/circular rank along `d` or link
    each referrer to its own **clone view cell** on `T`'s `+d.clone` rank (always posward of the
    master, never negward). Audit every diagram and lowering rule for hidden fan-in.
  - **No Foreign Serialization Inside Cells**: Structured records, lists, ASTs, and activation
    frames must be represented as ZigZag cells and links (`d.vars`, `d.values`, `d.elts`,
    `d.stack`), never serialized into strings or hidden in C++ structs when the design claims
    manifold grounding.
  - **Hypertime & Transclusion Provenance**: When structure is persisted to a `Store`, does it fold
    cleanly from OSMIC `OpKind::Structure` (`MakeCell`, `SetLink`, `SetValue`, `Splice`) where a
    `CellRef` is the `MakeCell` op index (R7) and text content is transcluded from the permascroll?

______________________________________________________________________

## 3. Severity Definitions and Critique Format

Every critic must classify each finding using these exact three levels:

- **`BLOCKING`**: The design cannot work as specified, violates a hard invariant (e.g., 2-degree
  link eviction, trail rollback destroying state, ambiguous grammar that mis-parses its own
  examples, cross-arena raw `CellRef` dereference), or contradicts existing C++ code without a
  numbered proposal in the Proposal Register.
- **`MAJOR`**: An implementer would build the wrong thing, hit an unspecified failure/edge case, or
  be blocked; a layer boundary or C++-vs-Vortex rule is violated; or a proposal in the register
  lacks justification, pricing, or a fallback.
- **`MINOR`**: Wording polish, table/diagram formatting, minor redundancy, or non-blocking
  clarifications.

### Required Format for Each Critique (`r<round>-<role>.md`)

```markdown
# Round <N> critique: <Role Name> (<doc-filename>)

Paths below are relative to the repo root.
Tally: <B> BLOCKING, <M> MAJOR, <m> MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R<prev>.1 (<short title>): RESOLVED | PARTLY | OPEN — <one-line reason>

## BLOCKING

### 1. (§X.Y, line L) <Short title under 80 chars>

Full title: <Full descriptive title if shortened above>

**Problem:** <Precise statement of the defect or contradiction.>

**Evidence:** <Exact quotes from the doc + `file:line` citations from the repository.>

**Fix:** <Concrete replacement wording, grammar production, cell/dimension topology, or proposal.>

## MAJOR
...

## MINOR
...

VERDICT: CONVERGED | NOT CONVERGED
```

______________________________________________________________________

## 4. Orchestration Protocol (Step-by-Step)

### Step 1: Set Up the Review Directory and Common Brief

Create a review directory `design/projects/<doc-slug>-review/` (or `/tmp/<doc-slug>-review/` during
fast scratch iteration before copying the final record into `design/projects/<doc-slug>-review/`).
Write `brief-common.md` naming:

- The target design document path and repository root.
- Relevant sibling design docs and source directories.
- The house style rules (§1 above) and output format rules (§3 above).

### Step 2: Launch Round 1 (All 6 Critics in Parallel)

Invoke all six critics in a **single `invoke_subagent` tool call** using `TypeName: "self"` (or
`"research"` when write tools are not needed for scratch output) and **`Model: "flash"`** (or the
cheapest capable model available) so they run concurrently at low cost:

1. `writer` -> writes `r1-writer.md`
1. `vortex` -> writes `r1-vortex.md`
1. `codebase` -> writes `r1-codebase.md`
1. `architect` -> writes `r1-architect.md`
1. `ergo` -> writes `r1-ergo.md`
1. `zigzag` -> writes `r1-zigzag.md`

### Step 3: Verify, Write `r<N>-disposition.md`, and Revise

When the six subagents report back:

1. **Verify every `BLOCKING` and `MAJOR` finding** against the actual codebase (`file:line`).
1. **Write `r<N>-disposition.md`** recording the orchestrator's ruling on every finding:
   - **Accepted**: How the design document was changed to resolve it.
   - **Deferred (`U-X<k>`)**: Recorded in the document's Open Questions table with a concrete
     default or scope boundary.
   - **Refused**: The technical reason why the critic's premise does not hold or conflicts with a
     higher-priority invariant (so follow-up rounds do not re-litigate it without new evidence).
   - **Proposal Register Updates (`P<k>`)**: Any newly identified changes to existing C++ files.
1. **Revise the design document**, bump its version header, and run `mdformat --wrap 100` and
   `mdl -i` on the updated document and review files.

### Step 4: Follow-Up and Focused Rounds Until Convergence

- **Rounds 2+ (Follow-Up)**: Re-invoke every critic that emitted `VERDICT: NOT CONVERGED` (plus any
  converged critic whose domain was substantially altered by the revision). Provide each critic with
  its previous critique (`r<N-1>-<role>.md`) and `r<N-1>-disposition.md`.
- **Focused Rounds (Once `BLOCKING == 0`)**: Switch to focused instructions: report **only**
  `BLOCKING` and `MAJOR` findings (plus at most 5 `MINOR` bullets at the end), and forbid upgrading
  minor polish to keep the review running.
- **Stopping Condition**: Stop when all six critics emit `VERDICT: CONVERGED` (zero `BLOCKING` and
  zero `MAJOR` findings remaining). Apply any final `MINOR` polish, update
  `design/projects/<doc-slug>-review/README.md` with the round summary table, and present any
  numbered core proposals (`P1..Pn`) to the user for approval.
