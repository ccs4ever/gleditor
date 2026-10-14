# Native Xuzz navigation validation (scoped J24/J25)

Build: `/workspace/gleditor` HEAD `4f2d3fac6fc4e5aa59d11618ae25dd647f6151c8`
(`feat/spatial-ui-overhaul`), runner `/workspace/spatial-build/run.sh`. This is a scoped
native-interface check, not a complete J24/J25 journey audit. The fixture was authored in Xuzz
through default key bindings; navigation used visible panel controls. No product files were changed.

The fixture contains two UI-authored documents: `Origin exact passage.` (selected span bytes 0–21)
and `Distant target exact passage.` (selected span bytes 0–29), joined as one link. Both documents
were open in the same running session. The link had one left and one right member, one occurrence
each, and one place each.

| Journey | Step                                 | Affordance used (menu, key, pointer, prompt)                                     | Outcome                                                                                                                                                                                                                         | Evidence                                                           |
| ------- | ------------------------------------ | -------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------ |
| J24     | Author endpoints and link            | `Ctrl+N`, type/select, `Ctrl+Alt+[`, `Ctrl+Alt+]`, `Ctrl+Alt+L` default bindings | Pass for this local two-document fixture; UI reports 1 left and 1 right member, each 1 occurrence and 1 place                                                                                                                   | `loaded-session.log`; `authored.png`; source operation dumps below |
| J24     | Enter distant endpoint               | Pointer on visible **Enter** button                                              | Pass for logical entry. `reading: left member 1` and exact range are retained; the landing frame clips the passage beginning outside the left viewport, so full readable landing is not accepted                                | `loaded-enter.png`; `loaded-session.log`                           |
| J24     | Return with Origin                   | Pointer on visible **Origin** button                                             | Pass. Reading returns to `right member 1`, at target bytes 0–29                                                                                                                                                                 | `loaded-origin.png`; `loaded-session.log`                          |
| J24     | Return with Back                     | Pointer on visible **Back** button                                               | First Back after Origin is a root no-op. Final Back restores default document focus at caret 0, corresponding to the immutable root target; it differs from Origin returning to the right passage.                              | `loaded-back.png`; `loaded-back-second.png`; `loaded-session.log`  |
| J25     | Enter twice from the root checkpoint | Pointer on visible **Enter** button twice, with Back between entries             | Pass for two sibling entries from the root: activity data contains two `visit2 1` records, both parent VisitId 1, and distinct linked visit identities 2 and 3. The repeated `1` is the parent field, not a duplicate identity. | `loaded-session.log`; `same-session-activity-after.txt`            |
| J25     | Restore root after second entry      | Pointer on visible **Back** button                                               | Pass for root activity/focus restoration: the final tree says `focus: default`, default caret 0, and current visit 1. The two sibling visit records remain.                                                                     | `loaded-back-second.png`; `same-session-activity-after.txt`        |

## Findings and limits

The logical endpoint traversal worked when both documents were already open in one session. The
Enter frame clips the beginning of the left passage beyond the viewport; it does not establish a
fully readable landing or camera framing acceptance. This does not establish opening a distant
document from an external/unavailable source. No unavailable target or pending confirmation appeared
in this fixture, so unavailable and cancel behavior were not exercised. Repeated occurrences,
alternate branches, and closing/reopening the app to resume a branch were not exercised. The failed
`--open-store` launch attempt is retained as diagnostic material only and is not counted as a
journey step; the successful journey used no store-preloading flag.

The source operation dumps from the navigation run match the deterministic UI-authored control
fixture by SHA-256: origin `0966c3dce31f7f5a3a161d1a8033eb2fee61716d512e43c709713787b7e1ccc9`,
target `07fda1ea881a3747c68498095ba0adc5c6b09d7af401bd1f88da6f19d81cc960`. These are independent
control-profile comparisons, not a same-profile pre-navigation snapshot; they show no source change
relative to that control fixture, but do not prove byte-for-byte same-session before/after identity.
Dumps: `same-session-source-origin-after.txt`, `same-session-source-target-after.txt`,
`baseline-origin.txt`, `baseline-target.txt`.

The visit activity dump was taken after the final Back. It shows root visit 1 plus two children
whose encoded parent field is 1; these sibling entries have distinct visit IDs 2 and 3. The
`d.activity-current` writes show ID 3 after the second Enter and ID 1 after the final Back, so Back
restored the root visit. The two successful Enter captures establish the displayed endpoint
transitions. Xuzz logs `accessibility: not in this build`; the retained a11y text dumps are
diagnostic UI-tree output, not an AT-SPI validation.

No code fix is proposed from this scoped run. Evidence includes per-step frames and the full native
session transcript; the UI-authored source and activity operation dumps are retained alongside them.
