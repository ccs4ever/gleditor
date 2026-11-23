# Vortex I/O frontend examples review

This review covers the companion source-surface examples, not implementation code. R1 reviewed
SHA-256 `2b90cb6a675e12d1ace92a4b222744bc44fa3ce9e4541f8146693c0563034d93`; R2 target
`ce9b9815f379d706588813ea6f4d17b2fc383b6d0408deeae6190eb2f3f4e594` had six frontend and the Vortex
acceptances. Its VAgent report requested a source-shape correction. R3 target SHA-256
`043f09854ec8f11b62aaa2e5a43e9d8634e277eb708fd4f1cc7e07ffde466c30` received Vortex acceptance and
VAgent approval with a minor finding. The post-correction focused confirmation target is SHA-256
`de8b6e72e4ca0fd7fd69215a4fd8c596e0868063c268dee34f4b47086fbeef56`. The authoritative I/O plan was
accepted at SHA-256 `86b68b2aab972fbe0cee7c5b81f20ab6da219cc3fbc0b79d86d7905b47c273ed` before this
companion was linked. The link-only revision is SHA-256
`9226c1c887c993da9f7f2d4845cd3ecca20082b8c5f63cde68719e5308ff8a0e`; this later hash is not
represented as the accepted plan hash. The plan review tracker has its own record of prior plan
reviews.

The review asks one frontend expert for each of VQL, VPL, VProlog/Vlog, VLace, VIcon, and VAgent,
plus a Vortex execution/continuation expert, to check their scoped example against this exact
example hash and the accepted plan contract. A reviewer should report source evidence, concrete
omissions or invented API, and a verdict. The author records findings and dispositions below. The
final current example hash is the R4 target above; carried reviews are labeled with the snapshot
they actually checked.

## Review status

| Scope                           | Reviewer report                                                                | Verdict                                               |
| ------------------------------- | ------------------------------------------------------------------------------ | ----------------------------------------------------- |
| VQL                             | [R1](r1-vql.md), [R2](r2-vql.md)                                               | CONVERGED at R2                                       |
| VPL                             | [R1](r1-vpl.md), [R2](r2-vpl.md)                                               | CONVERGED at R2                                       |
| VProlog/Vlog                    | [R1](r1-vprolog.md), [R2](r2-vprolog.md)                                       | CONVERGED at R2                                       |
| VLace                           | [R1](r1-vlace.md), [R2](r2-vlace.md)                                           | CONVERGED at R2                                       |
| VIcon                           | [R1](r1-vicon.md), [R2](r2-vicon.md)                                           | CONVERGED at R2                                       |
| VAgent                          | [R1](r1-vagent.md), [R2](r2-vagent.md), [R3](r3-vagent.md), [R4](r4-vagent.md) | CONVERGED at R4 on final target                       |
| Vortex execution / continuation | [R1](r1-vortex.md), [R2](r2-vortex.md), [R3](r3-vortex.md)                     | Accepted at R3 hash `043f…`; no later semantic change |
| Formatting, lint, and links     | Final docs check                                                               | PASS                                                  |

The latest role matrix covers all seven scopes. VQL, VPL, VProlog/Vlog, VLace, and VIcon converged
at R2 on `ce9b9815f379d706588813ea6f4d17b2fc383b6d0408deeae6190eb2f3f4e594`; their reviewed domains
were unchanged by the focused VAgent revisions. Vortex accepted the R3 snapshot
`043f09854ec8f11b62aaa2e5a43e9d8634e277eb708fd4f1cc7e07ffde466c30`; the later edits only add the
VAgent goal marker and clarify its Unit binding. VAgent R4 and the current companion both use
`de8b6e72e4ca0fd7fd69215a4fd8c596e0868063c268dee34f4b47086fbeef56`.
