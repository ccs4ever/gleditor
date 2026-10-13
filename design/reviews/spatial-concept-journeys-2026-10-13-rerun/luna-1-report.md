# Independent review: J22–J26

This is an AI expert review of the scripted concept prototype and its captured fixture transitions,
not a participant study or a production pass. Action IDs refer to the runnable HTML prototype. I
inspected the rendered PNGs and state/effect traces for representative base actions and every
independent variant route, including a fresh rerender after the J23 reading-body correction. The
current `run.json` records no runtime failures; see the per-step evidence and the resolved J22 audit
finding below.

| Step  | Outcome           | Covered base actions       | Variant coverage                                                   |
| ----- | ----------------- | -------------------------- | ------------------------------------------------------------------ |
| J22/1 | Concept supported | J22-s1-a1–a2               | —                                                                  |
| J22/2 | Concept supported | J22-s2-a1–a2               | —                                                                  |
| J22/3 | Concept supported | J22-s3-a1–a3               | J22-v12-not-hit                                                    |
| J22/4 | Concept supported | J22-s4-a1–a4               | J22-anchor-fold; J22-partial-cancel                                |
| J22/5 | Concept supported | J22-s5-a1–a4               | —                                                                  |
| J23/1 | Concept supported | J23-s1-a1–a2               | —                                                                  |
| J23/2 | Concept supported | J23-s2-a1–a2               | J23-invalid-signature                                              |
| J23/3 | Concept supported | J23-s3-a1–a3               | —                                                                  |
| J23/4 | Concept supported | J23-s4-a1–a3               | J23-review-only; J23-retain-only                                   |
| J23/5 | Concept supported | J23-s5-a1–a3               | —                                                                  |
| J24/1 | Concept supported | J24-s1-a1–a3               | —                                                                  |
| J24/2 | Concept supported | J24-s2-a0–a7               | J24-overlap-link                                                   |
| J24/3 | Concept supported | J24-s3-a1–a5               | J24-cancel-unavailable                                             |
| J24/4 | Concept supported | J24-s4-a1–a5               | J24-wrong-version                                                  |
| J24/5 | Concept supported | J24-s5-a1–a2               | J24-reenable-carrier; J24-dismiss-carrier                          |
| J25/1 | Concept supported | J25-s1-a1                  | —                                                                  |
| J25/2 | Concept supported | J25-s2-a1–a3               | J25-preview-no-move                                                |
| J25/3 | Concept supported | J25-s3-a1, a1b, a2–a7, a4b | —                                                                  |
| J25/4 | Concept supported | J25-s4-a1–a5               | —                                                                  |
| J25/5 | Concept supported | J25-s5-a1–a6               | J25-unavailable                                                    |
| J26/1 | Concept supported | J26-s1-a1–a3               | —                                                                  |
| J26/2 | Concept supported | J26-s2-a1–a5               | —                                                                  |
| J26/3 | Concept supported | J26-s3-a1–a4               | —                                                                  |
| J26/4 | Concept supported | J26-s4-a1–a4               | J26-exact-duplicate-default; J26-equal-text; J26-shared-occurrence |
| J26/5 | Concept supported | J26-s5-a1–a5               | J26-availability                                                   |
| J26/6 | Concept supported | J26-s6-a1–a3               | J26-whole-cell-excluded                                            |

## Findings

**R1-J22-01 — resolved by final targeted rerender.** The earlier capture showed the origin reader
and stale Review side inspector at once. In the final
[J22-s5-a3](../../prototypes/spatial-journeys-2026-10-13/evidence/J22/main/J22-s5-a3.png) and
[J22-s5-a4](../../prototypes/spatial-journeys-2026-10-13/evidence/J22/main/J22-s5-a4.png) frames,
both the side card and center page agree on current visit 01, Lab notes V12, occurrence
`occ:lab-passage-09`, and offset 44. The fresh trace agrees for both actions. J22/5 is now Concept
supported.

Other representative frames support the intended distinctions. J23’s fresh rerender now aligns the
central reading body and trace with Review V4, including the reopen frame. J22’s tools preserve
Review reading while separate panels show C17 V8 and pinned V12 no-hit, and L17’s right member
remains a distinct C24 state. J24’s disabled-but-pinned L17 inspector presents explicit re-enable
and dismiss actions. J25’s exact retry of saved visit 05 resolves availability while current visit
06 and the existing route graph remain unchanged; Resume remains a separate action. J26 visibly
distinguishes whole-cell labels from selected spans, equal text at separate addresses, and shared
source ranges with distinct occurrences. Its availability variant preserves the typed identities and
type-specific recovery choices. These are visual concept outcomes for their captured states.

## Cross-check boundary

The browser run uses native DOM clicks and Tab/Enter against scripted fixtures. Run-level failures
are empty, and the traces record source-operation invariance and reader-visit effects for these
simulated transitions. The runtime’s accessibility-tree snapshot, keyboard delivery, responsive
layout, enlarged text, reduced motion and contrast checks describe this HTML prototype only;
`assistiveTechnologyTested` is false. None of the evidence establishes Xuzz interaction behavior,
real persistence, network responses, cryptographic checks, media playback, storage diffs or human
discoverability. `Concept supported` means that the task is coherently represented in the inspected
prototype state/affordances, not that implementation behavior is proven.
