# Exact publication-install footprint implementation

## Scope

Expose exact logical file sizes for the native store and retained publication dependencies at their
existing engine owners. Use these measurements inside the existing publication installer and verify
them against the files it already writes. This does not add admission limits, settings, network
behavior, a new on-disk shape, or a second restore/replay.

## Shared native writers

`StoreTables` will have one canonical encoder that returns the existing signature, version, and
bencoded body. `writeStoreTables()` will write those bytes unchanged, while the size query will use
the same encoded vector. Store will build the deployment table value in one private helper used by
both native save paths and the size query.

`SegmentedOpsSpool` will expose the checked logical file extent of its current nodes. The result is
the fixed 65,536-byte segment header plus the node count times `sizeof(CompactOpNode)`, with checked
multiplication/addition and a platform file-offset representability check. The spool writer will use
that same extent when establishing the file length. The empty spool remains a 65,536-byte file.

`Store::nativeFileSizes()` will be read-only. It will refuse if `sealPendingMetadataAsCells()` could
append Structure operations, including pending annotations, explicit edition selection, or
unregistered external scrolls for an authored store. It will not flush the permascroll, create a
directory, or otherwise prepare the store. The publication installer calls it on the
already-restored store before `save()`; if the guard refuses, install stops instead of estimating a
smaller pre-seal size.

## Retained dependencies and reconciliation

Each already-reviewed `PublicationSeed` will report checked logical bytes for its payload (`bytes`)
and the `metainfo.torrent` sidecar (`metainfo.size()`). The dependency review already deduplicates
hashes and verifies payloads; no filesystem walk or trust in unsigned lengths is added.

Before the existing save, `installPublication()` will retain the seed sizes and query the current
native sizes from its existing restored `Store`. After save and offline load, it will compare the
native files with the prediction and compare the retained `published/` logical file lengths with the
verified seed total. A mismatch refuses installation before the staging directory is renamed. This
checks the exact writer outputs without restoring the document again.

The reported measure is logical regular-file length. It excludes directory entries, filesystem
allocation overhead, caller permascroll flushes, the separate signed manifest carrier, and any
temporary duplicate cache used by a future download path. The metadata-only transport still creates
its cache directory before metainfo arrives; this increment makes no claim about those filesystem
effects or about a whole-request staging cap.

## Tests

- Canonical `StoreTables` encoded length equals the file length written by the existing writer,
  including the prelude; retain the existing round-trip and version checks.
- Spool predicted size equals actual file length with zero and multiple operations, including
  checked-overflow refusal where constructible.
- A nontrivial publication install reports exact retained and native file totals matching actual
  staged files, including metainfo sidecars.
- A store with pending metadata refuses the pure size query and remains unchanged; a stable restored
  store query also leaves operation count and permascroll state untouched.

No fixture format or signed bytes change. The sampled publication profiles are measurements, not a
default budget or representative maximum.
