# Round 9 disposition: all ten roles converge on v1.5

The focused follow-up reviewed the immutable [v1.5 candidate](r9-input-v1.5.txt), SHA-256
`229e8434f752c9e2fa9e1394f6e79549d005279f180f7356325588b63ec90147`, against runtime baseline
`64b05125`. The current proposal is byte-identical to that candidate. All ten Luna roles return
CONVERGED with zero BLOCKING and zero MAJOR findings. P19.1–P19.8 remain approved.

| Role                            | Closing report                            | BLOCKING | MAJOR |
| ------------------------------- | ----------------------------------------- | -------- | ----- |
| Technical writer                | [R9 writer](r9-writer.md)                 | 0        | 0     |
| Vortex and Spool-Mesh           | [R9 vortex](r9-vortex.md)                 | 0        | 0     |
| Codebase                        | [R9 codebase](r9-codebase.md)             | 0        | 0     |
| System architect                | [R9 architect](r9-architect.md)           | 0        | 0     |
| PL design and ergonomics        | [R9 ergo](r9-ergo.md)                     | 0        | 0     |
| Xanadu/ZigZag                   | [R9 zigzag](r9-zigzag.md)                 | 0        | 0     |
| Machine learning                | [R9 ml-expert](r9-ml-expert.md)           | 0        | 0     |
| Mathematics and math formatting | [R9 mathematician](r9-mathematician.md)   | 0        | 0     |
| Epistemology                    | [R9 epistemologist](r9-epistemologist.md) | 0        | 0     |
| Hardware faults                 | [R9 hardware-fault](r9-hardware-fault.md) | 0        | 0     |

## Closed findings

[Round 8 disposition](r8-disposition.md) records the accepted findings and their source
verification. Round 9 verifies the resulting contracts rather than treating current runtime tests as
P19 tests:

- Ground facts already coalesce by full tuple with multiple source-name supporters. P19 preserves
  this behavior while assigning independent observation histories, source identity, time/variance,
  feature and label ownership. Exact retries do not refresh data, and historical corrections do not
  revive superseded readings.
- Generic host upgrade and checkpoint restoration are distinct. The proposed translator requires
  full manifest/epoch compatibility, complete owned reference mapping, clone/rank reconstruction,
  replay preservation, fault latches and clock uncertainty/rebasing. The full automatic vagent
  upgrade pipeline remains a prerequisite; the generic callback is not that pipeline.
- Transport receipt is separate from committed verification. Intake/processing turns and bounded
  on-manifold journals/cached outcomes supply the proposed application-level replay boundary.
- The actor frontend and its actual AST/checker/compiler own P19 syntax. `check-runtime` and `run`
  are its future conformance paths; the bounded frontend is preserved.
- The scoped graph ABI now defines reference operands, evaluation frames, completion, choice floors,
  typed outcomes/refusals and atomic cleanup/publication. Numeric model evaluation remains Vortex.

No new BLOCKING, MAJOR or MINOR finding remains. Numeric/scoring/fusion formulas are unchanged: the
lead verified all seven fenced math blocks are byte-identical to the Round 8 input.

## Validation and scope

The proposal, briefs, dispositions, index and twenty new critic reports pass mdformat with GFM,
dollarmath and frontmatter, mdl, relative-link checks and whitespace checks. Snapshot hashes and all
ten closing verdicts are checked mechanically; see [validation](validation-r8-r9.txt) and
[snapshot identity](r9-input-sha256.txt). Earlier reports and approval remain historical records.

Only design/review documentation changes in this round. No engine code or on-disk format changes,
new P19 approval request, or claim of running P19 acceptance tests is made. The acceptance gates in
§8.2 remain required when implementation begins.
