# Shared brief — vlace-lens-bridge review round 1

- Target doc: `design/vlace-lens-bridge.md` (repo-relative)
- Repo root: `/data/git/gleditor-restore/.worktrees/sql-to-zigzag` (branch
  `design/sql-to-zigzag`)
- Sibling context: `design/anything-to-zigzag-lens.md`,
  `design/vlace-functional-language.md`,
  `design/ux_workflow_lenses.md`, `design/audit-2026-10-23-ux-lenses.md`
- Key sources: `apps/common/xanadu/vlace/{compiler,types,driver}.cpp`,
  `apps/common/xanadu/zigzag/{lens_repl,projection_session,projection_lens,
  lens_uri,router_lens,filesystem_lens,meta_lens,arena_manifold}.*`

## House rules every critic enforces

1. Argued, not asserted: cite exact `file:line`, price costs, record refused
   alternatives.
2. Cells and dimensions over side tables; no foreign serialization inside
   cells.
3. `std::optional`/`std::expected`, never sentinels.
4. No arbitrary ceilings; priced/documented bounds only.
5. Vortex over C++ where the language can express it.
6. Proposal Register `P<k>` required for any change to existing
   engine/core/stdlib files.

## Output format each critic must return

```markdown
# Round 1 critique: <Role> (vlace-lens-bridge.md)
Tally: <B> BLOCKING, <M> MAJOR, <m> MINOR.

## BLOCKING
### 1. (§X.Y, line L) <title>
**Problem:** ...
**Evidence:** doc quote + repo file:line.
**Fix:** concrete replacement.
## MAJOR
...
## MINOR
...
VERDICT: CONVERGED | NOT CONVERGED
```

- BLOCKING = cannot work / violates a hard invariant / contradicts real code
  without a `P<k>` proposal.
- MAJOR = implementer builds wrong thing or hits unspecified failure; layer
  boundary or C++-vs-Vortex rule violated; proposal lacks justification.
- MINOR = polish.
Critics are read-only: return the critique as your final message text; do not
edit files.
