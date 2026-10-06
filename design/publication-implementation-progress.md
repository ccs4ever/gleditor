# Publication implementation progress

These implementation batches follow the
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

## Restart and provenance fixes

The next batch persists the permascroll's device key and complete sealed-segment map in a private
LMDB transaction under `<permascroll>/publication-state` (`XUP1`, format version 1). Reopening
restores the same global name and incremental boundary. Identity mismatches, unknown versions,
noncontiguous segments and a checkpoint beyond the backing primedia are refused with
`PermascrollStateUnreadable`. Bytes are flushed before the descriptor transaction commits. Failed
seed output leaves the previous boundary intact. This local key persistence is a prerequisite;
OpenPGP delegation, mock enrollment and eventual revocation remain separate work.

Session publication now globalizes local spans against the author's shared permascroll rather than
sealing a second store-specific copy. Operations remain incremental per store. Invalid existing
store checkpoints and write failures are reported instead of silently restarting their history.
Withheld settings spans include the final operation, and their ranges are included before the
publication manifest is signed.

Signing and sealing share one redaction routine. Session signs separate primedia and history
records: the primedia digest covers exactly the new zero-filled wire payload, with its absolute
`permascroll_at`; the history digest covers exactly the new operations file. An unchanged history
claims no new operations file. Earlier publication artifacts retain their hashes and descriptors.

Run `make -j$(nproc) test/publication-local` to repeat the three-process Publish form journey. It
creates a fresh evidence directory under `build/publication-local/`, retains captures/logs, and
removes temporary private keys even when a check fails. The 98 focused engine tests and four
UI/keyboard tests also passed, as did repository formatting and lint.

Evidence from implementation validation is under `build/publication-restart-fixes/`. The form
journey checks sequences 1, 2 and 3, one permascroll key, reuse of all prior segment descriptors and
unchanged history reuse. The third process adds eight document bytes. Private settings/activity
updates also append primedia, so an unchanged document can still require a new author-scroll
segment. Seed payloads are checked against provenance digests and private ranges, and GPG verifies
the actual records before temporary signing keys are removed. These are local form and integrity
checks; DHT catalog ingestion and the complete P1–P7 acceptance run remain pending.

## Durable publication outbox

The third batch adds a persistent outbox (`XPO1`, version 1) under the primary store profile.
Preparing a publication commits its immutable signed request before returning. A worker verifies all
referenced seed files, metainfo hashes, piece hashes, segment coordinates, scroll keys and visible
span coverage before offering anything. File and data-directory symlink escapes, missing files and
incomplete piece tables are refused. Seed roots from earlier jobs remain available after restart,
including author-scroll segments retained beside another store.

The worker owns its BitTorrent session throughout construction, use and destruction. It seeds the
manifest and every dependency, then publishes the signed BEP 46 pointer. Completion requires a
`dht_put_alert` reporting successful remote responses and a signature matching that exact pointer,
salt and sequence. A value present only in the publisher's cache is insufficient. Bootstrap
introductions are retried because libtorrent's DHT startup is asynchronous. Failed jobs expose their
error; retry retains the signed sequence and manifest hash. Restart rechecks dependencies and
re-seeds jobs. A newer queued version supersedes an earlier announcement while retaining its seeds.

The Publish form adds signed, normalized topics and a destination choice. Local publication is the
default. `Ctrl+Shift+P` opens status, refresh and retry controls through the sovereign keymap and
accessibility tree. The publication command reports **prepared** while background work is pending.
Statuses distinguish local readiness, identity verification needed, seeding, awaiting DHT
acknowledgement, publication, failure and supersession.

For the test fixture, launch Xuzz with `--test-publication-swarm HOST:PORT` and repeatable
`--dht-node HOST:PORT`. This enables an explicitly labelled mock verification result for the
session's own publishing identity. The form's **Test swarm** destination opts into announcement. It
does not implement Oracle election, user enrollment or revocation. Ordinary local preparation
requires no mock enrollment. Signed topics are metadata at this stage; they do not yet make a
publication discoverable through a topic swarm or author catalog.

The outbox batch introduced publication format version 1 and topics. The inventory batch below
replaces it with version 2. Unversioned manifests and unsupported versions are refused by number
with `PublicationUnreadable`; regenerate owned test publications. Native store and operation formats
have not changed. Imported media now retains a multi-file torrent seed alongside its store, and
Session reloads its metainfo on reopen. Publication no longer depends on the imported file remaining
at its original path.

Validation evidence is under `build/publication-outbox-fixes/`. The engine and selected import
checks cover 114 publication/history/keymap/persistence, seed-escape and imported-media tests. The
selected PDF regression now uses the current capture flag and creates its own output directory.
`make -j$(nproc) test/publication-local` checks three UI publications with disposable GPG keys,
signed topics, status and actual keyboard retry. Captures and accessibility output are retained
under `build/publication-local/`; temporary secret keys are removed. The status frame was inspected
and its explanatory text shortened to fit the panel. The rebuilt Docker image
`gleditor-swarm-test:local` passed all 67 smoke tests with networking disabled. Repository
formatting and lint passed.

`make -j$(nproc) test/publication-swarm` exercises the outbox against a DHT node in a separate
network namespace. It stops the publisher before resolving the signed pointer, proving the remote
node retained it, then restarts the outbox and fetches/verifies the signed manifest with a fresh
reader. The pointer acknowledgement and lookup cross the veth device. The reader's manifest transfer
from the restarted publisher takes place within the reader namespace; this test does not establish
cross-machine transfer or reconstruction of the complete store inventory. The existing eleven
namespace transport/mutable-name tests run alongside this integration check under `test/swarm`, with
a fresh bootstrap fixture for each suite. Sharing the legacy suite's DHT fixture caused the outbox
check to stall; independent fixture lifetimes avoid that accumulated state. DHT protections are
unchanged.

Signing and initial sealing still run synchronously on the Session command path. This batch moves
dependency verification and network operations to the worker; moving snapshot creation and GPG
signing off that path requires a safely captured immutable store snapshot. These checks remain
prerequisites, not end-to-end passes for P1–P7.

## Whole-store inventory and author edition review

Publication format 2 signs the stable store ID, the global permascroll replacing author-local slot
zero, all terminal branch heads, every explicit xanadoc/slice birth and its branch-local names, and
the selected document birth. Births and heads use microversion names within the signed publication,
so changed operation indices on the reader do not change their identities. Editions and annotations
remain authoritative Structure operations; the inventory is a checked discovery index, not another
metadata writer. Version 1 and unversioned manifests are refused by number. Regenerate owned test
publications and their outbox records. Native store, node and binary-operation layouts are
unchanged.

`restorePublication()` creates a fresh store using the supplied reader permascroll, verifies torrent
pieces and history segment ordering/counts, remaps every global scroll descriptor and operation
reference, and checks the recovered inventory and selected EDL against the signed manifest. Missing
or corrupt dependencies, absent births and inconsistent inventories are refused. It appends no
primedia to the reader's scroll. The signed author's original slot zero explicitly roots imported
registry metadata, and registry scroll IDs follow global identity after deployment remapping. The
outbox now requires this full restoration check before reporting local readiness.

Current versions and editions are author decisions. Folding another branch does not overwrite a
pending designation. Explicit `setCurrentVersions()` choices seal as current editions even when
named editions already exist. Metadata preparation happens before the snapshot is signed;
serialization and sealing then read that prepared snapshot. Canonical timestamp text makes repeated
preparation idempotent. Creating a named edition does not invent a separate version annotation.

The Publish form defaults to **Keep all existing editions**. Its edition selector shows names and
current targets, offers repointing one selected edition to the version being published, and offers
adding a named edition. Repointing uses that edition's birth identity, including when multiple
editions share a name. These changes require explicit form choices and do not infer newer targets.

The swarm peer has a diagnostic `--restore-publication HASH HOST PORT CACHE` mode. It fetches the
signed manifest and every dependency through the named BitTorrent peer, using separate directories
for each immutable hash. The namespace runner supplies an opposite-namespace reader, rather than
having the publisher and final reader share a loopback. This diagnostic is not a remote-opening UI
affordance. At the end of that batch, `Session::readPublication()` still adopted the selected EDL.
The reader-opening batch below replaces that path; network catalog ingestion remains pending.

Validation evidence for this batch is under `build/publication-inventory-fixes/`; final outcomes are
recorded in the validation report. Docker smoke filters now include the inventory suite.

## Cached reader opening and offline deployment

`Ctrl+O` accepts a signed `.xanadoc` publication with immutable dependencies cached beside it.
Opening verifies the manifest, all torrent files and pieces, the complete operation history and the
signed inventory before creating a separate reader store under the user's xanadocs directory. It
copies carriers into that reader's `published/<hash>/` cache, re-verifies the copies and stages a
native load before installing the directory. Existing readers and their edits are never overwritten.
Signature or dependency failures produce a diagnostic and create no opened publication.

`Session::readPublication()` and diagnostic `--read` now open that complete store instead of
adopting only its selected EDL into store zero. The reader's own permascroll receives no copied
primedia. It retains author-selected editions and annotations; subsequent reader typing appends only
to the reader's local scroll. The normal Open dialog also reopens the installed native store
offline. This is a cached-opening prerequisite, not author-key following or topic discovery.

Store tables are now **format 4**, with ordered private deployment descriptors and the imported
author's original local-scroll binding. The authoritative registry and editions remain Structure
operations. The loader installs these descriptors and retained carriers before folding registry
names, verifies imported spans, and refuses missing/corrupt cache data. Unsupported table versions 2
and 3 are refused by number. Regenerate owned native stores; the repository's two sample generators
were run in this batch. Operation nodes, wire format and publication format 2 are unchanged.
`xudu-dump --section=scrolls` exposes the deployment slots and root binding without needing a
loader.

Saving a restored publication does not add author registrations for imported deployment descriptors,
change editions or append document operations. Its retained cache belongs to the Store and takes
precedence over a borrowed remote source, avoiding dangling resolver pointers after network
shutdown. The namespace diagnostic now retains downloaded metainfo, installs the full reader and
checks native reopening after its BitTorrent process exits. This remains a diagnostic, not a UI
download affordance.

Telescope no longer creates a text summary labelled as a verified publication when asked to open an
uncached result. It explains the missing fetch affordance and points to cached opening. Initial
verification and cache installation still run synchronously; moving them and signing to a worker is
remaining work.

Validation evidence is under `build/publication-reader-fixes/`. The local publication runner now
also drives cached opening, offline reopening and missing-cache rejection with `Ctrl+O`, capturing
the form, documents and diagnostics. Final counts and limitations appear in the validation report.

## Signed-name download and reader controls (2026-10-05)

`PublicationInbox` resolves a BEP 46 publication magnet, downloads its manifest and signed
content/history dependencies, then installs a separate native reader snapshot. All libtorrent
construction, calls and destruction, piece review and complete-store installation happen on one
worker. Submission does no networking. Each attempt owns a fresh transport and cache; failures and
cancellation remove its output, and an explicit retry resolves the same author/document name again.

A downloaded manifest must have a valid signature and match the requested key, salt and the DHT
pointer's sequence. A remembered immutable hash in the link is not a substitute for DHT resolution.
Default engine budgets cap manifests at 16 MiB, dependencies at 1 GiB and the dependency count at
4,096; the options allow a host to change these limits. Network metadata is obtained in upload mode
before payload is enabled; paths and budget are checked first. Pieces larger than 16 MiB are
refused, and copied read buffers are discarded after verification. The existing complete-store
installer checks all piece hashes, path/length constraints, history and inventory before the reader
becomes ready. No changes to native store format 4 or publication format 2 are needed.

Ctrl+O accepts the publication magnet in Custom path. Discovery-result activation uses the same path
once signed catalog entries exist. The drawn Download publication form exposes refresh, open, retry,
cancel and close. Errors stay in that accessible form. Closing the form leaves the job running; the
Open picker lists requests and completed snapshots. Snapshots live in the user data directory
separately from temporary primary workspaces; repeated opens reuse one mutable store instance.
Completion atomically retains the signed manifest beside the native store, drops the temporary
transfer cache, and exposes an offline snapshot in the picker on subsequent runs. Interrupted/failed
requests are not silently restarted. Opening preserves the publication's selected version and
authored edition choices; it creates no subscription or edition update. Signature checks establish
control of the key; Oracle enrollment and author-key/catalog discovery remain later work.

Validation evidence and remaining interface findings are recorded in
[the download validation report](ux_publication_download_validation_2026-10-05.md). This batch adds
the network/opening prerequisite for P2. It does not mark the original seven user journeys complete.

The full headless test target passed 539 library, 1,249 engine, 57 Xuzz and 119 ZigZag tests, plus
12 namespace transport/mutable-name cases and the publication namespace integration. One optional
video test was skipped and one benchmark remains disabled. After the final cancellation commit-order
fix, the focused inbox/outbox/inventory run passed all 38 cases. The local publication runner
passed, retaining its final evidence in `build/publication-local/run-wtmyh7k4/`. The live
publication namespace/UI integration also passed after that fix. The final Docker image
`gleditor-swarm-test:local` (`8b0375f74c72`) rebuilt the corrected inbox object and passed all 95
smoke tests with networking disabled. Repository formatting and lint gates passed. Final build, test
and image identity logs are retained under `build/publication-download/`.

## Signed author catalogs and topic discovery (2026-10-06)

A publication sent to the test swarm now also updates the author's signed catalog at
`bep46:<key>/catalog`. The catalog has its own durable sequence, independent of each document's
publication sequence. Its versioned canonical bencode lists document salts, immutable roots, titles,
topics and the author's selected microversions. Publishing a different microversion does not infer
an edition change. Completion waits for remote DHT acknowledgement of both the document pointer and
the catalog pointer; immutable catalog seeds remain available after restart.

`Ctrl+Shift+D` opens discovery by topic or publishing key. Telescope accepts Return on a simple
keyword such as `Ideas`, `#Ideas`, or `author:<64-hex-key>`. Author discovery resolves the signed
catalog pointer and fetches its torrent. Topic discovery joins the deterministic
`SHA1("xudu:topic:" + canonical-topic)` rendezvous and exchanges signed catalogs through the
`xudu_publications` BitTorrent extension. No separate service, direct reader peer address or
preloaded search record is needed. Discovery and download sessions stay on their workers.

Readers verify catalog signatures, requested publisher/topic, canonical encoding and observed
sequence high-water marks before ingestion. Forged metadata, conflicting equal-sequence snapshots
and document rollback are refused. Verified snapshots and explicitly followed keys persist privately
across restart. Catalog records are search metadata, not proof of Oracle enrollment, availability or
complete content; these distinctions are visible in the interface. Choosing a result starts the
existing signed-name download and complete-store verification path.

The new wire/catalog format is version 1. Native store format 4 and publication format 2 are
unchanged. The default Docker smoke filters include the catalog and worker regression suites.

Validation and remaining limitations are recorded in
[the discovery report](ux_publication_discovery_validation_2026-10-06.md). This adds author-key and
keyword discovery prerequisites for P2/P3; subscriptions, commentary discovery and link packages
still require their own interface journeys.

The final focused run passed 53 cases; the final namespace run passed 12 transport/mutable-name
cases and both publication integrations. The local publication UI runner passed with evidence in
`build/publication-local/run-9e43ava2/`. The rebuilt Docker image `gleditor-swarm-test:local`
(`cae7ecc8eda4`) passed 104 smoke tests with networking disabled, and its publication sources match
the host tree. Repository format-check and lint passed; clang-format 19 comparison found no new
deviations, while unchanged regions retain the baseline differences described in the report.

## Remaining work

1. Capture an immutable store snapshot and move initial signing/sealing off the rendering command
   path. Dependency review, seeding, pointer announcement, completion/retry, signed topics and the
   explicit mock verification boundary are now implemented in the outbox.
1. Add persisted update subscriptions, sequence polling, dependency verification, retry and
   acknowledgement, including missed updates after reconnect.
1. Add commentary/backlink and independent link-package creation, review, announcement and
   discovery. Package visibility must be a private reader preference that filters contributions
   without appending operations to visited stores.
1. Rerun P1–P7 and their rejection/offline/retry cases through the UI. The first batch's unit, form
   and transport passes do not substitute for that acceptance run.
