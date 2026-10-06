# Publication update validation — 2026-10-06

This batch adds explicit notification subscriptions to completed publication downloads. Readers
review signed sequence changes, retain old and new stores independently, compare snapshots and mark
updates reviewed. The author controls the published microversion and editions; polling makes no
edition decision. This is the update prerequisite for P5. The original seven journeys still need
commentary/backlink and independent link-package workflows and their complete interface run.

## Interface evidence

| Journey         | Step                                                               | Affordance                                                   | Outcome         | Evidence                                                                                             |
| --------------- | ------------------------------------------------------------------ | ------------------------------------------------------------ | --------------- | ---------------------------------------------------------------------------------------------------- |
| P5 prerequisite | Bob and Carl enable notifications for Alice's verified publication | Ctrl+O, publication URI, refresh, Notify me of updates       | Pass            | `build/publication-updates/network-ui/bob-subscribed.ppm`, `carl-subscribed.ppm`, corresponding logs |
| P5 prerequisite | Bob receives an update while connected                             | Automatic shared toast and polite accessibility notification | Pass            | `bob-live.log`, notification timeline captures                                                       |
| P5 prerequisite | Carl discovers the missed update after restarting                  | Persisted subscription and automatic worker polling          | Pass            | `carl-reconnect.log`, notification timeline captures                                                 |
| P5 prerequisite | Review selected old/new authorial microversions                    | Ctrl+Shift+U                                                 | Pass            | `bob-updates.ppm`, `carl-updates.ppm`                                                                |
| P5 prerequisite | Open the verified update alongside its earlier snapshot            | Open update alongside earlier version                        | Pass (overview) | `bob-compared.ppm`, `carl-compared.ppm`                                                              |
| P5 prerequisite | Retain reviewed state and suppress duplicate toast after restart   | Normal application restart, Ctrl+Shift+U                     | Pass            | `carl-restarted.log`, `carl-restarted.ppm`                                                           |

The fixture uses temporary publishing keys and explicit mock verification. Its stable DHT bootstrap,
publisher and readers have distinct private IP addresses. The reader receives Alice's initial
publication URI and a DHT bootstrap node; no content cache or direct BitTorrent peer is supplied.
The initial and revised mixed document/slice stores are prepared by the fixture. Those preparations
are transport preconditions, not evidence of UI completion of Alice's editing or publication
journey. Live acceptance uses rootless network namespaces; Docker supplies a separate reproducible
build and offline smoke test.

Keyboard automation uses the default bindings and drawn forms. Offscreen OpenGL frames and internal
accessibility dumps were inspected. Platform assistive-technology integration, other rendering
backends and public-internet behavior were not validated.

## Integrity and persistence

One worker polls each subscribed key/salt, preserving a high-water mark only for accepted complete
stores. The inbox re-resolves the signed name and checks it against the observed sequence/hash
before fetching. A newer signed response is admissible; rollback and conflicting equal-sequence
responses are refused. Authorship, immutable content and the complete dependency closure are
verified before a notice is accepted. Earlier text and quotations remain stable, and downloaded
primedia stays out of the reader's permascroll. Opening either version appends no operations to the
publication.

The private LMDB format `XPS1` retains subscriptions, pending download identities, old/new
microversions and snapshots, delivered flags and review acknowledgements. Restoring validates the
retained signed snapshots against the pinned key/salt and sequence/version. Unsupported versions
raise a typed numeric refusal. Store tables remain format 4 and publications remain format 2.
Bounded pending notices require review before accepting more; reviewed snapshots remain available in
the inbox. Titles outside the subscription format's 1–1,024-byte limit are refused before writing a
record. An invalid update leaves the accepted sequence unchanged and permits a later valid update;
its rejection does not make the restart cache unreadable. Pause/resume, retry, cancellation and
worker shutdown have dedicated regression coverage.

The toast is claimed durably before presentation, giving at-most-once delivery. A process crash
between that claim and drawing can omit the transient toast; the unreviewed update still appears in
the palette. Notice delivery is paced to prevent a burst from evicting earlier toasts. Durable
claims currently run in the UI handoff; a slow local disk can delay that frame. Network resolution,
transfer and verification remain on workers.

## Regression checks

The full headless `make test` run passed 539 library, 1,269 engine, 57 Xuzz and 119 ZigZag cases, 12
transport/mutable-name cases and all three publication namespace integrations. The final comparison
rerun passed independently before that full run. After the title-boundary fix, the focused native
run passed 60 cases; the final engine suite passed 1,270 cases. One optional video case was skipped
and one benchmark remains disabled. Logs are under `build/publication-updates/`.

The local publication runner passed signing, author edition review, cached opening and offline
controls, retaining evidence in `build/publication-local/run-2gytb_n4/`. It does not replace the
network journeys. Default keyboard forms, live/missed-update toasts, comparison overviews and the
reviewed restart were inspected in the retained OpenGL captures and accessibility dumps.

The final `gleditor-swarm-test:local` image (`331764ac1df1`) passed 115 smoke tests during its build
and again with Docker networking disabled. Fourteen changed/new C++ source hashes match the host,
and an audit under `/opt/gleditor` and `/work` found no generated publication-state or private
identity directories. The build reuses the prior local dependency layer and runs the tracked
recipe's Make/build/test tail, forcing changed objects to rebuild. Live acceptance used rootless
namespaces rather than Docker containers.

Repository format-check and lint passed with the installed tools. A separate clang-format 19
comparison found no new deviations in changed/new code; four touched files retain identical baseline
differences in unchanged regions. New subscription and test code also passed clang-format 19
directly.

## Remaining findings

- A comparison fits both first-page widths. On a narrow screen this is an overview rather than full
  reading size; zooming or focusing a passage is still needed for long text. Document-tab labels
  retain the wrapping finding from the download report.
- The mock verification boundary does not implement enrollment, Oracle election, user login or key
  creation/revocation UI. Ordinary offline sessions report unavailable polling rather than claiming
  to have checked the DHT.
- Carl's final review-only restart received a stale signed DHT answer in the comparison rerun. The
  reader rejected it, retained accepted sequence 2 and the reviewed state, and scheduled a retry.
  That capture (`network-ui/carl-stale-restart.png`) proves persistence and refusal; it does not
  prove a fresh answer on that restart. The later full-suite run returned the accepted sequence and
  showed Waiting for newer publication, retaining review state without another toast.
- The designated stable bootstrap is used here. Intermittent recovery when a restarted publisher is
  the sole bootstrap remains the finding recorded in the discovery report.
- Initial publication signing/sealing still begins on the rendering command path. Commentary
  discovery and independent package creation/discovery/visibility controls remain later batches.
- The full P1–P7 run, including Alice's response referencing commentaries and Devin's package, has
  not been completed.
