# Shared brief for reviewing `design/vagent-shared-program-and-live-upgrade.md`

You are one of six critics reviewing a design specification in the gleditor repository:

- **Document**:
  `/data/git/gleditor-restore/.claude/worktrees/bridge-cse_01JyeD4AMjEywzundTw7BvJ7/design/vagent-shared-program-and-live-upgrade.md`
- **Repo root**:
  `/data/git/gleditor-restore/.claude/worktrees/bridge-cse_01JyeD4AMjEywzundTw7BvJ7`
- **Workflow skill**:
  `/data/git/gleditor-restore/.claude/worktrees/bridge-cse_01JyeD4AMjEywzundTw7BvJ7/.claude/skills/vortex-design-critique/SKILL.md`

The specification proposes:

1. **U-A14 & §11.1 Sealed Versioned Program Manifold**: compiling a `vagent` program once per
   version into a frozen base `zigzag::Manifold` (`programManifold`, identified by Merkle digest
   `programRef` / `baseRef`) shared across all agents on a node and across parent/child hierarchies,
   so per-agent `ArenaManifold` overlays hold only mutable `$agent` state and shadow `OpHandle` /
   `-d.grab` slots.
2. **Eliminating `restoreState` Copying**: promoting only the ephemeral `$agent` state overlay on
   checkpoint and mounting it directly onto `programManifold` on resume, with symbolic plan names,
   `planGraphHash`, and suspension ordinals (`step`) on intentions.
3. **Live Agent Upgrades (`V_1 -> V_2`)**: quiescent turn-boundary upgrades under the same
   `ActorPid` with deterministic reconciliation of fields, beliefs, skills/behaviors, and active
   intentions, plus an `on Upgrade(from_version: Str)` lifecycle hook and numbered proposals
   `P17.1`–`P17.5`.

## House Style & Standing Rules

- Plans are argued, not asserted: cite exact code (`file:line`), price each ruling (bytes, cycles,
  links, messages, segments), and record refused alternatives.
- Prefer solving with more cells and dimensions over bit-packing, C++ side tables, or overloaded
  dimensions.
- Strict 2-degree ZigZag rank invariant: `A.pos(d) = B <=> B.neg(d) = A`; no uncloned fan-in on a
  single `(dim, direction)`.
- No sentinels in APIs (`std::optional` / `std::expected`). No arbitrary small ceilings that force
  workarounds.
- New C++ must justify why it is not Vortex.
- Any change to existing C++ engine/core/runtime code must be isolated in the Proposal Register
  (`P17.*`) with exact `file:line` scope, justification, price, and fallback.
- Markdown is formatted with `mdformat --wrap 100` and linted with `mdl -i`.

## Rules for Every Critic

- **Do NOT edit the target design document or any existing repository file.** You may only write
  your assigned critique file under
  `/data/git/gleditor-restore/.claude/worktrees/bridge-cse_01JyeD4AMjEywzundTw7BvJ7/design/projects/vagent-shared-program-review/`.
- Verify every claim against the code and sibling docs yourself (`view_file` / `run_command` with
  `grep`); cite `file:line` for evidence. Do not guess — if you could not verify something, say so.
- Write your full critique to your assigned output file as Markdown with numbered findings ordered
  by severity (`BLOCKING`, `MAJOR`, `MINOR`). Each finding must state:
  - Severity (`BLOCKING` / `MAJOR` / `MINOR`)
  - Location (`§X.Y`, line number)
  - Problem
  - Evidence (`file:line` from the repo and quotes from the doc)
  - Concrete Fix
- Stay in your lane: report findings outside your expertise only if `BLOCKING`.
- End your critique file with exactly one line: `VERDICT: CONVERGED` if no `BLOCKING` or `MAJOR`
  issue remains from your perspective, otherwise `VERDICT: NOT CONVERGED`.
- Your final reply to the orchestrator: at most 200 words listing finding counts by severity, the
  top three issues, and the verdict line.
