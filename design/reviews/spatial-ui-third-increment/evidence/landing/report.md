# Native readable landing validation

Build under review: `/workspace/gleditor` HEAD `85a9f8aba18b92504e69a9de68607454e3ad5962`, the
third-checkpoint working build after the landing fix. This is a bounded native J24/J25 follow-up,
not a complete journey audit. I authored two xanadocs and their Link through Xuzz in one fresh
profile and one running session. The left endpoint is `Origin exact passage.` bytes `[0,21)`; the
right endpoint is `Distant target exact passage.` bytes `[0,29)`. The Link has one member on each
side and one occurrence each.

The current native session ran OpenGL at 800×600 with the default font. Authoring and link selection
used the recorded default chords; Enter, Origin and Back used the visible, named link-context
controls through the runner's pointer path. The recorded order was Enter left endpoint, Origin,
Enter left endpoint again from the root checkpoint, then Back. The second entry is a sibling of the
first.

| Step                           | Affordance                                                                                                                 | Observed result                                                                                                                                                                                               | Evidence                                                                            |
| ------------------------------ | -------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| Author both documents and Link | `Ctrl+N`, type/select, `Ctrl+Alt+[`, `Ctrl+N`, type/select, `Ctrl+Alt+]`, `Ctrl+Alt+L`; select the Link with `Alt+Shift+N` | Pass. Xuzz reports the exact selected ranges and the Link panel shows both members.                                                                                                                           | `after/00-authored.png`, `after/session.log`                                        |
| Enter the left endpoint        | Visible **Enter** control                                                                                                  | Pass. The entire “Origin exact passage.” selection is readable in the frame. The accessibility dump shows the first authored document textarea with caret 21 from 0, `Doc 2` selected, and `focus: document`. | `after/01-enter.png`, `after/session.log`                                           |
| Return with Origin             | Visible **Origin** control                                                                                                 | Pass. Reading returns to the right member in Doc 3; the exact right range remains available in the link context.                                                                                              | `after/02-origin.png`, `after/session.log`                                          |
| Enter again, then Back         | Visible **Enter**, then visible **Back**                                                                                   | Pass for two sibling visits and root restoration. The second Enter again selects Doc 2 and its full passage. Back selects the default tab and leaves `focus: default`.                                        | `after/03-enter-sibling.png`, `after/04-back-root.png`, `after/session.log`         |
| Source and activity invariants | Same-profile `xudu-dump --section=ops` before navigation and after Back                                                    | Pass. Both source dumps are byte-for-byte identical. Activity has root visit 1, two child records with IDs 2 and 3 and parent 1, and its final current pointer returns to 1.                                  | `after/source-before-*.txt`, `after/source-after-*.txt`, `after/activity-after.txt` |

The two source operation hashes are unchanged across the same-profile before/after dumps: origin
`0966c3dce31f7f5a3a161d1a8033eb2fee61716d512e43c709713787b7e1ccc9`; target
`07fda1ea881a3747c68498095ba0adc5c6b09d7af401bd1f88da6f19d81cc960`. They also match the
deterministic UI-authored control fixture recorded in the previous navigation report. The activity
dump has separate visit records at operations 13 and 19; their visit IDs are 2 and 3, and each
parent field is 1. The last `d.activity-current` edge points to value 1 after Back. No extra visit
was appended by Origin or Back.

For comparison, `before/01-enter-original-4f.png` is the clipped landing from the previous native
checkpoint's frozen original build (recorded there as `4f2d3fac6fc4e5aa59d11618ae25dd647f6151c8`).
That reference run used a 1440×1000 window and the older drawer state, so it is not a same-size
visual comparison. The post-fix frame is the fresh 800×600 capture; it shows the full selected
passage and matching selected tab.

The native tree prints `accessibility: not in this build`; its text dump is useful evidence of
names, selection, caret and focus but does not establish an AT-SPI or assistive-technology session.
This run did not exercise unavailable/cancel targets, historical or cell endpoints, repeated
occurrences, application restart, enlarged text, narrow-layout variants, or other rendering
backends. J24/J25 remain only partially validated.
