# R1 guide revision disposition

Input: `r1-input.txt`, SHA-256 `deaea7a31d2c70e0dfdcc8221f36c0973c874f8c04208fd5741c0ff311232ae2`.

The technical writer, language specialist, Vortex expert, codebase expert and learner reviewed the
same frozen guide. Language and Vortex reviews converged in R1; teaching and implementation-status
findings required revision.

| Finding                                                | Revision in R2                                                                                                                                                                                                                              |
| ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Writer W1: runtime jargon before a concrete lesson     | Replaced the early arena/outbox/adapter account with plain turn and segment definitions; distinguished saved plan locals from handler locals.                                                                                               |
| Writer W2; learner L1/L4: what can actually be checked | Labeled all 24 complete units, supplied filenames, expected output and exit codes, and explicitly marked runtime stories as predictions with no current delivery/stepping command.                                                          |
| Writer W3: foundational density                        | Put events and conditions before advanced search, added a small edit/rejection checkpoint, and moved optional intention ids after adoption.                                                                                                 |
| Writer W4; learner workshop ambiguity                  | Added a delegation-only unit before the optional synthesis project. Replaced “total if” wording with the explicit stock comparison. Removed the misleading rolled-back Shortage emission and traced both sufficient and insufficient stock. |
| Writer W5: missing wayfinding                          | Linked composition, scheduling, modules, persistence and diagnostic/reference chapters in the reading path.                                                                                                                                 |
| Writer W6: abstract fact sources                       | Added an A/B assertion-withdrawal trace showing why another source keeps a fact alive.                                                                                                                                                      |
| Learner L2: nested rollback is hard to retain          | Added a complete plan with a search arm failure and later enclosing block failure; a step table follows budget 6, 1, 6, 3, 9, then 6.                                                                                                       |
| Learner L3: first wait is too elaborate                | Added one goal waiting for one future event, with selected turns at 0/1/2 seconds and a deadline-edit exercise.                                                                                                                             |
| Codebase C1: omitted compiler milestone                | Added the verified internal scalar compiler kernel, expressly without a source CLI or complete agent compilation.                                                                                                                           |
| Codebase C2: implied implemented map API               | Described Map as specified and dependent on the future linked prelude/runtime; removed the nonexistent API instruction.                                                                                                                     |

Validation before R2: all 24 complete units passed the existing guide parse/check test via
`make -j$(nproc) test/vagent-frontend TEST_FILTER='VagentGuideTest.*'`. Markdown formatting and lint
passed. These checks establish frontend acceptance, not the runtime traces.

R2 input: `r2-input.txt`, SHA-256
`b2fa52e2874a657d1ce33c3e9031e05ce59e9991da0fc4ba6b84c6f1f638ed6a`.
