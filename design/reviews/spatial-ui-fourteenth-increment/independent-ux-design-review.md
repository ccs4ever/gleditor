# Independent reader-package lifecycle review

Read-only review at `337c04727ba167f226e2ccb6f451b1e0d1cc7b2d`. I compared the current service and
UI behavior with the package choice contract in
`design/prototypes/spatial-journeys-2026-10-13/ contracts.md`, J24 steps 5 and its re-enable/dismiss
variants, the navigation contract, and the thirteenth-increment lifecycle audit. No product code or
test was changed. The bounded next change can establish disabled-but-pinned behavior; it should not
claim that Retain/Enable/Emphasis are now three separately persisted preferences until their
separate inventory model and controls exist.

## Requirement to current behavior

- **Retain independently:** `ReaderLinkPackages::retain()` verifies the carrier hash and holds the
  package in memory. Durable `package-visibility` stores only `InfoHash -> bool`; startup
  reconstructs payloads from exchange statuses when preferences exist. Retain is not a separate
  reader-owned inventory state, and exchange cache is coupled to curation.
- **Enable independently:** `Session::setLinkPackageEnabled()` calls `retain()` on enable, then
  stores a boolean. The review form exposes “Enable reader layer” / “Disable reader layer.” There is
  no separate Retain action, and the boolean cannot distinguish never-retained from retained but
  disabled.
- **Pin selection on disable:** `LinkContext::packageVisibilityChanged()` finds that `find(key)`
  returns null after disable and executes `nav::Dismiss`. `package()`/`find()` are enabled-only and
  `presentationLinks()` omits disabled links. The owning UI test explicitly expects
  `EXPECT_FALSE(navigation.selection())`. This loses the immutable selection, endpoint cursors, and
  inspection context required by J24.
- **Explain hidden drawing:** the package review form says “reader layer disabled,” but the selected
  link panel is dismissed. There is no visible disabled-state explanation, full carrier identity, or
  preserved left/right cursor and endset context on the reading surface.
- **Prevent stale beam actions:** `LinkBeams::performAction()` queues a `NavigationCommand` naming a
  stable key; `drawFrame()` later executes it. `LinkContext::execute()` rejects the key when the
  package is disabled. But the queued action has no package-visibility generation. If it survives
  disable and the exact same carrier is re-enabled before delivery, current lookup could authorize
  it. Link-panel actions carry a selection revision; beam accessibility actions do not.
- **Restore exact cursor state:** immutable link identity is hash plus ordinal, and `SelectedLink`
  carries side/member/occurrence cursors. Preferences persist the enabled boolean; package payload
  rehydration depends on exchange status. No disabled selected context survives the current Dismiss
  callback. A matching title/ordinal from a newer package must never restore the old context.
- **Emphasize independently:** authored `ProminenceTier` is link data. Reader package preferences
  have no Emphasis field or control. Reader emphasis needs its own persisted preference and visual
  effect without mutating the signed package or changing Retain/Enable.

## Recommended user-visible semantics

1. **Retain** adds the verified immutable carrier to the reader's curated inventory and keeps it
   inspectable across restart. Retain is separate from exchange discovery/download cache. Dismiss
   removes it from the active reader collection through a recoverable dismissed-items state; it does
   not delete the signed carrier or activity history. Display the carrier's immutable hash/sequence
   identity so same-title editions are plainly distinct.
1. **Enable** controls whether that exact retained carrier participates in beam drawing, beam hit
   testing, and automatic layer selection. Enabling requires the existing package verification gate.
   It never enables a newer same-name carrier. Disabling changes only the reader preference; no
   source operation or visit is appended.
1. **Emphasis** is a separate reader-owned visual preference. It changes the visual weight of that
   carrier when enabled, never its authored prominence or enabled state. Store it even while the
   layer is quiet/disabled so the preference takes effect if the reader later enables that same
   carrier. Keep the control visually quiet and accessible; do not add emphasis to the authored
   `ProminenceTier`.
1. **Disabled-but-pinned selection:** preserve the selected `LinkKey` (immutable authority/hash plus
   link ordinal), complete two endsets, active side, and left/right member and occurrence cursors.
   Keep the existing inspector/panel visible with a neutral, explicit notice such as “Drawing hidden
   — carrier #12 disabled; Link L17 remains selected.” Suppress package beam strands, selected-end
   highlights, anchor motion, and beam pick nodes while disabled. Show the full carrier identity,
   Enabled=No, and both cursor positions. Show Emphasis only if its preference exists. Until Retain
   is separately persisted, label this as a pinned selected context rather than reader-inventory
   retention. Do not substitute another carrier or clear either cursor. J24's example has Left 2/2
   and Right 1/3 selected, including an unavailable exact right occurrence; endpoint availability
   stays distinct from disabled drawing.
1. **Navigation policy:** while disabled, suppress drawing, beam picking, automatic layer selection,
   and endpoint Enter. Keep the exact inspector, endpoint browsing, and Cross available. Explicit
   Back/Origin restoration can proceed to its exact saved target with normal availability checks.
   Show why Enter is unavailable and provide explicit Re-enable and Dismiss actions through pointer,
   keyboard, and accessibility on the shared navigation command boundary. Register default
   keymap/Vortex paths as required by the Xuzz interaction contract. Re-enable restores drawing and
   the same identity/cursors; it must never retarget a different link. Do not silently dispatch a
   different link or occurrence.
1. **Queued actions:** capture the package key and visibility generation when creating a beam
   accessibility action. On delivery, require the same generation and `Enabled=true`, and verify the
   key still names that same immutable carrier. Disable cancels pending entry and invalidates the
   generation; disable-then-re-enable must not revive an old queued click. Apply the rule to
   deferred pointer/pick work. A fresh action after re-enable may act on the retained selection only
   through its exact key/cursors.
1. **Restart/re-enable/dismiss:** once the separate preference model exists, persist Retained,
   Enabled, and Emphasis independently by immutable carrier hash; do not infer reader retention from
   exchange status. For the bounded disabled-pin change, restore the same selected context and exact
   cursors from the exact payload if present. If the payload is missing, keep the immutable key/pin
   and explain that it is unavailable; do not replace it by a same-title package. Re-enable restores
   that same carrier's beams and leaves cursors unchanged. Explicit Dismiss clears the selected
   context and offers a route back to the retained inventory; it does not silently switch selection.

## Minimum acceptance before D2 consumes these states

- Enable a verified package, select one Link, choose non-default member/occurrence positions on both
  sides, then disable it. Assert its exact immutable key and both cursors remain selected; beams
  disappear; the panel identifies the disabled drawing; source stores and activity visit count do
  not change.
- Queue a beam accessibility action, disable and re-enable before queue delivery, then deliver the
  old action. It must be refused without focus movement, a new selection, or a visit. A newly issued
  action after re-enable must work against the same carrier key.
- Restart while disabled and selected. Verify the exact key and both cursors restore with no newer
  same-title carrier substituted. Once the independent preferences exist, also verify Retained,
  Enabled, and Emphasis separately. Re-enable and verify the same Link/cursors return to drawing;
  then separately Dismiss and confirm no automatic replacement.
- Exercise package state through visible controls, keyboard, and accessibility. The UI must expose
  Retain, Enable, Emphasis and Dismiss as distinct named decisions, and distinguish signature/
  verification, availability, retention, enablement, and emphasis. Do not use fixtures or simulated
  availability as proof of native package acquisition/trust.
