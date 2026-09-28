# Xuzz link branch integration review

`feature/xuzz-link-context` ends at `0f14df6`, which is the merge base of that branch and
`design/ux-vpl-vprolog-real-work`. All 24 of its commits are already ancestors of the current
branch. Replaying them would duplicate changes, so no cherry-pick was made. The two commits on
`feature/xuzz-frictionless-navigation` have patch-equivalent commits `dc136c9` and `cf80afe` here;
the workflow and vision documents have since been extended. That branch adds no missing code.

| Commit    | Semantic review against current tree                                                                                                                                                                                                                |
| --------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `52fb034` | Exact per-member occurrence query remains the navigation input.                                                                                                                                                                                     |
| `257bdec` | One selected-link navigator remains; later activity persistence extends its first in-memory log.                                                                                                                                                    |
| `86b17d6` | Test target still builds the programs its suites launch.                                                                                                                                                                                            |
| `865d5ea` | Beam, cell, keymap, and accessibility adapters still route through the navigator.                                                                                                                                                                   |
| `2f4742d` | Vulkan descriptor-pool sizing remains a rendering fix, independent of link semantics.                                                                                                                                                               |
| `d8ce923` | Selected-link panel and whole-link preview remain in Xuzz.                                                                                                                                                                                          |
| `54efa34` | Vulkan reversed-Z fix remains useful for distant scene text.                                                                                                                                                                                        |
| `7481d81` | Satelloid pick identity fix remains useful.                                                                                                                                                                                                         |
| `cc2899e` | `link` and `cell-quote` structure-script verbs are fixture tools only. Later UI authoring (`19f51d6`) supersedes them for journeys; their test still uses them to verify stored spans. They cannot count as a UI pass.                              |
| `901ce4b` | Clickable context, reading position, and cell highlights remain.                                                                                                                                                                                    |
| `c6b64ab` | Readable cards remain; J14/J15 refined their ancillary type and value badges.                                                                                                                                                                       |
| `55a23f6` | Reading frame and overview remain; J15 explicitly transfers camera control for comparisons.                                                                                                                                                         |
| `080afeb` | Screenshot settling after link alignment remains needed for headless evidence.                                                                                                                                                                      |
| `75b802a` | Default link bindings remain in `system://keymap`; later actions extend the list.                                                                                                                                                                   |
| `7ea28ca` | UX validation skill remains; its journey count now includes J16.                                                                                                                                                                                    |
| `6b70e5f` | Pointer and chord automation still routes through real input paths.                                                                                                                                                                                 |
| `b636628` | J1–J6 audit is historical evidence, not current acceptance.                                                                                                                                                                                         |
| `2027107` | New-document settling and persistence remain in use.                                                                                                                                                                                                |
| `7afc0e2` | Keyboard-pane separation and keymap conflict resolution remain in use.                                                                                                                                                                              |
| `161ceec` | New-slice keyboard and radial controls remain in use.                                                                                                                                                                                               |
| `6b56d04` | Caret and edit bindings remain; J15 made version Back/Forward use the active view.                                                                                                                                                                  |
| `27247ba` | Activity-store reading place remains correct, but its resume path advanced a recorded historical slice to `Store::latest()`. That loses the reader's place and treats bookkeeping as an authorial head decision; this review removes that fallback. |
| `6a09213` | Persistent pouch behavior remains useful outside link navigation.                                                                                                                                                                                   |
| `0f14df6` | Selection drag/drop fix remains useful; it does not create an authored link pair.                                                                                                                                                                   |

The integrated UI covers selected link identity, independent member and occurrence cursors, exact
document/cell entry, distant-cell materialization, and branching Activity Forward. The prototype
collection also proposes overlap candidate browsing, large-endset search, a Walks explorer with
references and annotations, ambient provenance, and the Flap. Those are design targets, not features
supplied by either named branch. J16 exercises the boundary and records any missing affordances
without using the structure-script DSL as a shortcut.
