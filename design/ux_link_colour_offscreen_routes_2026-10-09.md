# Link colour ownership and offscreen routes

Implemented in native xuzz on 9 October 2026. This addresses the mixed highlight hues and the
missing quieter beam beside a selected link. The deferred shallower-angle arrangement remains
unwired.

## Behaviour

An entered endpoint's editing selection uses its Link hue. Ordinary editing selections keep their
usual blue treatment. Ownership records the exact selection revision, view, version, range,
selected-Link generation and session generation. Cross preserves ownership; editing, selecting
another Link, Dismiss/reselect and replacing a version invalidate it. Returning manually to the same
byte range does not revive ownership.

Semantic fill bands carry an opaque runtime layer identity for authority plus Link, independent of
RGB. Navigation and semantic coverage from the same owner contribute one band. Different owners
remain separate even when they have equal colours. The selected contour composites against the paper
rather than another Link's fill. Legacy generic edge decoration keeps its previous behaviour. True
transclusion remains separate from explicit Link colouring.

A document Link with one visible endpoint retains a full NURBS route to the exact first remote page
fragment. The viewport clips that route; it creates no invented endpoint, gap coverage or terminal
cap. Current page geometry supplies the remote attachment without reshaping text.

Document pairs use stable lanes ordered by cached owner identity. Three quadratic NURBS spans give
horizontal attachments, tangent-continuous gutter joins and line-height-bounded end turns. The same
routing rule applies with both endpoints visible, so merely crossing the viewport boundary does not
swap the route. Equal-height attachments retain the ordinary horizontal curve. Selection changes
brightness and depth without reordering lanes.

Preferred connector weight remains `beams.connectorWidthOfLine`, default 0.40 line height. Gutter
fitting may reduce it to the live `beams.gutterMinimumWidthOfLine`, default 0.30 and bounded by the
preferred width. Capacity includes the existing kerf. Overflow shares lanes without discarding
relations; four margin slots do not promise four distinguishable full-width gutter beams.

A visible unresolved companion is also protected during panel placement. This geometric protection
chooses no navigation occurrence and records no visit.

## Native interaction evidence

All runs use isolated configuration, data and cache directories and software headless rendering. The
prepared KJV fixture has Link20's sparse Genesis/Psalm/Revelation endset and Link42's continuous
Psalm62:12–63:3 range. Both address the same notes range. Preparation is not a UI authoring pass.

| Step                                        | Affordance                                               | Result                                                                              | Evidence                                                          |
| ------------------------------------------- | -------------------------------------------------------- | ----------------------------------------------------------------------------------- | ----------------------------------------------------------------- |
| Enter Link20's Genesis occurrence           | Default link/member/place/Cross/Enter chords             | Link-colour selection and contour; separate cyan20 and quieter blue42 gutter routes | `build/link-colour-route-{opengl,opengles,vulkan}-05/left.png`    |
| Change active side and return               | `Alt+Shift+X`                                            | Hue ownership remains stable; anchor ends extend/retract                            | `right.png`, `left-return.png`, start/mid captures                |
| Edit selection, then restore the same bytes | `Shift+Left`, `Shift+Right`                              | Ordinary blue selection; navigation ownership does not revive                       | `edited.png`, `edited-exact-range.png`                            |
| Dismiss                                     | `Alt+Shift+D`                                            | Both routes remain visible                                                          | `dismiss.png`                                                     |
| Pick the exposed remote route               | Pointer `(804,650)`                                      | Beam tag kind4, strand7; selects Link42 while Genesis remains in view               | `run.log`, `picked-remote.png`                                    |
| Enter the remote occurrence                 | `Alt+Shift+Return`                                       | Focuses exactly `[2302771,2303273)` on KJV pages622–623                             | `run.log`, `entered-remote.png`, accessibility dump               |
| Four overlapping owners                     | Same native navigation over a prepared four-Link fixture | Four discrete colour bands and readable glyph ink                                   | `build/link-colour-route-opengl-four-04/{left,right,dismiss}.png` |

Each three-backend pointer run has only the two explicit Enter focus calls: Genesis and then the
chosen Psalm range. Source `ops.nodes` and `store.tables` hashes remain unchanged in each run;
`hashes.json` retains the full comparison.

## Verification and review

- Full `make -j$(nproc)` succeeds.
- Eight LinkPanelOverlay tests, 48 generic picking/automation/page-gap tests, six gutter-route and
  anchor-motion tests, and two configuration tests pass.
- Backend comparison succeeds in `build/link-colour-backend-comparison-04/`: glyphs, picking,
  overlays, atlas growth, coarse text and zoom checks. The short sample cannot split threaded page
  recording. The broader binary orchestration battery was intentionally omitted from this run.
- Formatting, lint and `git diff --check` pass.
- Independent aesthetics and UX reviews accept separate two-owner routes and discrete highlight
  colours. Their native04 review found an unresolved-companion panel overlap; native05 confirms the
  placement correction on all three backends. Both independent reviewers approve the corrected
  frames: the notes line and anchors remain clear while its occurrence stays explicitly unchosen.

The first routing attempt used an unnormalized knot vector and was rejected by the native NURBS API;
the normalized route was rerun. The initial backend comparison failed because an OpenGL shader
warning appeared as a notification in the image. Initializing the owner array removes that warning;
the fresh comparison passes. Neither failed capture is acceptance evidence.

## Fresh remaining-check results — 11 October 2026

Fresh isolated measurements now cover dense lane exhaustion at ten, 100 and 200 links. The 100-link
fixture retains 320 authored strands, the 200-link fixture 640; no relation is discarded. All 100
owner identities are reachable through the default Next-link control and wrap in order. Pointer
Previous/Next controls reset the independent occurrence choices. Full captions remain readable in
the constrained overlap panel. These are prepared-fixture diagnostics, not UI authoring passes.

Retained caret geometry and the conservative Session media-classification cache reduce selected
100-owner preparation from 5684.365 ms to 1.371 ms in the isolated software OpenGL run. Median
steady frames are 6.949 ms for ten, 12.258 ms for 100 and 15.033 ms for 200; all steady samples
report zero shaping. Commands, source/permascroll hashes and logs are preserved under
`build/ux-full-20261010/stress-dense/`, particularly `resume-isolated-results.json` and the
`*-media-r2-isolated/` directories. Hardware performance is not inferred from these timings.

Eight component cases cover rotated/depth-overlapping pages, foreground occlusion, a single visible
endpoint, dark paper and enlarged type on OpenGL, GLES and Vulkan. All 24 processes complete with
source hashes unchanged; independent frame review accepts bounded attachment and clipping behavior.
Evidence: `stress-dense/component-final/`. Roll120 framing still clips content, some bends look
faceted, and the enlarged dark case shows a collapsed Controls dock. The component placement harness
is not an ordinary UI rotation affordance.

Fresh no-sworph KJV entry now keeps the visible notes companion alongside the selected Psalm
passage. All three backends focus exactly `[2302771,2303273)` across pages622–623 while the notes
occurrence remains explicitly unchosen. The complete controls sit above the notes body. Independent
review accepts this pairwise framing; arbitrary many-document framing is not established. Evidence:
`build/link-colour-route-{opengl,opengles,vulkan}-resume-11/entered-remote.png`, `run.log`,
`hashes.json` and `permascroll-integrity.json`. The isolated clone appends about55KB of
configuration primedia; its complete original source prefix remains identical.

Full backend comparison, including binary orchestration, passes in
`build/ux-full-20261010/backend-resume/run.log`. Thirty panel/overlay/cache regressions and final
format/lint checks pass. The final overlap layout accounts for caption-role padding rather than
assuming every control uses the panel label font's line height.

## Scope remaining

Dense owner cycling establishes sequential identity access, not independent pointer picking of every
saturated route. Fresh UI authoring now creates a 2-left/3-right clasp with distant cells8/52/92.
When its owning store is primary, both endsets remain visible during independent cursor choices,
entry into A/C and two activity futures; both cell futures restore after reopening. Visited-store
hashes remain unchanged during navigation. Evidence: `build/ux-full-20261010/lead-j16-author-r3/`
and `j16-primary-store/`.

The normal multi-store session still omits a secondary store's authored link from reader candidates.
Restored reverse document entry also fails to recover its link context. Walks
preview/reference/notes and a genuinely unavailable-target request remain unvalidated or have no
affordance. These findings prevent a complete J16 pass; the full real-work audit records the other
journey results and fixes.

One earlier dense selection frame contains blank body text; fresh consecutive settled frames contain
readable text and identical pixels, but the earlier failure remains recorded. Native accessibility
finds the application without the old empty-text panic. Direct D-Bus queries confirm its child tree
and corrected Document/Custom-path option Actions succeed, but the field exposes no
Text/EditableText interface; no native editing pass is claimed. Findings and fresh journey results
are tracked in [`audit-2026-10-10-real-work.md`](audit-2026-10-10-real-work.md). The shallower-angle
arrangement remains deferred and unwired.
