# Independent publication links: validation (2026-10-06)

## Scope

This batch adds independent signed package preparation, publishing, scroll rendezvous discovery,
endpoint review and exact publication downloads. It implements prerequisites for P4/P5/P6 in
[the publication journeys](ux_workflow_publication.md). It does not complete those journeys or P7:
commentary authoring, Alice's reply citing both commentaries, arbitrary package endpoint authoring,
reader visibility toggles and the complete four-user run still require interface validation.

The network fixture supplies a signed store containing authored many-to-many links and its sealed
history/permascroll/research dependencies. It provisions two independent curator keys and publishes
packages named Bob's Commentary and Carl's Commentary. Their cited snapshot is the fixture's Story
Ideas publication; this is not evidence that Bob and Carl created complete commentary documents
through the interface. No curator keys, incoming catalogs, incoming packages or direct peer
endpoints are supplied to the reader. The source manifest is supplied as Alice's already published
work.

The fixture's history/permascroll provenance records use placeholder OpenPGP signatures.
Package/catalog/manifest Ed25519 signatures, DHT signatures and piece hashes are real. This network
fixture does not certify OpenPGP provenance verification. The separate local publication runner
exercises actual GPG signing.

## Changes

- `Ctrl+Alt+Shift+P` prepares all authored links from an explicitly selected signed publication. The
  curator chooses a package title and stable name, reviews the retained endpoints, and can publish
  the same signed package to the explicit test swarm destination. Preparing, publishing and retrying
  do not choose an edition or reserve another sequence for the same package.
- `Ctrl+Alt+Shift+L` opens Links and responses. It joins `SHA1("xanalinks:" + globalScrollKey)` for
  the chosen publication's registered scrolls, receives signed catalog metadata, and offers packages
  for fetch and review. Catalog advertisements are checked against the downloaded curator identity,
  name, sequence, title and complete endpoint-key set. The existing BitTorrent metadata extension
  carries these catalogs; no auxiliary listener was added. Package and catalog DHT acknowledgements
  both precede Published status.
- Review retains one package/link selection and both ordered endsets. A cited publication opens
  through the existing download controls. Its immutable root, author key, name, sequence, title and
  author-selected microversion must match; all native-history and primedia dependencies must verify
  before installation. This path never resolves a newer mutable head in place of the citation.
- Package fetch/review writes only the private package cache. It does not call the legacy
  `adoptLinkPackage()` importer, which authors Structure operations. No visibility toggle is offered
  by this batch. Original document operations and earlier publication snapshots remain intact.
- Network sessions and seed preparation live on workers. Pending enrollment, unavailable peers,
  corrupt pieces, mismatched metadata and conflicting/older package sequences cannot produce Ready
  or Published status. Explicit cancellation persists across restart; retry keeps the signed bytes
  and sequence. Earlier own packages remain reviewable and are not reannounced after a newer package
  is queued.

Catalog format is now **2**, with typed document/package entries and referenced scroll keys. Package
format is **1**, with versioned signed endpoint metadata and immutable publication citations. Old
catalog format 1 and unversioned packages are refused explicitly, without a compatibility reader.
Development catalog caches must be regenerated. Native store format 4, publication format 2 and the
64-byte operation layout are unchanged; binary fixture regeneration is unnecessary.

The private package request/cache uses bencode format 1 and atomic staging/rename. Tests establish
process restart recovery, not power-loss durability. Cache/transfer limits are explicit; there is no
cache eviction UI yet.

## Validation

Evidence is retained under `build/publication-links/`. Native graphical runs use offscreen video,
dummy audio, software OpenGL and separate XDG profiles. The namespace run has a stable bootstrap at
10.77.0.1, publishers at 10.77.0.2 and readers at distinct addresses. Only the named local bootstrap
is introduced; public routers, trackers and local discovery are disabled.

The first direct-package run failed to bootstrap its download session because it bypassed mutable
name resolution. The fix retries bootstrap introductions in the fetch path too. Failed captures are
retained in `network-ui-failed-bootstrap/`; they are not passing evidence. An initially colliding
shortcut was replaced with Ctrl+Alt+Shift+L, with a keymap uniqueness regression check.

| Scope                                  | Affordance/check                                                                      | Outcome | Evidence                                                                                               |
| -------------------------------------- | ------------------------------------------------------------------------------------- | ------- | ------------------------------------------------------------------------------------------------------ |
| P4/P5 discovery prerequisite           | Ctrl+Alt+Shift+L, Find packages, fetch both curator results                           | Pass    | `network-ui/response-0-catalog.png`, `response-1-catalog.png`, logs                                    |
| Package endpoint review                | Review signed endpoints; inspect full scroll identities                               | Pass    | `response-0-review.png`, `response-1-review.png`, `*-keys.png`, accessibility dumps                    |
| Exact cited snapshots                  | Open related publication, refresh, open                                               | Pass    | `*-download.png`, `*-opened.png`; both native stores reconstructed from two dependencies               |
| P6 preparation/publishing prerequisite | Ctrl+Alt+Shift+P, choose supplied source, prepare, review, Publish reviewed package   | Pass    | `prepared.png`, `prepared-review.png`, `prepared-published.png`, `prepared.log`                        |
| Offline metadata recovery              | Reopen retained package cache without transports                                      | Pass    | Namespace integration and worker restart tests                                                         |
| Integrity/lifecycle                    | Piece/signature/advertisement checks, pin checks, cancellation/retry/high-water marks | Pass    | `focused-final.log`: 64 cases                                                                          |
| Existing network workflows             | Key/topic discovery, subscriptions, publication retrieval                             | Pass    | Three integrations in `network-final.log`; the final package integration in `network-review-final.log` |
| Existing transport                     | Separate namespace peer/mutable-name tests                                            | Pass    | `transport-final.log`: 12 cases                                                                        |

The first publishing-control check remained in Waiting for acknowledgement after 35 seconds. The
final fixture places Devin's publishing process on the publisher stack, separate from the bootstrap,
uses a distinct user profile/key, and refreshes after a bounded longer wait. Published status still
requires both acknowledgements. The before-fix evidence is in `network-ui-before-review-fix/`.

The captured initial review wrapped long hashes into neighboring rows. The corrected review shows
ranges first, compact identities and a separate numbered-parts inspector for full keys. Choice
fields retain complete metadata as accessibility descriptions; a library regression verifies both
selected and individual option descriptions and unchanged underlying answers. Final 800×600 OpenGL
captures were inspected and show readable ranges, citation labels and mock publishing status.

The local publication runner also passed signing, author edition choices, cached opening, offline
reopening and download controls, with evidence in `build/publication-local/run-_1oihp3h/`. Its
preparation/profile boundaries remain those of the earlier publication reports.

Final native suites passed **540 library, 1,284 engine, 57 Xuzz and 119 ZigZag** cases. One optional
video case was skipped and one existing benchmark remains disabled. An initial full run failed the
existing predicate-pruning benchmark's timing ratio while the image was compiling; the later full
engine run passed its unchanged assertion at 14×. No benchmark assertion was relaxed.

The final `gleditor-swarm-test:local` image (`485bcd8f427d`) passed 129 smoke tests during its build
and again with Docker networking disabled. All 32 changed/new C++ source hashes match the host. An
audit under `/opt/gleditor` and `/work` found no private identity files, generated publication-state
or GPG private-key directories. The build reused local dependency/compiled layers and ran the
tracked recipe's Make/test tail, forcing changed objects to rebuild. Live UI acceptance used
rootless namespaces rather than Docker containers. Repository format-check and lint passed. The
separate clang-format 19 comparison found no new deviations; unchanged regions retain identical
baseline differences.

## Remaining findings

- Rendezvous is scroll-based and a permascroll can carry many works. Searching all registered
  scrolls can return packages about other passages. Results require endpoint review; precise
  publication-span filtering belongs with the reader layer/navigation integration. Discovery is a
  bounded peer query, not a promise that every commentary was found.
- The package form currently exports all links from a selected signed snapshot. Selecting/editing
  individual package links and creating a package independently of a source publication remain
  future UI work.
- Raw machine identities use compact rows and a numbered-parts inspector. Very long human titles can
  still exceed a form row. This build does not include AccessKit, so platform assistive-technology
  delivery is not validated.
- Enrollment, Oracle election, user creation/login and key revocation remain mocked/deferred. The
  package status explicitly labels mock verification; a curator signature alone proves key
  ownership, not enrollment or another author's provenance.
- Private reader enable/disable preferences, package contribution rendering, precise many-to-many
  navigation and their nonmutation/restart acceptance checks remain next work.
