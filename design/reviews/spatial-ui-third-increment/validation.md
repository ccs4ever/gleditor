# Third native checkpoint validation

The prior landing centered a readable-distance camera on two complete pages, leaving the focused
passage outside the viewport. It also placed the caret without activating its document tab. The
repair uses ordinary document activation for exact-range focus, cancels page-zero framing, and
reuses existing caret-follow framing after companion placement. The beginning takes priority if a
range is larger than the viewport. This is a bounded native repair, not full spatial framing.

| Check                                         | Result                     | Evidence / limit                                                                                                                                                                                                                                                    |
| --------------------------------------------- | -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Native default build                          | Pass                       | Recorded O0 behavior builder and initialized submodules; no optimized performance claim.                                                                                                                                                                            |
| New actual-interface regression               | Pass on all three backends | `linkTravelActivatesExactDocumentAndKeepsSiblingVisits` UI-authors both documents and the Link. Awaited selected tabs match Enter, Origin and Back; exact selections and root/sibling visit records are asserted.                                                   |
| Full native UI suite                          | 233/233                    | `evidence/third-ui-full.log`.                                                                                                                                                                                                                                       |
| Full Xuzz suite                               | 99/99                      | `evidence/third-xuzz-full.log`.                                                                                                                                                                                                                                     |
| Native visual review                          | Bounded accepted           | Independent reviewer inspected nine finalized Enter/Origin/Back frames across OpenGL/OpenGL ES/Vulkan. Both short passages are legible when focused; selected tabs match focus.                                                                                     |
| Independent native source/activity validation | Bounded Pass               | `evidence/landing/report.md` has actual UI authoring, matched readable focus/tabs, byte-identical same-profile source dumps before navigation and after Back at 800×600, root plus exactly two sibling visits and current restored to 1. Origin/Back add no visits. |
| Wider framing / accessibility                 | Open                       | No acceptance for long or multi-line ranges, narrow/enlarged text, reduced motion, AT, or closed/unavailable/historical/cell target variants.                                                                                                                       |

The previous broader E2E baseline classification, remaining three failed checks, O0 performance
failures and network/AT environment limits remain in earlier validation records. A passing new
regression does not make the full preservation or cutover gate green. Continue from the project
status file, using exact historical identities and the same native command boundary.
