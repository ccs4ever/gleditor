# Round 4 disposition

Input was the v1.3 scratch draft subsequently integrated as the design specification. The ten roles
reviewed the explicit changes in [Round 3 disposition](r3-disposition.md), without requiring
features explicitly deferred outside the initial scope.

## Follow-up changes

| Finding                                                           | Ruling   | Final change                                                                                                                                                                           |
| ----------------------------------------------------------------- | -------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Vortex: callback failure could consume enclosing choices          | Accepted | P19.8 enforces the saved choice-stack floor, root sentinel and child-mark-before-scope-mark cleanup                                                                                    |
| Ergonomics: LogicVar not defined                                  | Accepted | §4 defines LogicVar as question mark plus identifier                                                                                                                                   |
| Core review: scope helper has no callable VM path                 | Accepted | P19.8 names OpcodeKind::ScopedEvaluate, its input/output wings, same-cursor interpreter frame and typed callback ABI; register includes enum/core/artifact impacts                     |
| Core review: d.elts was an unexplained list ABI                   | Accepted | Owned list head uses d.grab, with d.step elements                                                                                                                                      |
| Hardware minor: authorized reset/verifier not tied to host policy | Accepted | §4 defines separate host-issued verification and health grants, checked by the future adapter with deny-default behavior                                                               |
| ML minor 1: unregularized logistic intercept                      | Accepted | §3.3 regularizes bias as well as weights, with finite learning-rate/regularization constraints                                                                                         |
| ML minor 2: verification weight omitted from SGD                  | Accepted | Both data-loss gradients carry q; regularization remains separate                                                                                                                      |
| Epistemology major: self-issued verifier authority still possible | Accepted | Only a separately authenticated verification percept with a host-issued scoped grant enters a label-capable handler; ordinary plans request adjudication and cannot certify themselves |
| Epistemology minor: ranked near ties inconsistent                 | Accepted | Ranked uses the same tolerance, grouping around each remaining maximum without transitive chaining                                                                                     |

The additional authority fields describe an explicit integration boundary, not a complete new
identity framework. Host/operator adjudication remains fallible; grant validation establishes who
may label which observations, not whether the label is objectively true.

The math and hardware critics closed all previous major findings in Round 4. ML closed its majors
but reported two minors; epistemology requested the authority correction above. Those two roles
reviewed the final changes again in Round 5. Other closing reports are linked in the README.

Markdown checks apply to the final design and every new/modified review Markdown record using gfm,
dollarmath and frontmatter extensions. Historical R1/R2 reports and the exact baseline are retained
as evidence rather than rewritten. This is a documentation-only task; no engine build or runtime
behavior is claimed tested.
