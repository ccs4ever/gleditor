# UX follow-up: pointer access to coincident links

This continues the [Walks audit](audit-2026-10-05-ux-walks.md). Two authored links can have
identical left and right endsets while remaining distinct link identities. Their ribbon bodies
coincide; a GPU pick returns the foremost ribbon, leaving the covered identity unreachable through
that body.

The selected-link panel now exposes **Previous link** and **Next link**, with a **Link i/N**
indicator. These controls send the existing `StepLink` commands used by Alt+Shift+P and Alt+Shift+N.
Each selection shows its identity, type, owner and both complete endsets. Selecting a covered link
requires no guessed endpoint pairing; Enter remains a separate action.

| Step                                    | Affordance                                                                                                | Outcome                                                                                                  | Evidence                                                                                                              |
| --------------------------------------- | --------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| Author two links with identical endsets | Type/select in two documents; stage each side with Ctrl+Alt+[ and Ctrl+Alt+]; forge twice with Ctrl+Alt+L | Pass; two identities share the same ribbon body                                                          | `/tmp/ux-coincident-ui/coincident.png`, `prepare.args.json`, `prepare.log`; binary regression compares stored endsets |
| Select the foremost ribbon              | Click its body at 400,300                                                                                 | Pass; selects link 32, shows Link 2/2, keeps caret 2                                                     | `picked.png`, `picked.args.json`, `picked.log` in the same directory                                                  |
| Reach the covered link                  | Click Previous link                                                                                       | Pass; selects link 20 and shows Link 1/2; caret stays 2                                                  | `previous.png`, `cycled.log`; binary regression                                                                       |
| Return to the other identity            | Click Next link                                                                                           | Pass; link 32 and Link 2/2 return; caret stays 2                                                         | `next.png`, `cycled.log`; binary regression                                                                           |
| Compare pointer and keymap selection    | Previous/Next clicks and Alt+Shift+P/N                                                                    | Pass; both follow the same stable sequence                                                               | `parity.args.json`, `parity.log`                                                                                      |
| Read without changing visited stores    | Cycle both identities and quit                                                                            | Pass; both stores' operations and tables remain byte-identical; visit count/current visit stay unchanged | `before-cycling-hashes.json`, `after-cycling-hashes.json`; binary regression                                          |
| Deliver native accessibility actions    | Not exercised with native assistive technology                                                            | Remaining validation gap                                                                                 | Headless tree exposes the same controls; command mapping is tested                                                    |

The frames above were inspected. The replay driver is `/tmp/ux-coincident-replay.py`; argument files
record the exact UI inputs. Each fixture has independent XDG data/configuration directories. The
permanent binary regression authors the links through the same default bindings, checks their equal
endsets, then exercises real GPU beam and panel picks across relaunch. It checks the exact selected
IDs, saved caret, current visit, visit count, and operation counts in both documents.

This closes pointer reachability for completely coincident ribbon bodies by cycling candidates. It
does not implement a direct overlap comparison list or identify all ribbons under a pixel. The
candidate set is the existing list of links owned by the open reading stores, ordered by store index
and authored link ID, rather than only links touching the clicked passage. The active link stays
pinned if it leaves that list; its heading reports that state instead of silently selecting a
replacement. Changes to the candidate set invalidate the panel's cached content.

The new controls use the shared panel command list for drawing, pointer picks and accessibility
nodes. Candidate-position lookup is linear in the existing candidate list, as is the existing
StepLink lookup. It runs when the panel changes, without adding endpoint resolution or a Cartesian
product of left and right members. No new performance threshold is claimed.

Validation logs use `/tmp/ux-coincident-`: `build-final.log`, `engine-tests.log`,
`binary-tests.log`, `format.log`, `lint.log`, `analysis.log` and `backends.log`. Validation includes
the full build, formatting/lint, focused analyzer checks, engine panel/navigation tests, the
coincident-link and Walks binary regressions, and headless OpenGL/GLES/software-Vulkan rendering.
Software Vulkan uses SwiftShader and the established channel-rounding allowance; the default small
rendering sample does not exercise threaded page recording.

Remaining work: native accessibility delivery, discovery of unseen external/remote authorities,
unopened ZigZag formatting inheritance, and the richer direct overlap chooser. This follow-up does
not declare all of J16 complete.

Final results: 37 panel/navigation/activity engine tests pass, both focused binary regressions pass,
and all 33 binary scenarios pass on each of OpenGL, GLES and software Vulkan. The backend comparison
passes. The new panel captures were also inspected on GLES and Vulkan, retained as
`/tmp/ux-coincident-gles.png` and `/tmp/ux-coincident-vulkan.png`. Build, format, lint and focused
analysis gates pass.
