# Independent final review: readable navigation landing

Disposition: accept the bounded native correction for the captured short, single-line document
endpoints at 1440 by 1000. The previously clipped Enter passage is readable, and selected tabs now
agree with document reading focus. This does not close complete J16, J24 or J25 acceptance.

This reviewer wrote no product code. Reviewed all nine native PNGs under
`evidence/automated/{opengl,opengles,vulkan}/`: `entered.png`, `origin.png` and `back.png` for each
backend, the three test logs, the new interface E2E test and the product diff. The backend images
establish consistent settled appearance; they are not a motion recording or performance measurement.
Source-operation preservation awaits the separate same-profile validator evidence.

| State  | Independent visual finding                                                                                                                                                                                               | Bounded result                                                                                 |
| ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------- |
| Enter  | The complete `Origin exact passage.` is visible, beginning inside the left viewport. Doc 2 has the selected tab cue; the link panel says reading left member 1, bytes 0–21. The right companion passage remains visible. | Prior clipped landing and stale-tab findings resolved in all three backend frames.             |
| Origin | The complete `Distant target exact passage.` is visible on the right. Doc 3 is selected and the panel says reading right member 1, bytes 0–29. The source companion remains partly visible.                              | Frozen Return checkpoint and selected document are visually coherent for this fixture.         |
| Back   | The default tab is selected and the caret is on the empty root page. The retained Link still has both member counts and its original checkpoint metadata.                                                                | Restored root is distinct from Origin and no longer advertises Doc 3 as its selected document. |

The test uses UI authoring through default bindings, then the visible Enter, Origin and Back
controls. It awaits Doc 2, Doc 3 and default tab selection, checks exact caret selections and checks
root visit 1 with sibling visits 2 and 3. All three retained test logs report a pass. This automated
test exercises pointer controls rather than a keyboard-only or platform accessibility journey.

The correction uses `Views::activateDocument` to synchronize document activation before restoring
the exact range, and cancels ordinary page-zero framing. The existing Link companion alignment and
readability cap remain; `Views::keepInView` then brings the selected range's end and start into
view. This is a bounded reuse of the current architecture, with no new source renderer, host or
camera subsystem. The diff introduces no new spacing, colour or motion constants.

The dark tabs and panel remain visually consistent across backends: restrained cyan selection, clear
type hierarchy and muted provenance accompany high-contrast source text. Correcting the landing
gives the chosen sentence a clear visible anchor. The panel and overview overlap at the lower left
of Enter, and large empty page surfaces remain; those composition limitations are visible but do not
cover this fixture's words or controls. The slice HUD's `Focus: none` describes the slice, while the
selected document is conveyed by its tab and text caret; a unified focus description remains a later
orientation improvement.

## Remaining gates

No acceptance is given here for narrow screens, enlarged UI scale, long or multiline ranges, range
geometry pending construction, cell/historical targets, unavailable targets, source-operation
preservation, close/reopen restoration, reduced motion or animated continuity. In particular,
sequential end/start `keepInView` calls ensure a preferred endpoint but do not prove an entire long
range fits the readable viewport. The same-profile source and activity evidence, independent
keyboard navigation and broader journey audits must be recorded separately. Internal diagnostic tree
checks remain distinct from a platform AT-SPI session.
