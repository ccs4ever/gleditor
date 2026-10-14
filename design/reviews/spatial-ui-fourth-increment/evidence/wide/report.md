# Native wide and multiline landing evidence

The safe-margin candidate was checked with a bounded native J24/J22 landing replay, not a
full-journey test. The UI-authored source selection is 654 bytes across eight lines; its distant
target is 23 bytes. Each run used a fresh profile and the sequence created both documents, marked
and linked their selections, started the link, clicked `Enter`, then exercised Origin, Back, and
fold/unfold.

At entry, the active-side marker is `Left` while the reading label is `right member 1`. The click on
the visible `Enter` button selects Doc 2 and changes the reading identity to `left member 1`. This
is a successful landing; the earlier harness concern confused the active-side marker with the
reading identity.

| Native size and backend | Visible selection result                                                                                                                                                                                   |
| ----------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 640×480 OpenGL          | The selection begins inside the safe area. Only its visible prefix fits: about two full lines, with the next line partly obscured by the link panel. The remaining offscreen text is not claimed readable. |
| 800×600 OpenGL          | All eight lines are visible, beginning at x=30; both “When…” lines and the final line are complete above the link panel and dock.                                                                          |
| 800×600 OpenGL ES       | All eight lines are visible with the same safe inset and full text.                                                                                                                                        |
| 800×600 Vulkan          | All eight lines are visible with the same safe inset and full text. Vulkan ran under X11/Xvfb.                                                                                                             |
| 1440×1000 OpenGL        | All eight lines and the full 23-byte target are visible together; both range endpoints and the link are in frame.                                                                                          |

The final run directories contain actual PNG frames from authored state through folded state,
session logs with accessibility-tree dumps, argv, fixture, profile metadata, activity, and source
snapshots before and after. Source and target store dumps compare byte-for-byte unchanged in each
run. Their SHA-256 values are respectively
`4791c441b9b88f3913fa6c76e17df5d96cee1f5b51b587235d494f97991514c8` and
`319740fd1d2ec050b0bd4a7938405fd2fbd4980d780a37e234b5050cfb2bcc19`. Raw PPMs and private XDG
profiles are outside Git under `/workspace/spatial-build/fourth-raw`;
[the manifest](raw-frame-and-profile-manifest.md) lists hashes and archive paths. See the
[800×600 OpenGL frame](final/800,600-opengl/01-enter.png),
[OpenGL ES frame](final/800,600-opengles/01-enter.png),
[Vulkan frame](final/800,600-vulkan/01-enter.png),
[640×480 frame](final/640,480-opengl/01-enter.png), and
[1440×1000 frame](final/1440,1000-opengl/01-enter.png).

The baseline at `c1ab2314e7bf31db67545aefd8a59f795a0d4756` is retained separately under
`baseline-full/`; its 640×480 and 800×600 frames had content clipped at the viewport edge and hidden
behind the dock/panel. The first post-fix, repaired, isolated, and pre-margin attempts are also
retained separately with findings; the pre-margin candidate fit eight lines vertically at 800×600
but clipped the leading W at x=0. The safe-margin result above confirms a 30px inset while retaining
the full 800×600 text block.

The `system://ui` document says `ui.fontScale` defaults to 1 and is editable, but UI inspection
found no direct font-scale control, so no enlarged-text native variant was reached. No diagnostic
CLI font override was used. The system document and affordance inspection are in
`font-setting-affordance/`. Native AT-SPI is unavailable in this environment; Xuzz
accessibility-tree dumps are retained in each session log.
