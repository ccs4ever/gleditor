# Publication implementation progress

This is the first implementation batch following the
[validation report](ux_publication_validation_2026-10-04.md). It repairs prerequisites for
[the seven publication journeys](ux_workflow_publication.md); none of P1–P7 is yet an end-to-end
pass.

## Implemented

- **Current-store slice creation.** `Ctrl+Alt+Shift+N` adds the first slice to the current
  document's store, retaining its text and ancestry. The binding lives in `system://keymap`. An
  existing slice produces an explanatory message instead of an exception. `Ctrl+Alt+N` still creates
  a standalone store. Both actions share their creation/presentation code.
- **Search input isolation.** Swarm Telescope participates in modal input ahead of ZigZag, with
  publication/quotation forms taking precedence. It accepts composed text, UTF-8 caret movement and
  deletion, closes on Escape, and exposes search/results/open/close accessibility nodes and actions.
  Search changes do not append document operations. Production catalogs start empty; demonstration
  seeding remains an explicit fixture API.
- **Durable publication counters.** Session reserves a sequence before signing/sealing, in a synced
  LMDB transaction under its primary profile. Counters are independent for each public key and salt;
  concurrent callers cannot reuse numbers. Reservations can leave gaps on failure. Previously signed
  manifests provide a minimum when migrating from timestamp sequences. Invalid/unsupported counter
  records and exhaustion fail explicitly rather than resetting freshness. Publication timestamps
  remain separate.
- **History and dependencies.** The core `publishDocument()` helper now carries a complete V5
  operation-history snapshot and requires signed permascroll/history provenance. Session retains its
  incremental history path. History-bearing manifests include dependencies from operations and link
  endpoints, including branches, cells and quotations absent from the current text. Break markers
  retain their sentinel identity through history serialization/localization.
- **Seedable immutable output.** Permascroll and history seals write their actual payloads beneath
  `<output>/<infohash>/<torrent-name>/`, with `metainfo.torrent` beside the torrent-name directory.
  A seeder uses `<output>/<infohash>` as its save directory. Payloads are checked against metainfo;
  invalid paths, duplicate files and failed writes are refused. Later seals cannot overwrite a
  different edition's payload. Existing withheld-content tests use the new layout and still check
  that private ranges are zero-filled on the wire.

No native store or operation-wire layout changed; fixture regeneration is unnecessary.

## Validation

Fresh evidence is under `build/publication-first-fixes/`, with UI captures under
`build/publication_same_store/` and `build/publication_search/`. These are local ignored artifacts.
The build used `make -j$(nproc)`; graphical checks used offscreen video, dummy audio, software
OpenGL and separate XDG profiles.

| Scope                           | Affordance or check                                                                             | Outcome | Evidence                                                                                                       |
| ------------------------------- | ----------------------------------------------------------------------------------------------- | ------- | -------------------------------------------------------------------------------------------------------------- |
| P1 prerequisite                 | Type text, add slice with `Ctrl+Alt+Shift+N`, edit a cell, close/reopen                         | Pass    | `ui-tests.log`, `publication_same_store/slice.png`, `reopened.png`, native dumps                               |
| P3 input repair                 | `F3`, type search text, UTF-8 Backspace, Escape                                                 | Pass    | `ui-tests.log`, `publication_search/search.png`, `document.png`, accessibility dumps                           |
| Existing slice workflow         | `Ctrl+Alt+N`, create/link cells and resume editing                                              | Pass    | All four selected UI/keyboard tests passed                                                                     |
| Publication counter integration | Submit Publish form twice in separate application runs                                          | Pass    | `form/edition-1.log`, `edition-2.log`, signed manifests and `sequences.json`; one publisher, sequences 1 and 2 |
| History/globalization           | Reconstruct text, branch, external quotation, page break and linked cells from seed files       | Pass    | Signed-provenance regression and `engine-tests.log`                                                            |
| Counter integrity               | Concurrent allocation, key/salt isolation, timestamp migration, overflow and unsupported format | Pass    | `engine-tests.log`                                                                                             |
| Generated-seal transport        | Seeder plus three simultaneous readers on an internal Docker network                            | Pass    | `generated-seal-swarm/`: six tests per reader, 18 total                                                        |

The transport diagnostic used the existing Docker transport-test image to read a seed generated by
this batch's native application. It did not rebuild that image with these UI changes. The runner
removed its four containers and internal network. Temporary GPG and publication private keys from
the form fixture were removed after its two submissions.

All 89 focused engine tests and 52 related sealing/torrent/package tests passed. The four
UI/keyboard tests and 18 generated-seal network checks passed. Repository formatting and lint
passed.

The engine checks cover publication, counter allocation, provenance, V5 binary operations, withheld
content, search indexing, empty catalog construction and system/keymap behavior. UI frames were
inspected. Internal accessibility descriptions passed; platform assistive-technology delivery was
not tested because AccessKit is absent from this build. Network tests establish payload transfer and
mutable-name retrieval, not authenticated remote catalog ingestion or whole-store reconstruction.

## Remaining work

1. Implement one background publication coordinator for dependency review/sealing, seeding, mutable
   pointer publication and completion/retry state. Add topic fields and the mock verification
   boundary. Reconcile Session's per-store seal with the author's shared permascroll identity and
   validate incremental seals after restart.
1. Carry the complete store inventory, designated versions and annotations in the publication
   format, and reconstruct every advertised document/slice from an empty remote cache.
1. Add signed author catalog publication/ingestion and topic rendezvous exchange, author-key
   following and remote opening. An empty catalog now honestly stays empty until data is learned.
1. Add persisted update subscriptions, sequence polling, dependency verification, retry and
   acknowledgement, including missed updates after reconnect.
1. Add commentary/backlink and independent link-package creation, review, announcement and
   discovery. Package visibility must be a private reader preference that filters contributions
   without appending operations to visited stores.
1. Rerun P1–P7 and their rejection/offline/retry cases through the UI. The first batch's unit, form
   and transport passes do not substitute for that acceptance run.
