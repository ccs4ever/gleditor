# R3 final language and teaching review

Reviewed frozen guide snapshot `r3-input.txt`, SHA256
`6adf9a4e099fc1890f78b6d9080de7c7604b8eac53e257a2f1092c94b2eb8667`.

Verdict: CONVERGED — no actionable findings.

The final edits are accurate and improve the reader's model:

- Standalone `check` is correctly described as implemented, while the guide separately names the
  remaining module/template checking gaps and states that no user command executes agents. The
  internal scalar Vortex kernel remains clearly distinct from complete lowering and a source runner;
  this matches the progress record.
- The scanning example now says plainly that running it needs scanner lowering and runtime helpers,
  and that accepting its import provides neither. This avoids implying that source acceptance runs
  the imported operations.
- The workshop identifies `Warehouse` as a fixed test double whose question ignores `item` and
  returns its configured `units`, with no inventory decrement. That matches the code and bounds the
  example's claim.

I also rechecked the surrounding teaching sequence and unchanged source units against v3.9. The
success/Bool/Opt distinctions, scoped rollback trace, future-only event wait, delegation outcome,
belief-source provenance, and workshop branches remain consistent with the contract. Captions and
CLI guidance distinguish checked source from predicted runtime stories. No guide change is needed.
