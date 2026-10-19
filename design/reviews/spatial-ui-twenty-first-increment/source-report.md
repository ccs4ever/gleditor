# Increment 21 source report

The engine now exposes exact native store file sizes from the same owners used by the writers.
`StoreTables` has one encoder for the existing signature, version, and bencode bytes; both native
save paths use one deployment-table builder. `SegmentedOpsSpool::serializedFileSize()` checks
multiplication, addition, platform file offsets, and write limits, including the full 64 KiB header
for an empty spool. `PublicationSeed::retainedFileBytes()` includes the verified payload and the
retained metainfo sidecar with checked addition.

`Store::nativeFileSizes()` is read-only and refuses pending metadata. For an authored store with
external scrolls, it performs a read-only fold to confirm that save would not add missing registry
operations. Imported publication stores bypass that fold because their published local-scroll
binding prevents save-time registry authorship. The installer measures the initially reviewed seeds,
re-reviews the retained copies and compares their measured lengths, then queries its existing
restored store before save and checks the actual `ops.nodes` and `store.tables` lengths. It does not
restore a second store or change the disk format.

Validation: `make -j$(nproc) xudu_test` built successfully. The focused headless run passed 25/25
tests across `StoreTablesTest`, `SegmentedOpsSpoolTest`, the new `PublicationFootprintTest`, and
selected `PublicationInventoryTest` cases. It used fresh XDG roots under
`build/validation-twenty-first/xdg-*`. `git diff --check` passed. The available formatter was
clang-format 23.1.1; the repository's clang-format 19 binary was not present, so canonical
format/lint gates remain with root.

The measured value is logical file length only. It does not include filesystem allocation,
directories, permascroll flushes, manifest carrier bytes, or metadata-only transport side effects.
No request budget, ordinary network transport, or production service is implemented here.
