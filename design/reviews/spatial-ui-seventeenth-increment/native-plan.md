# Seventeenth increment native publication-opening plan

This is a source/control inventory, not a native pass. No app was launched, no package was authored,
and no visit or source/profile state was changed. The previous increment's scripts and profile were
read only. No network simulation, fake transport, test swarm, fabricated response, or source fixture
is accepted as journey evidence.

## What the current controls can establish

| Question                                        | Existing user route                                                                                                                                                                                                                           | Finding and evidence boundary                                                                                                                                                                                                                                                                                                                                                       |
| ----------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Genuine signed package with a cited publication | Publish a document through `Ctrl+Shift+S`, then use “Publish independent links” (`Ctrl+Alt+Shift+P`) and “Prepare for review”. `Session::prepareLinkPackage()` signs the selected source publication and includes its exact `PublicationPin`. | Reachable through real UI controls. The previous native authoring scripts show the document/link/publication sequence, but their package preparation adds no network announcement. A fresh run can prepare a genuinely signed package that cites its source publication.                                                                                                            |
| Open exact citation                             | In “Review independent links”, choose the “Cited publication” field and “Open related publication”. This calls `PublicationInbox::submitPinned()` for that pin, then shows its download status.                                               | The route is present and pins the exact hash, publisher, salt, sequence, version, and title. A normal profile has no inbox transport: `PublicationInbox::download()` transitions to failure with “No publication swarm configured. Set up the local test swarm, then retry.” That failure is a truthful environment boundary, not an unavailable-content response from a real peer. |
| Real pending / cancel                           | Submit the exact pin and use the download status form's cancel control while a real transfer is queued/in progress.                                                                                                                           | Not reproducible in a normal launch without a configured real source. The no-transport worker can fail before a user can cancel. The available test-swarm configuration is explicitly excluded as mock/test transport and cannot establish native acquisition behavior.                                                                                                             |
| Ready / available baseline                      | Obtain and verify the pinned publication bytes from a real, independently configured peer; inspect “Ready to open”; stop before the final open/enter action.                                                                                  | No real peer, network node, or known publication source is configured for this run. The local authoring workflow produces a signed source and citation but does not make an inbox snapshot or download transport available. Do not claim a Ready/opening pass from the local published file or from a pre-seeded store.                                                             |
| Unknown curator identity                        | Inspect the existing signature/enrollment fields on a real received package.                                                                                                                                                                  | Signature verification and enrollment are separate, but incoming packages do not receive the optional identity callback; native received candidates remain Unknown. A real identity service or oracle response is not configured. Do not fabricate a status or interpret a signature as enrollment.                                                                                 |
| Invalid signature/payload                       | Receive a candidate that fails genuine signature, canonical payload, or advertised catalog checks.                                                                                                                                            | Those checks are in the real receive path, but existing user controls do not edit or corrupt signed bytes. Repacking or changing bytes outside the UI would be a diagnostic fixture, not a native user journey. No such result is available without a genuine malformed source.                                                                                                     |
| Unavailable cited content                       | Open a valid, signed citation through a real inbox transport that resolves the citation and cannot provide its exact immutable manifest/dependencies.                                                                                         | The form exposes the separate exact-pin acquisition route, but no actual peer/source is configured. The no-transport failure is “transport not configured,” not evidence that a cited publication is unavailable at its pinned hash.                                                                                                                                                |

## Safe native route when a real source is available

1. Create a fresh run directory. Export all six headless variables to that run's private XDG roots:
   `SDL_VIDEODRIVER=offscreen`, `SDL_AUDIODRIVER=dummy`, `LIBGL_ALWAYS_SOFTWARE=1`, and fresh
   `XDG_DATA_HOME`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, plus a fresh `GNUPGHOME`. Use Xvfb with
   `-noreset -screen 0 1600x1200x24`, OpenGL, and a 1440×1000 app window. Record exact binary/hash,
   argv, logs, accessibility dumps, frames, and source visit/source-store operation counts.
1. Through the UI, author a Link, publish a signed source snapshot, and prepare a signed package
   citing that source. Confirm the package signature and exact citation pin through the UI. Do not
   announce it unless the intended real publication workflow explicitly requires that action.
1. With an independently real, configured peer, use “Open related publication” and record the status
   transitions and exact pin. Capture the pending state and cancel only if the transfer is genuinely
   still active; otherwise report that state as unobserved. Retry/availability testing must remain
   pinned to the same hash.
1. Capture “Ready to open” as the available-content baseline. Stop at that status. Entering/opening
   the publication may append a reader visit, so do not take that action unless the journey is
   explicitly authorized to record it; if authorized, capture before/after activity and source
   operation evidence and label the resulting visit as an intentional journey action.

## Current conclusion

The UI affordance and exact-pin handoff exist, and genuine signed package preparation can be done
through the interface. Current available local controls cannot establish a truthful Ready, pending,
cancel, unknown-enrollment response, invalid signed payload, or unavailable exact citation from a
real peer. A no-transport failure can be captured as a configuration failure, but it does not answer
those publication lifecycle questions. No source edits, fixtures, launches, or visits were made for
this inventory.
