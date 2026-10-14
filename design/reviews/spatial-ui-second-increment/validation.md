# Second native checkpoint validation

This increment repairs the pre-existing Store Object Manager layout defect that hid controls named
in the accessibility tree. Nested widgets round their padding to pixel edges; fractional measured
padding could wrap Close Drawer or New Xanadoc into a row allocated zero height. The repair reserves
whole-pixel measured insets and floors the header label width. It keeps themes, settings, retained
birth identities, stale-action checks and creation semantics intact.

| Check                         | Result                           | Evidence / limit                                                                                                                                                                                                                                                                     |
| ----------------------------- | -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Default native build          | Pass                             | `make -j3`, same recorded O0 behavior builder. No optimized performance claim.                                                                                                                                                                                                       |
| Owning drawer suite           | 4/4                              | Existing fitted geometry sweep covers 48 size/font-scale/family combinations; retained drawing/actions, scroll/focus and stale accessibility actions also pass.                                                                                                                      |
| Full native UI suite          | 233/233                          | `evidence/second-ui-full.log`; previous 232/233 gate is resolved.                                                                                                                                                                                                                    |
| Named-control creation E2E    | 1/1                              | `evidence/second-drawer-e2e.log`; New Xanadoc, New Slice and Close Drawer all activate through visible named controls. Correct births and original text preservation are asserted.                                                                                                   |
| Actual new drawer routes      | Six launches pass                | UI authors source text; visible Store Object Manager opens; New Xanadoc/New Slice create; Close Drawer closes; visible control reopens. Narrow 640 and normal 1440 windows, independently profiled on OpenGL/OpenGLES/Vulkan.                                                        |
| Independent UX and aesthetics | Bounded accepted                 | `independent-initial.md` and `independent-final.md`; actual final PNGs inspected. Enlarged text/dense state and keyboard-only creation/AT remain open.                                                                                                                               |
| Baseline classification       | Four original failures reproduce | `e2e-baseline-classification.md` has pinned/original-increment logs, frames and hashes. Whole-page frames are pixel-identical. Original drag destinations are stale test assumptions; no test threshold was weakened. Drawer creation is now fixed; the other three checks stay red. |
| Native J24/J25 follow-up      | Scoped logical transitions       | `evidence/navigation/report.md`: two-document Link, Enter/Origin/Back and two sibling visits retained. Left landing clips the passage beginning; readable landing, historical/cell/unavailable/cancel/reopen variants stay open. No same-profile pre-navigation source snapshot.     |

The first checkpoint's 100 owning checks, 99/99 Xuzz suite, bounded source/visit isolation evidence
and renderer core checks remain recorded. This small drawer change does not imply full J1–J16
preservation, full spatial journey acceptance, physical workspace milestones, optimized performance
or assistive-technology parity. Continue from the project status file's exact next task.
