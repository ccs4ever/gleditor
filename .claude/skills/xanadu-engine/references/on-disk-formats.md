# On-disk formats: bump the version, don't carry a shim

Nothing is in production and every store regenerates from its inputs, so **a format may change
shape: bump the version, write the new shape, delete the old reader.** Non-negotiable are the
invariants: `sizeof(CompactOpNode) == 64` and its alignment, 64 KiB Merkle pieces, append-only-ness.
This ruling expires the first time someone outside this repository has a document they care about
(R11 in [`design/store-slice-convergence.md`](../../../../design/store-slice-convergence.md)).

A store directory is exactly:

| file           | shape                                                                                                             |
| -------------- | ----------------------------------------------------------------------------------------------------------------- |
| `ops.nodes`    | `OpsSegmentHeader` (64 KiB, sparse, records `nodeSize`) then a run of `CompactOpNode`                             |
| `store.tables` | `\x89XUDUTBL` + version 4 + bencode: local segments, deployment scroll descriptors, imported author-local binding |
| `ops.export`   | only with `--export-osmic`: the operations as OSMIC text or the compact binary wire format (`CompactBinaryV4`)    |

**A store holds no primedia**, so it is not portable alone: it needs the permascroll it was written
against (`$XDG_DATA_HOME/xuzz/permascroll/<key>/active.primedia` or `--permascroll`). The retired
files — `primedia.spool`, `ops.spool`, `current.yaml`, `versions.yaml`, `scrolls.spool`,
`links.spool`, `origins.spool` — are **refused by name**, because loading one against the wrong
permascroll renders a document that is not the document. `ops.spool` now names only the in-memory
`SegmentedOpsSpool`; nothing on disk may use it.

Rules:

- **Refuse loudly, by number.** Each format checks a signature and version and throws a typed
  exception naming what was wrong. A reader that returns empty for a file it did not understand is
  the failure all of this exists to prevent. A new format does the same.
- **Regenerate every fixture in the same commit** as a layout change (see
  [`build-and-test`](../../build-and-test/SKILL.md)).
- **One writer.** `SegmentedOpsSpool::writeSegmentFile()` alone produces `ops.nodes`;
  `Store::save()` calls it.
- **Diff meaning, not bytes.** Before and after a format change, diff
  `store-dump --section=ops <store>` (or `xudu-dump` symlink; add `--permascroll=<dir>` to render
  text); `--section=header` is where a version bump shows.
- **System xanadocs are the one exception to "refused means refused":**
  `Session::systemStoreIndex()` moves an unreadable one aside and writes a default, catching every
  typed refusal *by name* — add a format, add it there, or an old config stops the program starting.
  A user's document is never treated that way.
