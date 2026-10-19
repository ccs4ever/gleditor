# Increment 21 independent native evidence and claim review

## What the frames support

The `native-compat` restart frames support the narrow claim that the saved failed request opens
again in the ordinary chooser and that the review form presents the stored failure and identity. In
`02-restarted-failed-form.png`, the failure note, selected `Refresh progress` action, unchecked
verification, carrier hash, requested edition, publication name, and publisher control all fit in
the form. This is a restart/inspection result; no Retry or discard action was taken.

The `local-import` run supports successful local signed-publication import and installation. The log
records a complete store opening, and the target native files and three retained dependency trees
are present and validated in the run report. The final frame does **not** establish full page
readability: the document's text is mostly clipped beyond the left edge of the visible page area,
and the link-context panel occupies the center-right of the view. The frame shows a visible text
fragment and the link context. The operation dump and UI text tree establish model/content
availability, not that the whole imported page was comfortably readable in this composition.

The log's `--dump-a11y` output is Xuzz's internal accessibility tree. Both runs also state
`accessibility: not in this build`; these captures do not prove native AccessKit/AT-SPI exposure or
screen-reader operation.

## Source-test coverage update

The final source diff closes two test-coverage gaps noted in the earlier review:

- `exactInstallFootprintMatchesRetainedFiles` now snapshots the installed reader permascroll bytes,
  repeats `nativeFileSizes()`, and checks the operation count, permascroll bytes, and returned sizes
  remain unchanged. `pendingMetadataRefusesWithoutMintingOperations` also checks permascroll bytes
  as well as operation count.
- `canonicalFixturesKeepTheirExactEncodedBytes` reads three existing `store.tables` fixtures,
  decodes them, and compares the canonical encoder output byte-for-byte with each original file.

These resolve the review's source-level coverage gaps. I did not rerun builds or tests; pass/fail
claims for those checks remain with the owning build/test report.

## Evidence references

- Restart frames, log, decoded request, and hashes: `native-compat/raw/`,
  `native-compat/logs/restart.log`, `native-compat/checkpoints/`, and `native-compat/evidence/`.
- Local-import frames, import log, installed-file inventory, retained-seed closure and hashes:
  `local-import/raw/`, `local-import/logs/local-import.log`, `local-import/evidence/`, and
  `local-import/checkpoints/`.
- The three fixture byte-equality assertions are in `tests/xudu/store_tables_test.cpp`; the
  permascroll purity assertions are in `tests/xudu/publication.cpp`.

No product files were edited and no builds or tests were run for this final evidence review.
