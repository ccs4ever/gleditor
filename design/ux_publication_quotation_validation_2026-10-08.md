# Publication quotation follow-up — 8 October 2026

This batch follows the offline-retention report. It repairs quotation pickup and native pouch
provenance. It does not certify the complete seven-user publication journey or real identity
verification and Oracle election.

## Implemented behavior

- A page pickup freezes the entire ordered span run, committed version, source store instance,
  scoped xanadoc birth and exact range. A cell pickup uses the pick from that press rather than the
  previous pointer target. Escape and release invalidate delayed pickups.
- Document drops share keyboard quotation insertion's whole-run address translation and preflight.
  Delayed destination picks retain the actual view object, store, version and scoped document;
  changes refuse the drop visibly. A drop within its own exact source selection is a no-op.
- A detached quotation creates a reader-owned native store, inserts into its authored xanadoc
  genesis, and preserves source operations. All members remain references to their original media.
- Pouch drops preflight the entire run, then produce one card per contiguous member, matching the
  keyboard convention. Active pointer movement and release are delegated through the modal drawer;
  an ordinary mouse release creates no quotation. Releases on drawer headers or blank space refuse
  placement rather than dropping through to a covered document. Failed placement uses the existing
  warning surface.
- The quotation card remains above the pouch and names a valid collection target. The redundant
  forge drag guide is removed; the existing provenance tether connects the source and card.
- Pouch provenance records native store authority, exact version, scoped document and range as
  authored Structure cells on `d.origin-store`, `d.origin-version`, `d.origin-document`,
  `d.origin-start` and `d.origin-end`; cell origins also use `d.origin-birth`. These are separate
  from a global document-state descriptor. No native record layout or tables format changes.
- The visible, accessible **Open source** row resolves the saved authority and exact version,
  validates the saved range against primedia identity, activates that scoped view and selects the
  passage. External scroll slots are compared through global identity. Completed arrivals use the
  reader's private activity store. Unavailable source locations leave quotation insertion available.

These native C++ changes implement input routing, persistence adapters and renderer integration.
They reuse existing pouch controls, keymap actions, span translation and navigation activity; there
is no new gesture or hardcoded key binding. Edition and current-version decisions remain with the
author.

## Evidence

The source publication is a prepared three-member fixture, using temporary keys and mock provenance.
Preparation is not a pass for publication creation, verification or discovery. The steps below use
native key bindings and pointer/accessibility controls after opening that fixture.

| Journey           | Step                                                     | Affordance used                                                               | Outcome                                                                                    | Evidence                                                                       |
| ----------------- | -------------------------------------------------------- | ----------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------ |
| P4 subset         | Drop selected text onto a page                           | Pointer drag                                                                  | Pass for a single member                                                                   | `aDraggedSelectionLandsWhereItIsDropped`                                       |
| P4 subset         | Detach a foreign three-member quotation                  | Pointer pickup and release in empty space                                     | Pass; new reader store, all three global identities preserved, source operations unchanged | `foreign-drag/spawned.png`, `spawn/transcript.log`                             |
| P4 subset         | Collect a three-member quotation                         | F2, named Zones control, close drawer, pickup, F2, pointer release into Notes | Pass; three saved cards with exact ordered source ranges                                   | `foreign-drag/pouch-dropped.png`, `pouch/transcript.log`                       |
| P4 subset         | Abandon a detached quotation                             | Escape, then pointer release                                                  | Pass; no new store                                                                         | `foreign-drag/cancelled.png`, `cancel/transcript.log`                          |
| P4/P5 subset      | Return to the exact saved source after restart           | F2, Zones, Open source: ALPHA                                                 | Pass; Alice's saved version/range and private activity arrival checked                     | `commentary/returned-source.png`, `returned-source.log`                        |
| Offline retention | Return when source is unavailable, then continue quoting | Open source: ALPHA, Notes page, Insert, Escape                                | Pass for warning diagnostic and retained quotation; native warning appearance unverified   | `commentary/unavailable-source.png`, `offline-after.png`, `offline-source.log` |

Captured frames, transcripts, native stores and permascrolls are under
`build/quotation-evidence/<backend>/`. Persisted assertions check addresses and operations; the
paginated drawer frame alone shows only the first card. The same bounded interaction tests are run
on the compiled backends, independently of the broader publication journeys.

The final focused OpenGL run passed all 22 cases (14 pouch, five tether and three interface
orchestrations). OpenGL ES and Vulkan each passed the three affected orchestration assertions.
Independent UX and aesthetics reviewers inspected the corrected OpenGL hover/drop frames: the
collection title, full preview, release instruction and Notes destination are readable. OpenGL ES
and Vulkan frames were also inspected; card positions vary during the running tether animation.

Vulkan is **not a clean diagnostic pass**. Its pouch transcript and hover capture show a validation
error: concurrent `vkDeviceWaitIdle` use of a queue. The functional assertions pass, but the error
notification overlaps the release instruction. Follow up the renderer/capture and glyph-cache idle
paths (`src/render/vulkan/device_vk.cpp`, `device_vk_frame.cpp`, `src/glyphcache/cache.cpp`) and
rerun this pouch case with strict diagnostics. This batch does not change generic device locking.
The [subsequent tendril and ownership batch](ux_quotation_tendril_validation_2026-10-08.md) fixes
the pouch event-thread buffer destruction and reruns this case with strict diagnostics.

The device-free/native suites passed 61 Xuzz and 193 slice/overlay cases. The engine run excluding
interface orchestrations passed 1,283 cases, skipped 17 (16 namespace-dependent network cases and a
video capture case), and narrowly failed the existing memoization benchmark: 34,695 hits/s against
35,000 under concurrent validation load. Its isolated retry passed at 47,088 hits/s; no threshold
was weakened. Both logs are retained. This is not a full `make test` pass.

`tools/compare-backends.sh` passed its library rendering, picking, overlay, atlas-growth and coarse
text checks for all three compiled backends. It ran with SDL offscreen because `xvfb-run` is absent.
The full orchestration extension was interrupted before completion and then excluded with
`XUDU_TEST_BIN`; the affected three-case backend runs above supply the bounded interface evidence.
The small sample did not exercise actual multi-thread command-recording splitting.

Format-check, lint, the UI text-policy check and `git diff --check` passed. Native operations dumps
for spawned reader stores and saved pouches are alongside their stores as `validation-ops.txt`. No
store layout or edition decision was changed.

## Remaining work

Saved cell source-return reports an explicit unavailable state. It needs an exact scoped slice
activation adapter. The existing activity `DocumentSite` still represents store, version and range,
without a scoped xanadoc birth; saved pouch source-return carries that birth independently, but
activity-history restoration for multiple xanadocs in one store needs its own follow-up.

The F9 startup-target issue, dense publication/pouch presentation, notification consolidation, and
full P4/P5 followed by P1–P7 validation remain open. This batch does not rerun network transport,
identity consensus or the local swarm.

Pointer pickup while the pouch is already modal remains blocked. The validated flow configures the
zone first, closes the drawer, picks up the source, and reopens the drawer with F2. A future
modeless or explicit background pickup adapter belongs in the pouch input design.

Independent frame review also found existing thick stepped tether geometry, a clipped source
companion after spawning, the oversized floating card, cramped forge/header labels and duplicate
ready notices. These remain presentation findings. Additional UI scales, reduced motion, dense/long
quotations and complete keyboard/accessibility parity for drag destinations have not been validated
in this batch.
