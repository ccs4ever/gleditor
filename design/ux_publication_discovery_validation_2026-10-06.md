# Publication discovery validation — 2026-10-06

This batch adds signed author catalogs and topic rendezvous to the publication download path. Bob
can enter Alice's publishing key, while Carl can search the advertised topic `Ideas` without knowing
her key. Both routes expose signed catalog records and let readers select a publication for complete
store download. The seven publication journeys remain an acceptance target: subscriptions,
commentaries/backlinks and independent link-package workflows are still pending.

## Interface evidence

| Journey         | Step                                               | Affordance                                                              | Outcome | Evidence                                                            |
| --------------- | -------------------------------------------------- | ----------------------------------------------------------------------- | ------- | ------------------------------------------------------------------- |
| P2 prerequisite | Discover Alice from an empty profile               | Ctrl+Shift+D, follow-key mode, type Alice's 64-hex key, Start           | Pass    | `build/publication-discovery/network-ui/bob.log`, `bob-catalog.ppm` |
| P2 prerequisite | Download and open the selected publication         | Open selected publication, refresh download, Open completed publication | Pass    | `bob-download.ppm`, `bob-opened.ppm`                                |
| P3 prerequisite | Discover an advertised topic from an empty profile | F3, type Ideas, Return                                                  | Pass    | `carl.log`, `carl-catalog.ppm`                                      |
| P3 prerequisite | Download and open Alice's result                   | Open selected publication, refresh download, Open completed publication | Pass    | `carl-download.ppm`, `carl-opened.ppm`                              |

The namespace fixture supplies the publisher's signed store and explicit mock enrollment. Readers
receive only their stated query and a DHT bootstrap node. The peers use distinct IP addresses even
where a namespace is shared. No publication magnet, immutable root, search record, content cache or
direct BitTorrent peer address is supplied to their interface. All metadata and payload travel
through the DHT/BitTorrent transport. The fixture proves discovery and opening, not UI creation of
Alice's initial mixed document/slice store.

Keyboard automation follows the default bindings and drawn forms. Offscreen OpenGL frames and
internal accessibility dumps were inspected. Native platform assistive-technology integration, other
rendering backends and public-internet connectivity are outside this run.

## Integrity and lifecycle

Catalog format 1 is canonical, signed bencode capped at 512 KiB and 1,024 entries. Each entry names
its document salt, immutable root, title, advertised topics, selected microversion and publication
sequence. The catalog uses an independent durable sequence. Updating it does not choose editions or
infer changes from hypertime order. Its mutable pointer uses the established author `catalog` salt.
Topic targets use the established `SHA1("xudu:topic:" + canonical-topic)` convention.

Catalog ingestion verifies the signature, requested author or advertised topic, and cached
high-water marks. A topic response carries the author's signature directly; no curator identity is
trusted to speak for the author. Equal-sequence conflicts and observed document rollback are
refused. Snapshots and followed keys persist privately; search results are rebuilt from accepted
snapshots. The interface distinguishes signature checks from Oracle enrollment, and checks
content/history and availability during download rather than inventing metrics.

Validation exposed extension-ID overlap with libtorrent's built-in extensions, automatically queued
seeds, missed early DHT announcements and an idle-worker stop-notification race. Custom extensions
now negotiate separate IDs, publication seeds remain active, early rendezvous is retried, and idle
shutdown synchronizes with the wait predicate. Peer addresses learned through DHT remain pending
until the torrent can accept connections. Repeated short mutable-name polls retain their pending
lookup rather than reissuing it each time. Publishers periodically rotate their retained document
and catalog pointer announcements with the same hashes and sequences, so nodes joining after initial
acknowledgement can learn them. The fixture gives its bootstrap, publisher and two readers distinct
private addresses and explicitly permits 100 DHT packets per second per IP; ordinary sessions retain
libtorrent's default allowance of five.
[libtorrent settings](https://www.libtorrent.org/reference-Settings.html#dht_block_ratelimit). Total
connections are capped at 128 and topic swarms at 16 connections each. Bencode decoding also bounds
nesting before signed catalog verification.

Piece verification alone did not ensure the retained files were ready for review while libtorrent
still had pending disk writes. Downloads now await the matching cache-flushed alert before reviewing
or installing those files. The namespace test also checks the retained bytes while the transfer
session is alive. This is a completed-write barrier, not a physical-disk fsync guarantee.

## Regression checks

The final focused run passed 53 catalog, discovery, outbox, inbox, bencode and search-index tests.
The initial full native run passed 539 library, 1,259 engine, 57 Xuzz and 119 ZigZag cases, but its
publication restart integration failed. After the fixes, `make test/swarm` passed all 12
transport/mutable-name cases and both publication integrations, including the fresh-profile UI
journeys above. One optional video test was skipped and one engine benchmark remains disabled. The
final local publication runner passed its signing, edition review, cached opening and offline
controls; evidence is under `build/publication-local/run-9e43ava2/`. Build, focused and network logs
are under `build/publication-discovery/`. Repository format-check and lint passed with the installed
tools. A separate clang-format 19 comparison found no new deviations in changed/new code; five
touched files retain identical pre-existing differences in unchanged regions. The comparison and
diagnostics are retained beside the gate logs.

The rebuilt `gleditor-swarm-test:local` image (`cae7ecc8eda4`) passed all 104 default smoke tests
both during the build and with Docker networking disabled. Fourteen publication source hashes match
the host tree, and the image audit found no generated publication-state or private identity
directories. The build reuses the existing local image as a dependency layer and runs the tracked
recipe's Make/build/test tail. Live transport acceptance used native rootless namespaces rather than
Docker containers. Image identity, source hashes and offline smoke output are retained with the
logs.

## Remaining findings

- A restarted publisher used as the sole bootstrap sometimes returned no document pointer to a fresh
  reader. Reader fixtures use the designated stable DHT bootstrap node; arbitrary ephemeral
  bootstrap recovery remains a finding. No direct BitTorrent peer is supplied to those readers.

- Followed keys are persisted preferences, not update subscriptions. Refresh requires an explicit
  discovery request; missed-update recovery and notifications still need implementation.

- Keyword discovery matches advertised canonical topics. It does not perform unrestricted full-text
  searches of the DHT. A publisher must advertise `ideas` for this topic journey.

- Topic exchange currently requires a reachable publisher. Curator relay, independent link-package
  advertisements and revocation/enrollment rules remain later work.

- Publication signing/sealing still starts on the rendering command path. Discovery, downloads,
  verification and network session ownership run on workers.

- Reader-tab display labels, long form-input layout and safe reclamation after crashes retain the
  findings from the preceding download report.

- The original P1–P7 acceptance run, including slice traversal, commentary discovery, edition
  review, subscriptions and package visibility toggles, is not complete.
