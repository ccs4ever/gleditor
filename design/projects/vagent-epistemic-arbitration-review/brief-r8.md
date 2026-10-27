# Round 8: critique against the integrated actor runtime

Repository: `/data/git/gleditor`, branch `design/vagent-epistemic-arbitration`. Runtime baseline:
`64b05125` (integration of `46402f26`). Input:
`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md`, v1.4. Immutable snapshot:
`r8-input-v1.4.txt`; SHA-256 `c3d1750831376784f75d91799b431964f7b87331f26f9256d1b9f95bca659924`.

The owner requests another full critique, verifying P19 against the newly integrated upgrade and
belief logic. All P19.1–P19.8 are already approved. Approval is not implementation or evidence of
conformance. Follow `.claude/skills/vortex-design-critique/SKILL.md`. Ten roles review in waves
within the concurrency limit, all on Luna as requested in the original review.

## Ground truth and focus

- The actor frontend lives under `apps/common/xanadu/vagent/runtime/`, namespace
  `xanadu::vagent::runtime`; it has a distinct AST/parser/checker/compiler from the preserved
  bounded frontend under `vagent/`. `check` uses the latter, `check-runtime`/`run` the former.
- Read `runtime/abi.*`, `compiler.*`, `agent_executor.*`, `program.*`, `reconciler.*` if present;
  locate actual names with `rg --files` before assuming a file exists. Inspect
  `tests/xuzz/vagent_{belief,upgrade,resume,frames,types,effects,core_changes}_test.cpp` and
  `mesh_actor_vagent_test.cpp`. The integrated runtime tests pass; this does not implement P19.
- Cross-check `vortex/mesh_actor.*`, `vortex/vortex_vm.*`, `zigzag/arena_manifold.*`,
  `zigzag/vlog.*`. Verify shared base versus overlay, migration ordering/identity/ref remapping,
  state reconciler field allowlists, active intentions/effects/timers/tokens, program/model/schema
  epochs, rollback and actor delivery acknowledgement, predicate keys, source coexistence,
  rule/ground fact mixing, verification and arbitration purity. Distinguish supported behavior from
  prerequisites.
- P17 companion: `design/vagent-shared-program-and-live-upgrade.md`. The historical P18 document can
  be read with
  `git show f033eda8:design/vagent-extended-capabilities-and-belief-vlog-unification.md`; it is
  absent from this branch. Do not assume absence of that document proves absence of all belief
  capabilities: inspect actual runtime and tests. Do not import or implement P18/P19 here.
- Review numerical/epistemic/hardware semantics where lifecycle changes create stale labels, replay,
  correlation, resets, clock rebasing, or model-feature misbinding. New literature is only needed
  for a specific factual uncertainty; use primary sources for any external research.

## Required report

Read-only critique: do not edit the design, source, tests or other reports. Write only your assigned
`r8-<role>.md` with precise source file:line evidence. Use the skill's report format, tally
BLOCKING, MAJOR, MINOR, previous R7 findings if relevant, concrete fixes and final
`VERDICT: CONVERGED` or `VERDICT: NOT CONVERGED`. Focus on real blockers/majors, at most five minor
bullets. Do not elevate stylistic polish. Verify any claim about current behavior in code/tests.
Distinguish newly required engine changes, fixes already covered by approved P19, and doc-only
baseline corrections. Paths in reports are repo-relative. Preserve earlier reports verbatim.

The lead verifies every substantial finding, writes the disposition, updates the specification and
baseline citations, then requests focused follow-ups on the revised candidate until convergence. No
engine implementation changes are authorized by this critique task.
