# Increment 21 independent final source review

## Finding

I found no blocking correctness issue in the reviewed footprint implementation. The first slice
matches the stated scope: it measures native-store files and retained dependencies at their existing
owners, then checks the installer’s existing writes. It does not claim a whole-request staging
budget or pre-payload admission.

`Store::nativeFileSizes()` is observational with respect to authored operations in the reviewed
code. It rejects pending metadata before returning a result and separately detects the save-time
authored external-scroll registration case. It does not call `sealMetadata()`, `save()`, or
`UserPermascroll::flush()`. `Store::save()` still performs the deliberate metadata seal, and both
save paths and the size query share `deploymentTablesForSave()`.

The `store.tables` estimate uses `encodeStoreTables()`, and `writeStoreTables()` writes that same
encoded string. The operation-file extent includes the full 65,536-byte header region and checked
node extent. `writeSegmentFile()` consumes that extent and refuses overflow beyond `off_t` or
`ssize_t` limits. The installer measures retained seed payload plus metainfo, checks the copied seed
measurements again, queries the already-restored store without a second replay, and verifies the
actual `ops.nodes` and `store.tables` lengths after save before publishing the root.

## Remaining limits and test coverage

- This increment measures and reconciles the native install output after dependencies have been
  fetched. It does not provide pre-payload admission, a configured aggregate budget, or an exact
  whole-peak reservation. The separate signed install projection and transport preflight remain
  prerequisites for that later claim.
- The estimate is logical file length. It excludes filesystem allocation overhead, the author’s
  permascroll flush, the signed manifest carrier, and duplicate download-cache bytes, as the plan
  specifies.
- The added tests cover pending-metadata refusal without operation-count change, imported registry
  sealing, empty and populated spool lengths, canonical table output length, and installed retained
  and native file totals. I did not run tests. The new tests do not directly snapshot stable
  permascroll bytes around repeated `nativeFileSizes()` calls; source inspection shows no
  permascroll mutation or flush in that method.
- `encodeStoreTables()` returns an in-memory `std::string`, while `writeStoreTables()` casts its
  length to `std::streamsize` for output. The ordinary supported build targets have `size_t` and
  `streamsize` ranges sufficient for practical table files, but this cast is not separately guarded
  for an object larger than `streamsize::max()` on an unusual platform. This is a portability edge,
  not a blocker for the current bounded installation path.

This review is read-only. It records source inspection only; build and test results belong to the
owning validation run.
