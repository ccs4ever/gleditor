# Vortex I/O redesign critique brief

## Target and scope

- Repository:
  `/home/ccs4ever/.codex/worktrees/eced7532-310c-4b04-8ad9-6e2ab242f574/gleditor-restore`
- Target: `design/vortex-io-redesign.md`
- Workflow: canonical `vortex-design-critique` skill at
  `/data/git/gleditor-restore/.claude/worktrees/bridge-cse_01JyeD4AMjEywzundTw7BvJ7/.claude/skills/vortex-design-critique/SKILL.md`
  (SHA256 `fbbb3c5dd7d0316cf823e29c9c4aaec5df1ec8cbdbd813b0dc0efa7e73d274a6`). The skill source is
  external to this current worktree; read it as workflow only. All code and design claims must be
  checked against the current worktree.
- User model instruction: use GPT-6 Luna at high effort. This explicit instruction overrides the
  skill's recommendation to use cheaper critic models.
- Scheduling adaptation: six distinct read-only critics, in two waves of three, because only four
  global concurrency slots are available and the lead editor occupies one. Collect all six round-one
  reports before editing the target. Critics write only their assigned report files.
- This round-one review uses the stable target snapshot; the lead records its SHA256 before
  dispatch. Preserve pre-existing dirty runtime/compiler/test files; do not touch them.

## User decisions that constrain proposals

- All runtime/lens I/O is asynchronous and off the evaluation and UI threads.
- The program interface is cell-only.
- Input is transient unless explicitly captured as permanent.
- Memory is bounded and backpressure is explicit; initial disk spill is out of scope, and overflow
  is explicit when the producer cannot pause.
- Explicit stream/network submission freezes or pins its output. A new execution creates a new
  operation, including after backtracking.
- Automatic resume continues the same request and never resubmits it.
- Preserve Evidence grants, durable attempts, and no-silent-retry behavior.
- Preserve VAgent's post-commit policy.
- Keep the Vortex VM/core language-neutral. Frontend needs inform generic capabilities; syntax,
  typing, purity/effect placement, multiplicity, and frontend-local continuation behavior stay in
  compilers/adapters. Current VQL-specific VM coupling is baseline debt; cleaning it is a separate
  project and not an I/O enablement gate.
- Mechanisms remain open to critique, including native side tables; the user's decisions above are
  constraints, not a predetermined implementation.

## Relevant sources and documents

Read the target and follow its citations. Likely relevant paths include:

- `apps/common/xanadu/vortex/` — core, VM, host, stdlib, I/O and related runtime implementation.
- `apps/common/xanadu/zigzag/` — manifold, arena manifold, Vlog and cell views.
- `apps/common/xanadu/vql/`, `vpl/`, `vprolog/`, `vicon/`, `vagent/` — language implementation and
  call sites.
- `apps/common/xanadu/identity/` — Evidence grants and identity-related runtime paths.
- `design/vortex-hyperstructural-runtime.md`, `design/vlog-logic-extension.md`,
  `design/vortex-distribution.md`.
- `design/vql-agent-experiment.md`, `design/vicon-goal-directed-language.md`,
  `design/vlace-functional-language.md`, `design/vagent-agent-language.md`.
- `design/store-slice-convergence.md` (especially R8/R11/R12), `AGENTS.md`, `GEMINI.md`, and
  `.claude/skills/system-architect/SKILL.md`.

Use exact current-worktree file and line citations for repository evidence. Do not treat the
canonical workflow skill's source worktree as code evidence.

## House rules

Follow the canonical skill's six personas and severity definitions. In particular:

- Argue mechanisms with exact `file:line` evidence; price bytes, cycles, links, messages, or
  segments where relevant; record technically refused alternatives.
- Treat cells and dimensions as the default representation for logical structure; scrutinize ad-hoc
  C++ side tables, serialized structures in scalar text, and bit packing. A side table is not ruled
  out without evaluating its role and cost.
- Use `std::optional`/`std::expected` for absent and fallible API values; do not introduce
  sentinels.
- Avoid arbitrary ceilings. Price resource bounds and make them documented settings or grant limits
  where required.
- Justify new C++ against Vortex or Vortex standard-library alternatives.
- Isolate changes to existing C++ engine/core/stdlib/runtime files in numbered Proposal Register
  entries with exact files, justification, cost, and fallback.
- Preserve the user's decisions above, including asynchronous off-thread I/O, cell-only program
  interface, explicit permanent capture, bounded memory/backpressure and overflow, frozen/pinned
  explicit submissions, same-request auto-resume, durable Evidence attempts/no silent retry, and
  VAgent post-commit behavior.

## Report format

Each critic writes only `r1-<role>.md` in this directory, using the canonical skill's format:

- Heading: `# Round 1 critique: <Role Name> (vortex-io-redesign.md)`
- Paths relative to repository root.
- Severity tally and separate `BLOCKING`, `MAJOR`, and `MINOR` sections.
- Each finding includes a precise problem, exact document quotation plus repository `file:line`
  evidence, and a concrete fix.
- End with `VERDICT: CONVERGED | NOT CONVERGED`.

Personas: `writer`, `vortex`, `codebase`, `architect`, `ergo`, `zigzag`. Be specific; do not report
speculative defects without evidence. Critics are read-only and must not edit the target or shared
review files.
