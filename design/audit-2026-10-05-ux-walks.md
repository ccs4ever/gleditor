# UX follow-up: Walks, visit notes and references

This continues the [known-authority audit](audit-2026-10-05-ux-known-authorities.md), addressing
J16's missing saved-visit browser, annotations and references. It does not declare all of J16
complete.

| Step                                                              | Affordance                                                                                                                                                  | Outcome                                                                           | Evidence                                                                            |
| ----------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| Create two sibling visits                                         | Type and select; Ctrl+Alt+[ and Ctrl+Alt+] stage the clasp; Ctrl+Alt+L forges it; Alt+Shift+N, X, Return enter; Alt+Shift+B returns before the second entry | Pass; both children retain the same parent                                        | `/tmp/ux-walks-validation/prepare.args.json`, `prepare.log`; binary regression      |
| Preview and annotate the earlier child while the other is current | Alt+Shift+W opens Walks; Home, Down preview; N edits; Return saves                                                                                          | Pass; preview Visit 2 while Visit 3 stays current; note persists                  | `annotated.png`, `annotated.log` in the same directory                              |
| Reference a visit using the pointer                               | Click Reference in Walks                                                                                                                                    | Pass; reference persists; underlying caret and document operations stay unchanged | `annotated.args.json`, `hashes.json`; binary regression                             |
| Restore saved branches after restart                              | Alt+Shift+W; preview with arrows; Enter restores                                                                                                            | Pass; saved side and endpoint return without creating another visit               | `preview.png`, `restored.png`, `reference-other.png`, `restored-other.png` and logs |
| Annotate an unavailable target and attempt restoration            | Ctrl+W closes the document; Walks previews its visit; N edits; Enter saves; Enter attempts restoration                                                      | Pass; note remains editable, refusal is visible, current visit is unchanged       | `unavailable.png`, `closed.log`; binary regression                                  |
| Deliver native accessibility actions                              | Not exercised with native assistive technology                                                                                                              | Remaining validation gap                                                          | Headless accessibility tree describes controls, focus and modal state only          |

The captures above were inspected. Each run uses independent XDG data and configuration directories;
all authored text and links are created through default keyboard bindings. The replay driver and
argument files are retained under `/tmp/ux-walks-replay.py` and `/tmp/ux-walks-validation/`.
Automation uses the same input routes as these controls. Document operation and table hashes match
before and after preview, annotation, reference and restoration. The permanent regression separately
checks visit count, selected visit, persisted note/reference records and visited-document operation
counts. It also removes the document from disk to verify unavailable-target handling after restart.

Walks is available through **Alt+Shift+W** and the View radial submenu. Its bounded visit list shows
roots and parent IDs, the current visit and references. A separate preview cursor shows the durable
authority/version, target range, availability, arrival link and active side. Up/Down and Home/End
select previews; Enter restores; R references; N edits a note. Tab reaches the visible buttons;
Escape cancels an edit or closes Walks. Notes currently support appending text and Backspace rather
than a full text editor. References mark existing visits; they do not create another visit.

Annotations and references append records in the private activity store on `d.activity-annotations`
and `d.activity-references`. The latest note revision is displayed; earlier records remain. Unknown
visits are refused before writing, and repeated references are idempotent. Preview does not select
the navigator's current visit. Restoration selects an existing visit only after the target readiness
guard succeeds. No action writes to a visited document or slice.

Walks captures pointer presses and editing keys. Document commands are blocked while it is open; the
configured Quit command remains available. The synthetic click path now delivers modal presses
through the application's input handler. Accessibility controls decode their owned IDs and expose
Click/Focus, while the note exposes SetValue. A headless tree is evidence of the description, not
native accessibility delivery.

Validation: full build, repository format and lint gates, focused `clang-analyzer-*` checks, 26
navigation/activity engine tests and 39 command/accessibility library tests pass. The binary
regression and backend comparison results are recorded in
`/tmp/ux-walks-{ui-test-final,backends}.log`; build and gate logs use the same prefix. Software
Vulkan uses SwiftShader and the previously established two-level channel rounding allowance. The
small default rendering sample does not exercise threaded page recording.

Remaining work includes fully coincident link selection, unseen external and remote authorities,
native accessibility delivery and unopened ZigZag formatting inheritance. The richer Walks design
still proposes named walks, destination snippets, reference labels and a structural visit index;
this implementation displays the existing append-only visit records and their parent relations.
