# Round 2 critique: Xanadu / ZigZag Purist (`vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 2 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (Base-to-overlay linkage & Rule V12): PARTLY — Opcode scratch COW shadowing eliminated, but
  §2.3 item 1 still claims overlay cells may link to base cells.
- R1.2 (Dimension overloading & fan-in in root table): RESOLVED — `d.vagent.field` and overloaded
  `d.vagent.type` removed; canonical `d.vars`/`d.values` used.
- R1.3 (Preservation of `d.clone` and `d.capacity`): PARTLY — Replaced by flawed placement of skill
  clone chains in base manifold with uncloned fan-in from overlays.
- R1.4 (Permascroll transclusion model): RESOLVED — §2.1 now models program store as EDL
  transcluding from author's deployment permascroll with `historyFromSeal`.
- R1.5 (Suspension table ZigZag rank modeling): PARTLY — C++ tuple removed, but step cell connection
  to resume opcode lacks named dimension.
- R1.6 (Structural plan navigation): RESOLVED — Delimited compound strings removed in favor of
  standard qualified plan names.
- R1.7 (Mermaid fan-out labeling): PARTLY — Diagram edges labeled, but Base graph conflates runtime
  dimensions `d.skill`/`d.behavior` with bytecode/plans and invents `d.plan`.
- R1.8 (Dimension naming disparity): RESOLVED — `d.vagent.*` fictitious dimensions completely
  eliminated.

## BLOCKING

### 1. (§2.3, line 232) Skill master-to-clone chain in base manifold violates 2-degree invariant

Full title: Misplacement of skill master-to-clone chain in base manifold violates strict 2-degree
invariant, Rule V12, and capacity dispatch

**Problem:** §2.3 item 4 states:

> "Preservation of `d.clone` and `d.skill`: Skills with multiple capacity instances maintain their
> master-to-clone chain along `d.clone` in the base manifold (`compiler.cpp:4055–4075`). Overlay
> mountings link the master instance into the agent's `d.skill` rank without duplicating the clone
> chain."

This creates four severe topological and architectural violations:

1. **Uncloned Fan-In Across Multiple Agents:** The base `programManifold` is immutable (`PROT_READ`)
   and shared across all agents on a node running that `programRef`. In ZigZag, every link is
   strictly reciprocal: $A.\text{pos}(d) = B \iff B.\text{neg}(d) = A$. A cell has degree $\le 1$ in
   each direction on each dimension. If skill $S$'s master cell $M$ resides in the base manifold,
   and Agent 1 links $M$ into its `d.skill` rank, then
   $M.\text{neg}(d.\text{skill}) = \text{Agent}_1$. When Agent 2 mounts the same skill, setting
   $\text{Agent}_2.\text{pos}(d.\text{skill}) = M$ forces
   $M.\text{neg}(d.\text{skill}) = \text{Agent}_2$, which evicts Agent 1! In `ArenaManifold::link`
   (`arena_manifold.cpp:1339`), linking to a base cell unconditionally triggers `shadow(*to)`,
   forcing each agent to copy $M$ into its private overlay anyway.
1. **Violation of State vs. Program Separation (Rule V12):** In `vagent`, a skill can declare
   mutable `var` fields (`design/vagent-agent-language.md` §5.3: "holds the skill's fields on
   `d.vars`"). A skill frame is mutable agent state. Placing $M$ in the base manifold freezes it in
   `PROT_READ` memory where fields cannot be modified, or mutates shared memory across distinct
   agents. Rule V12 flatly prohibits state cells from linking into the program.
1. **Inversion and Breakage of Capacity Dispatch:** In `compiler.cpp:4093–4095` and
   `vagent-agent-language.md` §5.3, the cells appended to an agent's `d.skill` rank are the
   **capacity instance cells**, *not* the master frame cell. Method dispatch walks `d.skill`,
   matches `d.capacity`, and traverses along `-d.clone` back to the master frame cell (`#Deref`
   follows `d.clone` to the master). Linking the master frame to `d.skill` leaves capacity instances
   unlinked and unreachable, breaking capacity dispatch entirely.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:232–235`
- `apps/common/xanadu/vagent/compiler.cpp:3876–3885, 4058–4075, 4093–4095`
- `design/vagent-agent-language.md:518–525` (§5.3)
- `design/vagent-agent-language.md:226–233` (Rule V12)

**Fix:** Correct the topology:

1. The base program manifold holds only the **skill declaration manifest** (method signatures,
   effect contracts, parameter types).
1. When an agent mounts a skill, the skill's runtime instance cells—the master frame cell (holding
   `d.vars`/`d.values`) and its capacity instance cells along `+d.clone` and `+d.capacity`—are
   minted entirely within the **agent's private ephemeral overlay arena** (`context.arena`).
1. The capacity instance cells are appended to the agent's `d.skill` rank in the overlay.
1. No manifold edges cross between the agent's skill instances and the base program manifold.

______________________________________________________________________

## MAJOR

### 1. (§2.3, line 223) Inward overlay-to-base links violate Rule V12

Full title: Permitting overlay-to-base links contradicts Rule V12, causes COW shadowing of base
cells, and conflicts with the document's own register architecture

**Problem:** §2.3 item 1 states:

> "Overlay-to-Base Links Are Inward-Directed Only: An overlay cell may link to a base cell along any
> dimension (`overlay.pos(d) = base`). In `ArenaManifold`, reading `neg(d)` from `base` through the
> overlay reflects the overlay cell via `overlay_` lookup without mutating the frozen base."

1. In `ArenaManifold::link(from, dim, dir, to)`
   (`apps/common/xanadu/zigzag/arena_manifold.cpp:1339`), when `to` names a base cell
   (`!isEphemeral(*to)`), the implementation unconditionally executes:
   `const auto target = (to.has_value() && *to != noCell) ? shadow(*to) : std::nullopt;` It copies
   the base cell into `slots_` and enters it into `overlay_` and `shadowOrder_`. It does *not*
   perform a lightweight reflection; it performs full copy-on-write shadowing.
1. Rule V12 (`design/vagent-agent-language.md:226–233`) explicitly rules: "No state cell links into
   the program."
1. In §1.3, §2.2, and §4.3, mutable state is referenced symbolically or via overlay activation
   frames on `d.vars`/`d.values`. There is zero operational need for an overlay state cell to link
   directly to a base code cell.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:223–225`
- `apps/common/xanadu/zigzag/arena_manifold.cpp:1338–1340, 1464–1503`
- `design/vagent-agent-language.md:226–233` (Rule V12)

**Fix:** Reaffirm Rule V12 in §2.3 item 1: state cells never link to base program cells. All
cross-boundary references are coordinate arguments (canonical `DimRef`s passed to
`link()`/`linked()`) or VM instruction pointers held in `WorkerContext`, never manifold edges
between cells.

______________________________________________________________________

### 2. (§2.2, lines 160–171) Dimension conflation in program root diagram

Full title: Program root links conflate runtime agent dimensions (`d.skill`, `d.behavior`), invent
`d.plan`, and create uncloned fan-in with manifest declarations

**Problem:**

1. The §2.2 Mermaid diagram shows:
   - `PR -->|"d.skill"| CODE`
   - `PR -->|"d.behavior"| PLANS`
   - `PLANS["Plan & Handler Table\n(Qualified plan rank along d.plan, steps along d.step)"]`
1. Simultaneously, §2.2 item 2 states:
   - "Capacities, Skills, and Behaviors: Linked along `d.skill` and `d.behavior`" from `PR`. Linking
     `PR` along `+d.skill` to both `CODE` and manifest skills, and along `+d.behavior` to both
     `PLANS` and manifest behaviors, is an uncloned fan-in (violating degree $\le 1$).
1. In `runtime_abi.hpp:67–87`, `d.skill` and `d.behavior` are runtime agent dimensions
   (`agent -> instances`). Furthermore, `d.plan` does not exist in `runtime_abi.hpp` or
   `VortexCore`.
1. In `design/vagent-agent-language.md` §5.7, the canonical program layout is rooted at the Module
   cell:
   - `Module ─(+d.vars)─► Symbol` (module symbol table)
   - `Symbol ─(+d.clause)─► Plan/Handler` (plans for goals, handlers for events)
   - `Plan/Handler ─(+d.grab)─► Guard/Where`
   - `Plan/Handler ─(+d.spin)─► BytecodeGraph`
   - `Plan ─(+d.step)─► SuspensionTable`

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:167–170, 200–202, 214–216`
- `apps/common/xanadu/vagent/runtime_abi.hpp:67–87`
- `design/vagent-agent-language.md:616–623` (§5.7)

**Fix:** Align the diagram and §2.2 with §5.7: `PR` links along `d.vars` to symbol declarations (the
module symbol table). Goal symbols head ranks on `d.clause` of plans; plans link to bytecode on
`d.spin` and suspension steps on `d.step`. Remove `d.plan` and do not use `d.skill`/`d.behavior` for
bytecode/plans.

______________________________________________________________________

## MINOR

### 1. (§2.2, line 216) Suspension point linkage to resume opcode lacks explicit dimension name

**Problem:** §2.2 states: "and a suspension rank along `d.step` linking step ordinals ($0, 1, \dots,
K$) to their resume labels." It omits the dimension name connecting the step ordinal cell to the
resume opcode instruction cell.

**Fix:** Explicitly specify the topology: each suspension point cell $S_k$ on the plan's `+d.step`
rank links posward on `+d.point` to the target opcode cell ($S_k \xrightarrow{+d.\text{point}}
\text{resumeOpcodeCell}$), using `VortexCore`'s canonical `d.point` dimension.

______________________________________________________________________

### 2. (§2.1, line 150) Sealing should cite span globalization via publish

Full title: Sealing lifecycle in §2.1 and P17.2 should explicitly cite `xanadu::publish` span
globalization

**Problem:** §2.1 correctly notes that the sealed program store transcludes from the author's
deployment permascroll and cites `historyFromSeal`. However, Proposal P17.2 does not explicitly name
the `xanadu::publish` step that globalizes programmatic permascroll spans so remote swarm nodes can
mount `programRef` with full text resolution.

**Fix:** Mention in P17.2 that sealing the program store invokes `xanadu::publish` to globalize
primedia spans into the package descriptor.

______________________________________________________________________

VERDICT: NOT CONVERGED
