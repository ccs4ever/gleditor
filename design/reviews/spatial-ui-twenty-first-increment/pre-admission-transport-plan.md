# Pre-payload transport admission seam

## Current path

`PublicationInbox::download()` in `apps/common/xanadu/publication_inbox.cpp` creates a fresh
`PublicationDownloadTransport` for each attempt. It fetches the signed manifest carrier first,
validates the decoded publication and signature, builds a `std::set<InfoHash>` from all scroll
segments and ops segments, then fetches each unique dependency. The worker passes
`maximumManifestBytes` for the manifest and a decreasing `maximumDependencyBytes` remainder for
dependencies.

The production factory in `publicationDownloadSwarmTransport()` constructs the internal
`DownloadTransport`. Its `fetch()` calls `SwarmContentSource::addMagnet(..., true)`, waits for
metadata, obtains both `metainfo(hash)` and `torrentMetadata(hash)`, checks limits and path safety,
then calls `startDownload(hash)`. This is the right byte-admission seam: at that point the
infohash-validated torrent metadata is present, but the payload download has not been enabled.
`Metainfo::files()` provides every path and exact logical file length; `totalLength()` and
`pieceLength()`/`pieceCount()` provide stream and transfer-layout lengths. The encoded torrent
metadata string is already available as `encoded`, and the implementation writes those exact bytes
to `directory / "metainfo.torrent"` after a successful flush.

The current pre-start checks reject total payload above the passed limit, pieces over 16 MiB, unsafe
or duplicate paths, and a bad piece-count relation. They do not include the retained
`metainfo.torrent` sidecar in that byte limit. They validate per-carrier metadata before download,
but the transport API has no admission callback to reserve the measured bytes against the inbox's
shared request budget.

## Smallest API change

Add a value type to `publication_inbox.hpp` for the measured carrier footprint, containing the
infohash, validated relative file paths and lengths, checked total payload bytes, encoded
`metainfo.torrent` byte length, piece length/count, and a checked `retainedLogicalBytes` sum. The
sum is payload bytes plus `encoded.size()`, with overflow checks; it describes the cache files the
transport itself intends to retain, not filesystem block allocation or unobserved libtorrent
temporary allocations.

Extend `PublicationDownloadTransport::fetch()` with a required admission callback invoked after all
current metadata/path checks and immediately before `startDownload()`. The callback receives the
measured footprint and the request stop token, and returns an explicit accept/refuse result. The
production `DownloadTransport` checks cancellation again immediately before invoking the callback
and again before `startDownload()`. A refusal throws a typed admission error and never calls
`startDownload()` for that carrier. Keep the existing `maximumBytes` argument as a local payload
ceiling unless the owning API deliberately renames it; do not silently change its meaning.

`PublicationInbox::download()` supplies the callback. For the manifest, it admits against the
manifest ceiling before the first payload request. For dependencies, it admits each unique infohash
against the shared decreasing dependency remainder, charging both payload and sidecar logical bytes.
Use checked subtraction (`charge <= remaining`) before decrementing. Do not charge the same hash
more than once: the current `std::set<InfoHash>` is the correct deduplication point. If a carrier is
refused after earlier carriers transferred, retain the attempt and report failed admission with its
exact measured hash/size; the existing explicit partial-data discard path owns cleanup. Never remove
files behind a caller's back.

This change gives a per-carrier guarantee: no payload starts unless that carrier's exact metadata
footprint fits the remaining cap. It does not guarantee that the entire dependency closure fits
before the first dependency payload starts, because the signed manifest contains dependency hashes,
not each dependency's torrent file lengths. A strict whole-closure preflight would need a separate
metadata-only inspect/prepare phase for every unique dependency, retain the prepared
metadata/handles, total the exact footprints, and only then enable any payload. That is a larger
transport lifecycle API and is unnecessary if partial attempts plus explicit discard are the chosen
refusal behavior.

## What metadata can and cannot establish

For the manifest carrier, its magnet's infohash identifies the info dictionary; once the peer has
provided torrent metadata, the exact payload file lengths and exact sidecar length are known before
`startDownload()`. This bounds the logical bytes requested for that carrier. The manifest's own
signed dependency closure is not available until the manifest payload is downloaded, decoded, and
signature-checked.

After that verification, the manifest gives the unique dependency hashes and a unique dependency
count, but not their payload or sidecar lengths. Each dependency's lengths become knowable only
after that dependency's metadata arrives. Therefore, `maximumDependencies` bounds count, and the
shared callback can bound cumulative admitted logical bytes incrementally; neither the count nor the
signed manifest alone proves the total closure size before transfer begins.

`encoded.size()` is available only after libtorrent has received and materialized torrent metadata.
A post-metadata callback can prevent payload allocation, but cannot bound memory/network cost spent
receiving that metadata. If hostile metainfo acquisition itself must be bounded, add a separate
metadata-byte limit at the swarm metadata-receive boundary; do not claim the payload callback covers
it.

Finally, a payload-plus-sidecar limit is not a full install peak limit. `PublicationInbox` retains
the downloaded cache while `installPublication()` builds the staged native store, and removes the
cache only after installation. Any cap intended to bound peak inbox-owned storage must combine the
measured carrier cache footprint with the prepared native-store footprint at that later install
seam. The staging policy plan's separate whole-root accounting should remain authoritative for that
total; the transport callback should report measurements, not duplicate install sizing logic.

## Failure, cancellation, and cleanup boundaries

The worker's stop token is checked while waiting for metadata, between dependency fetches, during
piece reads and around installation. Add the immediate pre-admission/pre-start checks described
above so cancellation cannot slip across an accepted admission and start a new torrent download. If
a started transfer fails or is cancelled, the attempt remains separately owned under
`<request-id>.attempt-<n>`; the UI exposes an explicit discard action. `cleanupPartialData()` walks
only those owned attempts, refuses symlinks and special files, checks cancellation during its
walk/removal, and reports bytes actually removed. It currently checks integer overflow but has no
configured maximum byte ceiling for cleanup work. If “bounded cleanup” is meant as a strict resource
bound, introduce a cleanup-byte cap and refuse before deletion when its verified logical size
exceeds that cap; keep partial files intact on refusal. The admission callback itself should not
create or clean files.

## Validation target

Test the production callback path with the existing real namespace swarm test and real peer, not a
mocked service: confirm a metadata footprint is delivered before `startDownload()`, exact file and
sidecar lengths match bytes written after success, rejection leaves no payload file, and
cancellation before the callback or immediately before start does not enable payload download.
Separately use the existing deterministic transport tests for cumulative unique-hash accounting,
duplicate references, sidecar charging, arithmetic overflow, and refusal preserving earlier partial
bytes. These tests can prove admission logic; they cannot claim real network availability or
whole-closure preflight unless the inspect/prepare lifecycle is actually implemented and exercised.
