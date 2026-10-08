# Publication journeys in a temporary local swarm

## Purpose and scope

Alice, Bob, Carl and Devin must be able to publish, discover, quote, revise and curate a shared work
through Xuzz. These seven journeys specify the acceptance contract; they are not a report that the
current build passes. Validate them with
[`ux-validation`](../.claude/skills/ux-validation/SKILL.md), alongside the creation, transclusion
and navigation journeys in [real work](ux_workflow_real_work.md).

Only identity enrollment and Oracle verification are mocked. Torrent transfer, mutable DHT
publication and resolution, signatures, global address translation, history reconstruction,
discovery and UI actions must be real. A local file exchange, a preloaded search result or an API
call completing a missing user action is diagnostic evidence, never a journey pass.

Full user creation, login, Oracle election, email verification, delegation renewal, key recovery and
revocation UI remain future work. These journeys do not certify those processes.

## Local swarm and identity fixture

The runner provisions four isolated user profiles and at least two local DHT bootstrap nodes. Use
separate processes and network namespaces on a private bridge with no route to the public internet.
Disable public bootstrap routers, trackers, UPnP/NAT-PMP and local discovery. Introduce only the
local bootstrap endpoints with `SwarmContentSource::addDhtNode()`. Enable the actual DHT and peer
transfer. Record actual listen addresses and ports rather than guessing startup delays. The existing
[two-peer runner](../tools/swarm-netns-test.sh) is a starting point, not a runner for these
four-user journeys. Namespace or veth unavailability is a blocked run, never a pass.

Each profile has its own temporary `GNUPGHOME`, XDG data/config directories, download cache, catalog
index, publication sequence state and permascroll. Generate disposable OpenPGP signing keys and
distinct Ed25519 publishing/device keys. Retain public keys and fingerprints in the evidence; keep
private keys inside the fixture. Restarting a user reuses that user's keys. The bootstrap nodes
carry no user's private keys and provide no preloaded publication metadata.

Run graphical clients with `SDL_VIDEODRIVER=offscreen`, `SDL_AUDIODRIVER=dummy` and
`LIBGL_ALWAYS_SOFTWARE=1`, and profile-specific `XDG_DATA_HOME`/`XDG_CONFIG_HOME`. Provisioning
profiles, keys and network endpoints is fixture setup. Creating content, choosing publication
metadata, following authors, searching, subscribing and toggling packages are user steps.

The mock verifier returns an explicit fixture attestation binding a display name, OpenPGP
fingerprint and device public key to this run. Support approved, pending and rejected outcomes.
Display **Test verification — mock Oracle**; never a production verified badge. Mock approval must
not bypass OpenPGP provenance verification, Ed25519 manifest/DHT signatures or piece hashes.
Pending/rejected enrollment must prevent announcing new publications and explain the state. Approval
must allow retry without creating another identity or duplicate publication.

Record the replacement boundary: an enrollment request and its identity binding enter the verifier;
an attestation or refusal comes back. Later election, quorum, verification, expiry and revocation
replace this fixture boundary. They must not be simulated by calling an unrelated catalog API with
`verified=true`.

## Publication identities and discovery conventions

Use these fixture names consistently; labels below are chosen test data, not hardcoded product
defaults:

| Object                         | Stable salt or rendezvous                     | Owner                      |
| ------------------------------ | --------------------------------------------- | -------------------------- |
| Author catalog                 | `catalog`                                     | Each user's publishing key |
| Story Ideas store publication  | `doc:story-ideas`                             | Alice                      |
| Imported local research scroll | `scroll:story-research`                       | Alice                      |
| Main user permascroll          | `permascroll`                                 | Each user's device key     |
| Bob's Commentary               | `doc:bobs-commentary`                         | Bob                        |
| Carl's Commentary              | `doc:carls-commentary`                        | Carl                       |
| Devin's link package           | `curations:story-ideas`                       | Devin                      |
| Topic discovery                | `SHA1("xudu:topic:" + canonicalize("Ideas"))` | Shared rendezvous          |
| Links touching a scroll        | `linkPackageRendezvousTarget(scrollKey)`      | Shared rendezvous          |

The topic convention and `catalog` salt come from
[topic ledgers and search](swarm-topic-ledger-and-search.md). The older
[BEP 46 design](bep46-publication-and-discovery.md) also suggests empty/`index` catalog salts; this
contract chooses `catalog` consistently rather than silently querying several names. A remote
author-catalog publishing/ingestion path still needs validation. Per-device permascroll salts must
come from `UserPermascroll::currentScroll()`, including its device suffix when present.

The DHT locates mutable pointers and rendezvous peers; it is not a substring search engine. Carl's
query joins the canonical `ideas` topic swarm, receives signed metadata over the swarm, and searches
the ingested metadata with the local catalog index. Alice explicitly tags her work `Ideas`; this
contract does not assume arbitrary title words are automatically announced as tags. Results must
originate from those received announcements, with a verifiable publisher and fetchable publication,
rather than `SwarmCatalog`'s demonstration entries.

Link-package discovery uses the implemented `SHA1("xanalinks:" + scrollKey)` convention, not a new
document-title namespace. Announce for every referenced global scroll key. Discovery of a package
still requires exchanging its signed metadata and fetching it; a DHT peer list alone does not
contain the package. Use the same rendezvous mechanism for commentary backlink packages in P4, so
Alice need not already know Bob's or Carl's key.

## P1. Alice publishes a store and all its local dependencies

1. Alice opens her provisioned profile. The identity panel shows her public identity and mock
   verification state. She creates one store containing a xanadoc and a slice through the UI.
1. In the xanadoc she types “A city remembers every story told within its walls.” She imports a
   small local research text through a file chooser and transcludes two passages from that scroll.
   The runner may supply this input file; it must not supply the finished store.
1. She creates a slice with three idea cells and a `d.next` dimension, links the cells, edits one
   cell and splices another. One cell transcludes her xanadoc; another quotes the research scroll.
   Include a branch and a non-default current version to expose history/address remapping errors.
1. She selects **Publish**, names the work **Story Ideas**, tags it `Ideas`, and reviews the store,
   local research scroll and permascroll dependencies and their publication extent. A dependency
   with no global identity must be listed for publication, not silently omitted.
1. She confirms. The client seals and seeds the dependencies and store history, signs manifests and
   announces their mutable pointers. Only then does it announce the catalog/topic entry. The review
   must make any permascroll bytes outside the selected work visible before approval.
1. She sees the stable sharing name, published version, sequence and a retrievable state. She closes
   and reopens the profile and sees the same identity and publication state.

Acceptance: an empty-cache reader can retrieve the xanadoc **and** slice without Alice's filesystem
or local permascroll. This is a store publication, not just a flattened text EDL. Dependency-first
ordering means a visible entry never intentionally points to an unseeded local dependency; this is
not an atomic multi-key DHT transaction. A failed dependency leaves publication incomplete, with a
retry action that preserves keys, offsets and previously completed seals.

Evidence: publication review frames/accessibility; a dependency graph with global keys, hashes,
sequences, lengths and piece lists; signed provenance; DHT observations from another node; native
store dumps before publication and after remote reconstruction. Run the format checks below.

## P2. Bob retrieves Alice's latest publication by her key

1. Bob starts with an empty cache and only Alice's public publishing key, provided as fixture input.
   He adds/follows the author through the UI; no infohash or manifest file is supplied.
1. He browses Alice's signed `catalog`, chooses **Story Ideas**, and opens it. The client resolves
   the document's mutable name through the local DHT and downloads its dependencies.
1. He reads the xanadoc, follows its transclusions and opens the slice. He checks the edited/spliced
   cells and traverses `d.next` in both directions.
1. He selects **Notify me of updates**, closes and reopens his profile, and resumes reading.

Acceptance: the pinned key, catalog signature and selected publication agree. Publisher history and
reader working history remain separate. Alice's primedia is external to Bob's permascroll;
downloaded Alice bytes are not appended as Bob's typing. The latest *observed* sequence is shown;
timeouts or missing seeders report unavailability instead of empty content or false freshness.

Evidence: author-following and subscription controls; DHT key/salt/sequence transcript; transfer
logs and hashes; remote text/topology comparison; Bob's permascroll size before and after reading.

## P3. Carl finds Story Ideas without knowing Alice

1. Carl starts with no followed authors, no Alice key, no publication cache and no indexed fixture
   entries. He opens swarm search and enters **Ideas**.
1. The client discovers topic peers and ingests their signed announcements. **Story Ideas** appears
   with its title, publisher identity, `Ideas` tag, verification status and availability.
1. Carl opens the result, verifies the retrieved publication, reads the xanadoc and slice, and
   enables update notifications through the UI.

Acceptance: Carl learns Alice's key from authenticated publication metadata. The result must arrive
through topic discovery, not Bob's cache, an injected catalog entry or a shared directory. Case
normalization must find the same rendezvous. An unrelated topic must not produce the result before
metadata has been learned through another declared route. A forged title/publisher claim cannot
become a verified result merely because it contains “Ideas”.

Evidence: Carl's empty initial index, topic target and discovered peers, received announcement,
signature outcome, indexed record and search/open frames. Repeat once after clearing only Carl's
discovery cache to prove acquisition rather than cached success.

## P4. Bob and Carl publish independently authored commentaries

1. Bob creates a new store titled **Bob's Commentary**. He transcludes at least three nonadjacent
   Alice passages, including research-scroll content and a slice cell, and writes his own response.
   He adds links connecting his commentary to the quoted passages.
1. Carl independently creates **Carl's Commentary** with different quotations and links. Both pin
   their citations to the Alice version they read, retaining global scroll offsets.
1. Each publishes through the UI. Their own new primedia is sealed under their own identities;
   Alice's dependencies are referenced with her original global keys and immutable segments.
1. Each publishes a signed backlink package for those commentary links and announces it at the
   rendezvous targets for the Alice scrolls it touches. Their catalogs list the commentary and
   backlink package; they also tag their commentaries `Ideas`.

Acceptance: quotations retain shared primedia identities; byte equality alone is insufficient.
Neither user signs as Alice or republishes Alice content as their own primedia. Turning Alice's
client off after another peer has seeded her dependency closure must still allow retrieval. The
commentary publications and backlinks remain independently fetchable and attributable.

Evidence: ownership/dependency graphs, global quotation tuples and localized spans, commentary
frames, package signatures, rendezvous announcements and a fresh-reader download of each work.

## P5. Alice discovers responses and publishes a new version

1. Alice opens **Links and responses** for her published work. The client queries the rendezvous
   targets of the global scrolls actually used by the work and fetches Bob's and Carl's packages.
1. She sees each curator/publisher, opens both commentaries and follows their references back to her
   text and slice. Discovery does not imply enabling every third-party link layer.
1. She edits her xanadoc, adds a slice idea in response, and transcludes a passage from each
   commentary with explicit response links. She republishes **Story Ideas** under the same key and
   `doc:story-ideas` salt after reviewing the new dependencies.
1. Bob and Carl receive one visible update notice for the new sequence, with author, title and
   old/new versions. They open the update and can compare it with their pinned earlier version.
1. Repeat with Carl offline during publication. On reconnect he receives the missed update once;
   restarting afterward does not notify him again for the same sequence.

Acceptance: Bob's and Carl's quotations of Alice's first edition stay stable. Alice's responses
retain Bob/Carl provenance. Previously sealed segments remain addressable and new primedia is
append-only. A durable per-name counter increases even for two republishes in one second and across
restart; wall-clock seconds alone do not satisfy this requirement.

Updates use periodic `resolveMutable()` polling and comparison with the subscriber's persisted
highest accepted sequence. The DHT does not push application notifications. Live scroll-sealed
broadcasts are not evidence of a new store publication. An update is offered only after its
manifest/dependency closure has been checked; a failed fetch remains pending with retry. Older or
equal sequences, duplicate responses and signatures from another key create no new notice.

Evidence: discovery transcript, before/after stable names and counters, preserved segment hashes,
new history/topology, notification frames, poll timestamps and persisted acknowledgement state.

## P6. Devin publishes an independent link package

1. Devin discovers **Story Ideas**, reads its versions and creates a package through the UI named
   **Connections for Story Ideas**. He selects at least two links with multiple span endpoints,
   connecting Alice passages and a commentary passage. Include an unchanged first-edition span.
1. He reviews endpoint provenance, referenced scrolls, curator identity and target publications,
   then publishes under `curations:story-ideas`.
1. The client signs and seeds the package, publishes its mutable pointer, records it in Devin's
   catalog and announces it at every touched scroll's `xanalinks:` rendezvous.

Acceptance: Devin needs neither Alice's private key nor write access to her store. The signed
package is independently retrievable; all ends resolve to global spans and declared scrolls. Any
unsupported cell/topology endpoint must be reported as a missing feature, not flattened silently
into a different link. Publishing changes no Alice/Bob/Carl document or slice operations.

Evidence: package editor/review frames, decoded verified package with both endsets, DHT and
rendezvous observations, and unchanged source-store operation dumps.

## P7. Readers discover and toggle Devin's package

1. Alice, Bob and Carl reopen **Story Ideas**. Each sees an available package with Devin's identity,
   title, verification state, sequence and link count without already following Devin.
1. Each previews its endpoints and enables it through a named **Show package links** control. Links
   appear wherever referenced content is visible, including quotations in commentaries.
1. They follow a many-to-many link, preserving the link identity and both endsets while exploring.
   They disable the package; its links disappear while other enabled packages remain visible.
1. They enable/disable it repeatedly, close/reopen and confirm their per-reader preference persists.
   Repeat the control and link navigation using the keyboard and accessibility tree.

Acceptance: layer toggles create no operations in visited source stores, duplicate links or copied
primedia. Preferences belong to private reader configuration. The publication and curator remain
inspectable when disabled. A package with an unavailable endpoint reports a partial/unavailable end
instead of forging a location; a tampered package cannot be enabled as verified.
`adoptLinkPackage()` by itself is not evidence of reversible layer activation: toggling requires
tracking package identity and filtering its contributed links without deleting other layers.

Evidence: frames/accessibility before and after toggles; package/link identities and active counts;
source operation counts/dumps unchanged; persisted preference after reopening; keyboard transcript.

## Format and globalization checks required by P1, P2 and P5

Native storage and the publication wire encoding are different products. Check all of them:

- Native store: `ops.nodes` has 64-byte, cache-line-aligned `CompactOpNode` records and a matching
  header node size; `store.tables` is version 3. The store contains no primedia. Do not package a
  bare native directory and call it portable or resurrect retired sidecar files.
- Sealed history: `sealableOps()` carries its `XSO` version-1 envelope, scroll remapping table and
  **CompactBinaryV5** records. V4 is obsolete. Contexts, Structure subjects, dimensions, targets and
  OpHandle values must be localized by microversion identities, not copied local indices.
- Store completeness: reconstruct every advertised xanadoc/slice and designated current version;
  compare titles, branches, annotations, text, cell values and topology. If publication lacks the
  required side-table metadata or structure inventory, report that omission. `adopt()`'s text EDL
  alone cannot prove whole-store reconstruction.
- Dependency closure: include scrolls used by historical operations, cells and link endpoints, not
  only the selected xanadoc's visible pieces. An unglobalized local scroll must block the final
  announcement with a named error. After sealing, every referenced key must resolve.
- Address invariance: compare `(global scroll key, start, length)` before/after localization and
  republication. Local numeric `ScrollId` and `CellRef` may differ on the reader. Source chains and
  topology must still mean the same thing. Verify external content never binds to reader slot zero.
- Incremental seals: reconstruct from all segments in order, then repeat after restart. Old hashes
  and offsets remain stable. Test a missing segment, truncated seal, obsolete binary version,
  invalid signature and wrong-key pointer; none may silently yield a successful empty document.
- Fetch realism: repeat reconstruction from an empty remote cache using actual piece downloads.
  `setExternalLiveBytes()` or direct publisher byte injection proves only a local round trip.

Use `xudu-dump --section=ops --permascroll=<dir> <store>` to compare meaning and `--section=header`
to record versions. Keep decoded manifests and a structure comparison as additional evidence.

## Current implementation evidence and work still to validate

This inventory comes from source inspection, not an executed swarm/UX validation:

| Area                 | Existing implementation or tests                                                                      | Remaining acceptance boundary                                         |
| -------------------- | ----------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| Sealing/global spans | `publication.cpp`, `tests/xudu/publication.cpp`                                                       | Actual remote dependency closure and local-scroll orchestration       |
| V5 Structure/history | `BinaryOpsTest.aPublishedSliceFoldsWithItsLinksIntact`, `sealedHistoryUsesTheSameAddressLocalization` | Whole-store metadata and network reconstruction                       |
| UI publishing        | `Session::publishDocument()` seals history and writes a signed `.xanadoc` locally                     | Seeder/DHT/catalog/topic announcement and durable sequence allocation |
| Core convenience API | `xanadu::publishDocument()` seals user primedia and calls `publish()` without ops segments            | Must not substitute for complete store/history publication            |
| Mutable names        | `publishMutable()`, `resolveMutable()`, existing two-peer runner                                      | Four-user catalog discovery, subscriptions and reconnect notices      |
| Search               | `SwarmCatalogIndex`, `SwarmCatalog`                                                                   | Real signed swarm ingestion; constructor seeds demonstration entries  |
| Packages             | `publishLinkPackage()`, `adoptLinkPackage()`, `linkPackageRendezvousTarget()`                         | Network advertisement/discovery and reversible UI layer controls      |

Before an implementation run, initialize submodules, build with `make -j$(nproc)`, and run the
relevant existing publication, binary-ops, mutable-name and package tests headless. Existing unit
passes are prerequisites, not substitutes for P1–P7. Fixes and validation reports are separate
changes from this journey contract.

## Run report and completion rule

For every step record the actual menu/control/default binding, outcome and evidence path:

| Journey | Step                  | Affordance                  | Outcome | Evidence                   |
| ------- | --------------------- | --------------------------- | ------- | -------------------------- |
| P1–P7   | One row per user step | Observed control or binding | Not run | Run-relative artifact path |

Use Pass, Fail, No affordance, Missing, Harness gap or Blocked; never infer a pass from source.
Include the build revision, public identity bindings, mock-verifier outcomes, network topology,
poll/fetch deadlines and per-user evidence directories. Missing enrollment UI is the declared
fixture exception; missing publication/discovery controls are findings. Mark unsupported network
setup as Blocked and retain logs. Keep artifacts on failure and stop fixture processes on exit;
remove temporary private keys when the run is finished.

The contract is complete only when all seven journeys pass through user affordances, their remote
downloads and format comparisons pass, and the rejection/retry/offline cases above pass. This
document currently makes no such claim.

## Author edition choices at publication

Current versions and editions are authorial decisions made as the store evolves. The Publish form
must expose their names and targets for review, keep them unchanged by default, and permit explicit
creation or repointing before signing. Reading another branch, constructing the inventory or finding
a higher microversion does not authorize a designation change. Multiple editions with the same name
remain independently selectable by their birth identities.

## Cached publication opening prerequisite

The normal Open dialog (`Ctrl+O`) accepts a signed `.xanadoc` with immutable torrent dependencies
cached beside it. It opens a separate complete reader store, retains author editions and can reopen
that native store through the same dialog after the publisher goes offline. Missing/corrupt carriers
and bad signatures must produce a diagnostic without creating a reader publication. The reader's
permascroll receives only their own later typing, never a copy of the author's primedia. This does
not satisfy P2 or P3 until following/discovery and downloading are reachable through their UI.
