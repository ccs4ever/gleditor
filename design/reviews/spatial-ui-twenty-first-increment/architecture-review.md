# Increment 21 architecture review: exact staging-size seam

## Recommendation

Keep admission and accounting in `PublicationInbox`, as the plan says. Reuse the existing store
writer and the install path's verified objects; do not add a second store serializer, a separate
storage service, or a view-owned scan. The smallest sound seam is an explicit prepared-save value
that freezes the exact data the existing `Store::save()` writes, paired with one canonical
`store.tables` byte encoder used by both that value and `writeStoreTables()`.

The prepared value must be explicit because `Store::save()` currently starts with
`sealPendingMetadataAsCells()`. That method can append Structure operations while sealing pending
annotations, current editions, or scroll registrations. A supposedly observational size query that
calls it would silently change the authored history. Make preparation a deliberate operation (or
require the caller to explicitly seal first), then make measuring and writing the resulting snapshot
side-effect-free with respect to the Store.

## Minimal shared contracts

1. **Store save snapshot.** Add a `PreparedStoreSave`/equivalent created explicitly from a Store
   after metadata sealing. It captures the sealed operation count and the `StoreTables` value that
   will be emitted. Its checked `logicalBytes()` reports the exact native files, and its writer
   delegates the operations file to `SegmentedOpsSpool::writeSegmentFile()` and the tables file to
   the shared canonical table encoder. `Store::save()` should use the same preparation-and-write
   path so the estimate cannot drift from normal saves. The snapshot should either hold the exact
   encoded tables bytes or immutable table inputs plus their canonical encoding; the writer must
   write those same bytes.

   `ops.nodes` logical size is `opsSegmentHeaderBytes + opCount * sizeof(CompactOpNode)` with
   checked multiplication/addition. `opsSegmentHeaderBytes` is 65,536, the sparse fixed header
   region; using `sizeof(OpsSegmentHeader)` would undercount. Check conversion to `off_t` and the
   writer's actual count before writing. The estimator should report file logical lengths, not
   filesystem allocation blocks.

1. **Canonical tables bytes.** Refactor the body construction inside `writeStoreTables()` into one
   encoder returning the complete file bytes (12-byte signature + 4-byte version + canonical bencode
   body), or an equivalent immutable encoded artifact. Both `writeStoreTables()` and the prepared
   snapshot consume it. Do not maintain a parallel bencode size formula: strings, lists, integer
   encodings, and future format fields would otherwise drift. Validate the published-local index in
   this common path exactly as the writer does today.

1. **Verified seed accounting.** Extend the existing verified-seed/preflight result, rather than
   inventing a second filesystem walker, to expose checked `metainfoBytes` and total payload bytes.
   For a retained seed, these are the actual `metainfo.torrent` byte length and the sum of validated
   regular-file lengths. The direct transport already has the encoded metainfo and validated
   `meta->totalLength()` before `startDownload()`: surface that measurement to the inbox at this
   point, and reject before payload transfer if the budget cannot admit it. The final accounting
   should reconcile to the same values returned by `reviewPublicationSeed()` after download and
   after retention-copy verification. Keep metainfo and payload as distinct terms in diagnostics.

1. **Signed install plan and verification.** Before fetching payload, derive the expected operation
   count from the signed, contiguous `opsSegments` lengths with checked addition. Build the expected
   `StoreTables` inputs from the verified manifest's store id, scroll descriptors, and published
   local binding through the same helper used by restoration. This permits the canonical encoder to
   give the exact table bytes before install writes. Once dependencies are present and history has
   been restored, create the actual prepared save snapshot and require its operation count and
   encoded tables bytes to match the signed-plan prediction before writing the native store. A
   mismatch is a refusal that preserves the attempt data; it must not fall back to a guessed factor.

   Count the native manifest file separately at its actual retained byte length. The install peak
   includes the manifest/dependency caches, the retained dependency copies, `ops.nodes`,
   `store.tables`, and the retained signed manifest while those copies coexist. Reserve and check
   that peak before beginning a payload or install write; then reconcile it against the actual
   attempt-tree logical file sizes before the final immutable-root rename. Keep the existing
   per-request ceilings independent from the aggregate budget.

These are narrow value/encoder seams in the existing owner chain. They do not require durable
reservations while the inbox worker remains serialized; on restart, the inbox can rescan owned roots
and attempts. A future concurrent worker would need synchronization around reservations, but that is
outside this increment.

## Current blockers to exact pre-write enforcement

- `Store::save()` is not observational: its initial metadata sealing may mint operations. Exact
  sizing must not call `save()` or an implicit preparation path merely to ask for a number.
- `writeStoreTables()` currently builds bencode internally and writes it directly. There is no
  shared encoded-byte artifact for an estimator to consume today.
- `PublicationDownloadTransport::fetch()` returns `void`. The swarm implementation knows the
  metainfo byte string and payload length before starting download, but the inbox cannot reserve
  those measured values through the interface. Add a preflight/measurement result or a fetch
  admission callback; do not wait until the seed is fully cached.
- A post-restore `Store` snapshot alone is too late to enforce the plan's requirement to refuse an
  oversized complete peak before payload transfer. The signed manifest contains enough operation
  count and table inputs in principle, but that projection must share the same table builder and be
  checked against the restored snapshot.
- A scan of actual staged bytes after writing is a reconciliation check, not pre-write admission. A
  guessed expansion factor is not exact and cannot establish the stated contract.

## Review lenses

- **Architectural synthesis:** the prepared snapshot, canonical table bytes, and verified seed
  measurement attach at current ownership seams (`Store`, table writer, transport/seed verifier,
  `PublicationInbox`). No parallel persistence architecture is justified.
- **Quality audit:** hidden operation creation in `save()` is the primary correctness hazard;
  duplicated serialization is the primary estimate-drift hazard; a UI directory walk would create a
  second, stale accounting authority.
- **Systems realism:** define the guarantee as aggregate logical bytes in inbox-owned roots and
  attempts. Checked arithmetic and fail-closed path validation are required. This does not bound
  filesystem allocation overhead, unrelated profile files, or power-loss outcomes.

This is an architecture review only. No product code was edited or built, and no implementation or
test result is claimed.
