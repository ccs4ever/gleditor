# Sixth increment native OpenGL journey: primary Luna baseline

The frozen baseline binary was `xuzz` SHA-256
`13e0efc05161179adac8ebc5f1f71c7f5d9279bf865cf6a8e957219cb0bc0a08`, with the matching frozen
`libgleditor.so.0` in `/workspace/spatial-build/sixth-gl-baseline-bin`. The checkout was clean at
`78f692e5cf1d5005381120459e6b09c63add6105` before the replay. Runs used native OpenGL at 800×600 in
fresh XDG profiles under the native-session container and Xvfb. Each process had an 80-second limit;
the normal and forced-page-break runs together stayed within the 180-second baseline bound.

| Step                                                                 | Visible control or input                                                                                                    | Outcome                                                                                                                                                                                                                                     | Evidence                                                                                                                       |
| -------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| Author an 80-line source and target                                  | `Ctrl+N`, type, `Ctrl+Return` in the forced run, target via `Ctrl+N`, pointer-selection automation, and default link chords | The source store contains two 3,719-byte inserts; the forced run has `kind=pagebreak at=3719`. The target is 23 bytes. See the source-length discrepancy below.                                                                             | `baseline-settled/{normal,forced}/argv.txt`, `source-after-*.txt`, `session.log`                                               |
| Select and inspect the link                                          | `Alt+Shift+N`, accessible link context                                                                                      | Link 21 is selected with both endsets. The source endpoint is displayed as bytes `0–7438`, and the target as `0–23`. The accessibility occurrence labels both say page 1/1.                                                                 | `baseline-captured/{normal,forced}/00-link-preview.png`, `session.log`                                                         |
| Enter the chosen endpoint                                            | Visible `Enter` button                                                                                                      | The click reaches the overlay button. The resulting live frame shows `Entry pending: building exact target page`; Doc 3 remains selected and focus reads `none`. The bounded process did not reach a settled post-entry accessibility dump. | `baseline-captured/{normal,forced}/01-after-enter-live.png`; `baseline-settled/{normal,forced}/session.log`, `exit-status.txt` |
| Continue through the source, return to origin, and use Activity Back | Ordinary wheel, `Alt+Shift+O`, `Alt+Shift+B`                                                                                | Not reached. The click/settled-capture sequence remains pending through the 80-second limit.                                                                                                                                                | `baseline-settled/{normal,forced}/exit-status.txt`                                                                             |

The visible preview is useful for identifying readiness. It reports the link occurrence positions as
page 1/1, while the frame captured during entry says the exact target page is still building. The
preview count therefore does not establish that the source document's pages or endpoint anchors are
ready. The existing `text.layout` debug output reports one page rebuilt for the source document, and
`xudu.links` reports one anchored strand; neither line records completion of the requested source
page entry.

There is a one-byte discrepancy in the authored source. The fixture file is 7,439 bytes, but the
shell command substitutions used to split it into two UI `--type` inputs remove the newline between
lines 40 and 41. The store therefore contains 7,438 text bytes: two 3,719-byte spans. The selection
command requests `[0,7439)` and logs that request; the link context reports `[0,7438)`. This replay
does not prove a 7,439-byte link endpoint. The forced page-break operation itself is at byte 3,719.

The separate `baseline-followup` run tried `Ctrl+Home` then `Ctrl+Shift+End`. Keymap debug confirms
`Ctrl+Home` is bound (also to `std:nav/hop_head`), but reports no binding for `Ctrl+Shift+End`. The
selection stayed empty and `Ctrl+Alt+L` logged that there were no links on screen. The successful
link replay uses the supported UI-selection automation gesture `--select`; no document content was
injected from a file.

In `baseline-settled`, both visited document operation dumps and the activity dump have matching
before/after hashes. The activity store has 12 operations in each profile. This confirms the bounded
pending-entry attempt added no visit or source-document operation. It does not validate
continuation, Origin, or Activity Back. Both modes ended with exit status 124 at the 80-second app
bound. That is the observed limit; it does not establish a deadlock or the reason the renderer did
not finish the entry/capture sequence.

`baseline-captured` contains the live frames that show pending status. `baseline-settled` contains
the pre-navigation and after-bound operation dumps. Raw PPMs remain in
`/workspace/spatial-build/sixth-gl-raw/`; each evidence directory has an `argv.txt`, session log,
lossless PNG frames where captured, and a SHA-256 manifest. The baseline follow-up images preserve
the unbound keyboard-selection attempt separately.

## Frozen candidate replay

The candidate was frozen at xuzz SHA-256
`404c21bc477c8dd5324bbba7957545f9de50ba52b0f96f38b94dfb3bd68a6cbd` and `libgleditor.so.0` SHA-256
`6964780c202365c8da5ada84fad51f0d21fc621a0c2db72ad0ebc792b75ffcf6`. The copied pair is under
`/workspace/spatial-build/sixth-gl-candidate-bin`. Root reports the build and owning tests passed;
these GL runs are native interaction checks, not a timing benchmark. Each used a fresh profile,
software OpenGL 4.5/Mesa, and a 1.5 UI font scale entered through the governed `Ctrl+Alt+Shift+F`
form with Backspace, `1.5`, and Enter. The resulting `system://ui` store in each profile contains
the typed `1.5` value (`ui-system-doc.txt`).

| Viewport and fixture | Authored source and link extent                                                      | Settled entry evidence                                                                                                                                       | Ordinary wheel continuation                                     |
| -------------------- | ------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------- |
| 800×600, normal      | 7,439 source bytes, no forced break; link left `[0,7439)`, target `[0,23)`           | `Doc 2` selected; left occurrence says pages 1–2/2; both anchors ready                                                                                       | Source remains highlighted, link 21 selected; P059–P070 visible |
| 800×600, forced      | 7,438 source bytes, Ctrl+Return at byte 3,719; link left `[0,7438)`, target `[0,23)` | `Untitled · a` selected; left occurrence says pages 1–2/2 and continues across breaks; debug log says the range crosses pages 0–1 and both anchors are ready | Source remains highlighted, link 21 selected; P059–P070 visible |
| 1440×1000, normal    | 7,439 source bytes, no forced break; link left `[0,7439)`, target `[0,23)`           | `Doc 2` selected; left occurrence says pages 1–2/2; both anchors ready                                                                                       | Source remains highlighted, link 21 selected; P047–P080 visible |
| 1440×1000, forced    | 7,438 source bytes, Ctrl+Return at byte 3,719; link left `[0,7438)`, target `[0,23)` | `Untitled · a` selected; left occurrence says pages 1–2/2 and continues across breaks; debug log says the range crosses pages 0–1 and both anchors are ready | Source remains highlighted, link 21 selected; P047–P080 visible |

The selected-tab names above come from the actual post-Enter accessibility trees. Each
`03-enter-selected.png` shows the source highlight and selected link panel. The
`04-continuation-wheel` frames show that an ordinary wheel scroll preserves the highlight and Link
21 while bringing the later passage into view; a `-40` tick probe overscrolled to an empty canvas,
so the matrix uses `-8` ticks. The forced entries therefore reach text after the byte-3,719 page
break. The normal source also lays out across two physical pages.

All four final candidate runs exited 0. The source-operation snapshots at the forge marker and after
the process are byte-identical. The marker can be buffered, though: in the 800×600 normal and both
1440×1000 runs, the snapshot named `before` already contains two `visit2` records, the same count
seen at Enter and after navigation. Those three snapshots do not establish the exact visit count
before Enter. The 800×600 forced run does: `activity-before` has one `visit2` record,
`activity-enter` has two, and `activity-after` still has two. Across all four runs, `activity-enter`
and `activity-after` preserve the same `visit2` records, parent operation ordinals, document
identities, and current ordinal, so Origin and Back add no further visit. The 1440×1000 normal run
invokes Back directly from the entered source: its post-Back accessibility tree selects `profile`,
then Origin selects `Doc 3`. In the other three runs, the recorded order was Origin then Back; the
final selected tab is `Doc 3`.

The calibrated runner is `/workspace/spatial-build/sixth-gl-scratch/run-native-journey.sh`. Final
matrix evidence is in `candidate-{800,1440}-{normal,forced}/`, with accessibility and debug logs,
source/activity snapshots, lossless PNG frames, actual argv, UI setting dump, and per-run hash
manifest. Raw PPMs remain outside Git in `/workspace/spatial-build/sixth-gl-raw/`. The
`candidate-font15-800-forced/` run is retained as a diagnostic of the overshooting `-40` wheel
setting; use `candidate-800-forced/` for the calibrated result.
