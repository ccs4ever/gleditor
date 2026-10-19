# Exact publication-install sizing audit

## Scope and result

This is a read-only Code Quality Auditor review of `installPublication()`, `Store::save()`,
`writeStoreTables()`, the operation spool writer, existing publication-seed accounting, and the
metadata-only swarm path. No product changes or builds were made. The root agent is synthesizing
this auditor review with the concurrently completed Architectural Synthesizer and Systems Profiler
reviews.

The writer ownership is already clean: `SegmentedOpsSpool::writeSegmentFile()` owns `ops.nodes`,
`writeStoreTables()` owns `store.tables`, and `Store::save()` composes those writers. Exact sizing
should extend those owners with shared measurement paths rather than reimplementing file formats in
`installPublication()` or the inbox.

## Code Quality Auditor

- `PublicationSeed::bytes` is `Metainfo::totalLength()`: the summed payload file lengths only.
  `retainPublicationSeed()` also writes a separate `metainfo.torrent` containing `seed.metainfo`.
  Any retained-copy size query that adds only `seed.bytes` undercounts the copy by
  `seed.metainfo.size()` per seed. The same distinction exists in the download path: `fetch()`
  checks `meta.totalLength()` against its payload limit, while `torrentMetadata()` is only checked
  for presence and not surfaced as a measured byte count.
- `writeStoreTables()` currently assembles and bencodes the table dictionary inline, then writes a
  12-byte signature, a 32-bit version, and the encoded body. There is no pure encoder or size API
  shared with the writer.
- `Store::save()` currently assembles deployment segments and the `StoreTables` value inline, then
  delegates the file to `writeStoreTables()`. A second reconstruction of that value in a sizing
  function would drift from the writer.
- `SegmentedOpsSpool::writeSegmentFile()` already owns the exact node header and node-byte write.
  Its logical file size is the 64 KiB header plus `opCount * sizeof(CompactOpNode)`, but that size
  is not exposed through the spool API. Keep the multiplication checked and in the spool owner.
- `Store::save()` first calls `sealPendingMetadataAsCells()`. That method may append the structure
  operations needed by the existing save contract. A query over `opCount()` before that preparation
  can be smaller than the file `save()` writes. The size query must not invoke save, manufacture
  operations, or run a second restore/replay merely to estimate output.
- `installPublication()` writes a new `store.partial/`, retains every seed, re-verifies the copies,
  restores one `Store`, saves it, and verifies an offline load before renaming. For this fresh
  destination its install files are the `published/` dependency tree, `ops.nodes`, and
  `store.tables`; `publication.xanadoc` is separately retained by the inbox in the request attempt
  root.

## Architectural Synthesizer

The smallest shared design is a prepared install object whose exact inputs are also the inputs
consumed by the writer:

1. Review the manifest and dependency seeds once. Keep the deduplicated, verified `PublicationSeed`
   values and the restored `Store` in a move-only `PreparedPublicationInstall` (or equivalent
   private owner-level value). Do not restore the same history a second time for sizing.
1. Factor `StoreTables` construction into one `Store` helper used by both `Store::save()` and a
   const footprint query. Factor table encoding into one canonical encoder used by
   `writeStoreTables()` and its byte-size query; the query should include the format prelude so it
   exactly matches the file.
1. Add a checked logical-size query to `SegmentedOpsSpool`, computed from its own `opCount` and
   header constants. Have `writeSegmentFile()` consume the same size result when it establishes the
   final file extent. No op nodes are appended by this query.
1. Expose a named `PublicationInstallSize` with separate `retainedDependency` and `nativeStore`
   totals (and per-file totals useful for diagnosis). The retained dependency total is the checked
   sum of each unique seed's payload bytes plus its metainfo sidecar bytes. The native total is
   exactly the spool's `ops.nodes` extent plus the canonical encoded `store.tables` size. Keep the
   inbox's retained manifest-carrier bytes separate because they are not part of
   `installPublication()`'s destination.
1. Make the installer consume that same prepared value after admission; the size query itself is
   const and does not write files or mint operations.

There is one preparation boundary to settle explicitly: `Store::save()` may seal pending metadata
into operations before writing. A prepared install must apply the existing save preparation once,
then measure and save that same resulting store. If the requirement is that a sizing call itself
never changes the store, expose the footprint only after that canonical preparation and refuse to
size an unprepared store. Do not clone-and-seal a second store or estimate from operation counts
that precede save preparation.

All arithmetic should be checked `uint64_t` additions/multiplications. The query should report a
typed refusal if an extent cannot be represented by the platform's file-offset type. The query must
not create directories, follow untrusted source paths, trust the manifest's lengths without
`reviewPublicationSeed()`, or treat existing destination files as reusable. Retained dependencies
are copied to a new staging tree, so the size describes the resulting logical files, even if a
separate source cache already exists. Directory allocation, filesystem metadata, and caller-owned
permascroll flushes are outside the logical file-byte total and must be named as such.

## Systems Profiler and metadata-path review

For each publication, operation segment descriptors carry operation counts in
`ScrollSegment::length`; the local spool writer has a fixed 64 KiB header and fixed-size
`CompactOpNode`s. This permits an exact native ops-file byte count without reading or creating
operation nodes for the measurement. Use sample publications and the existing unit fixture to
compare query output with the actual three output classes: retained payload plus metainfo sidecars,
`ops.nodes`, and `store.tables`. Include zero-op edge handling even though a complete publication
restore currently requires history segments.

The direct transport's `fetch()` creates the cache directory before calling
`SwarmContentSource::addMagnet()`. In metadata-only mode, `addMagnet()` sets `upload_mode`, clears
`auto_managed` and `paused`, and returns with no parsed metainfo. `startDownload()` unsets
`upload_mode` only after `fetch()` receives metainfo and checks payload length, piece length, safe
paths, and piece count. This code supports the claim that payload download is gated until those
checks. `MetadataOnlyMagnetWaitsForExplicitDownloadAndCanDiscardPieces` also checks that
`readStream()` is empty before `startDownload()` and reads the payload afterward. Neither this test
nor the wrapper checks the target directory before `startDownload()`, so they do not prove that
libtorrent creates no empty or nonzero files while metadata arrives. That needs a filesystem
observation test against the real metadata-only path. No claim about the full fetch budget should
rely on the wrapper code alone.

`torrentMetadata()` is available after metadata arrives, but `fetch()` only checks that its string
exists. The retained seed API later reads `metainfo.torrent`, and the install copy writes it as a
sidecar. Include the encoded metainfo byte length in exact retained-copy sizing. A later admission
slice must also bound or measure that metadata before disk writes and establish whether
metadata-only addMagnet leaves any files behind before payload start.

The audit found no need for a throughput benchmark or a copy-factor guess for this sizing
prerequisite. Evidence should be writer/query equality on actual staged file lengths and
checked-overflow tests. This query alone does not enforce a cumulative cache/install budget;
admission, metainfo preflight, and the libtorrent pre-start filesystem observation remain separate
prerequisites.

## Review synthesis

The code-quality view says exact facts are missing at the current owners and metainfo is currently
omitted from seed byte accounting. The architecture view recommends a single prepared publication
install feeding both a read-only footprint query and the unchanged canonical writers. The systems
view confirms the fixed-size operation spool makes `ops.nodes` exact without speculative operations,
while `store.tables` must use its actual canonical bencode encoder. The remaining risks are
save-time metadata sealing and metadata-only libtorrent filesystem effects; both need explicit tests
or a strict prepared state boundary before the query is used for admission.
