# Pre-admission native store sizing plan

## Scope

Increment 21 now exposes exact `ops.nodes` and `store.tables` lengths from a prepared native
`Store`. That query deliberately runs after `restorePublication`, which has replayed signed
operation segments and verified referenced primedia. The next prerequisite is to determine those two
native file lengths from the already verified signed `Publication`, before dependency payloads are
downloaded or retained. This is only a native-file projection. It does not size the publication
manifest, torrent metadata, dependency payloads, temporary copies, or peak disk usage, so it cannot
by itself authorize a whole-download budget.

## Existing source of truth

`restorePublication` verifies the signature, requires a reader permascroll and complete history,
checks every operation segment’s contiguous `at`/`length`, fetches and applies its payload, checks
that applied operation count equals the signed segment length, then verifies history heads,
structure inventory, selected version/birth, and selected pieces. Its setup binds
`pub.scrolls[pub.historyScroll]` first as the imported local scroll, then calls `addScroll` for all
signed scrolls. `addScroll` deduplicates by `sameContentAs`, merges segment descriptors, and assigns
deployment IDs in encounter order. The final table state is thus determined by the signed scroll map
plus this exact ordering/deduplication rule.

`Store::deploymentTablesForSave` writes `localSegments` followed by unique external scroll segments,
the external scrolls in deployment order, and `publishedLocalScroll`. A restored publication has no
local author segment inventory; its external descriptors come from the signed scroll map.
`encodeStoreTables` is already the one table encoder. `SegmentedOpsSpool::serializedFileSize()` is
the exact `ops.nodes` formula for a live spool: fixed 64 KiB header plus operation count times
`sizeof(CompactOpNode)`, with integer and platform write-limit checks. Publication segment lengths
are operation counts, not byte counts.

## Smallest safe seam

Add a pure publication-footprint query adjacent to the existing `restorePublication`
publication-domain API. It should accept a `Publication` that has already passed `verifyPublication`
(or explicitly verify it itself), validate the shape needed for a complete-history projection, and
return the same `NativeStoreFileSizes` value as the prepared-store query. It must not create a
`Store`, reader permascroll, `ContentSource`, spool, cache directory, or operations, and must not
access torrent payloads.

The query should use checked addition over the signed `opsSegments` lengths and reject gaps,
overlaps, empty segments, or count overflow using the same rules as restoration. The current
file-size formula should be factored into the spool owner as a count-taking pure helper, with the
live-spool method delegating to it, so the projection and writer cannot drift on the 64 KiB header,
node size, `off_t`, or `ssize_t` limits.

For `store.tables`, build the would-be restored deployment descriptor value by applying the same
history-scroll-first ordering and `sameContentAs` deduplication/segment merging as
`Store::addScroll`, then pass it to the shared `encodeStoreTables` and use its byte length. Prefer
extracting the descriptor projection/deduplication rule into a small publication/store-table helper
used by both restore setup and the query; do not hand-maintain a second subtly different identity
rule. The projected value should have `documentId = pub.storeId`, empty `localSegments`, the
restored external descriptor vector, and the restored published-local slot. A focused assertion at
restore completion can compare `deploymentTablesForSave()` with the projection, if it can be added
without replay or mutation. No on-disk format or writer changes are needed.

The API should document that arbitrary caller-constructed `Publication` values are untrusted. The
safest boundary is to verify signature plus all manifest-only constraints relevant to projection; it
must not imply complete publication validity, because payload-backed history checks remain the job
of `restorePublication`.

## Validation

- From a signed publication fixture, call the pure query before any content source or retained
  dependency directory exists. Assert it creates no files, operations, permascroll, or dependency
  cache entries.
- Restore/install the same fixture and assert both projected lengths equal the prepared
  `Store::nativeFileSizes()` result and actual `ops.nodes` / `store.tables` lengths.
- Use a fixture with multiple scroll descriptors that deduplicate by `sameContentAs` but have
  complementary segment descriptors. Assert projected table bytes match the restored encoder result
  and deployment IDs/order remain unchanged.
- Cover empty history rejection, non-contiguous/zero-length operation descriptors, checked count
  overflow, and a count whose native length exceeds platform file limits. These are pure-query
  refusals, with no side effects.
- Keep existing writer-equivalence tests for `store.tables` and `ops.nodes`; this query must reuse
  those authorities rather than replicate the byte encoders.

## Limits and unresolved admission work

The signed manifest gives segment operation counts and scroll descriptors, but it does not by itself
establish availability or size of the referenced torrent metainfo and payload files. Metainfo-only
fetch/admission must still happen before payload transfer, and its exact allocation behavior needs
separate verification. Current publication download setup creates request/staging directories before
metadata is known; this projection cannot promise zero temporary filesystem use. Exact native
lengths are also not the same as logical dependency bytes, retained-copy bytes, allocator usage, or
peak staging space.

Therefore this seam enables an early native-store component of admission, but a future aggregate
disk policy must combine it with bounded metainfo size, verified retained dependency lengths, and
any peak copies. It must reject before copying payloads or starting payload download when those
measurements are available; no budget default or network-enablement policy is inferred here.
