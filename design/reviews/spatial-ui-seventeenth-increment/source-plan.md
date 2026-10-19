# Source plan: package trust and cited-content availability

Read-only source audit at `f3841729`. I used the repository `bittorrent-expert` skill and checked
the sixteenth-increment contract, publication workflow, exchange/inbox implementations, identity
boundary, and owning tests. No product code, service, profile, or app run was changed or simulated.

## Current boundary map

| Fact                                  | Existing source of truth                                                                                                                                                                                                                                                                                                                                                       | Current user-visible outcome                                                                                                                                                                                                                                               | Gap                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Curator identity enrollment           | `PublicationIdentity` and the optional `verifyIdentity` callback on `LinkPackageExchange::Options` / `PublicationOutbox::Options`                                                                                                                                                                                                                                              | Outgoing announcement without a successful callback stops at `NeedsVerification`; its phase says “Curator enrollment required.” Incoming package review says “signature verified; enrollment unchecked.” Discovery calls catalog metadata signed and enrollment unchecked. | The incoming `LinkPackageExchange::step()` never calls `verifyIdentity`; its status remains `Unknown` through `Ready`. `Session` installs callbacks only in the test publication-swarm path. The BEP 10 identity ledger/controller exists, but no reader-facing lookup result is wired to these package or citation statuses. There is no production positive enrollment result for a received curator in this path. Unknown enrollment is not the same as an invalid signature and is currently not treated as one.                                                                                                                                                                                                                                                                                                                                                                                         |
| Package carrier and payload integrity | `reviewPublicationSeed()` checks the torrent/infohash content; `decodeLinkPackage()` checks format/canonical structure; `reviewLinkPackage()` checks the curator signature, limits, declarations and endpoint ranges; `validateReceived()` compares curator, salt, sequence, title and scroll keys against the signed catalog entry.                                           | Successful receipt becomes `Ready`. Failure becomes `Failed`; the package status form shows “Download failed” plus the raw `error`. The signed-endpoint review says signature verified after `cacheVerified()` rechecks the package and carrier hash.                      | `Failed` is one lifecycle phase for transport/peer unavailability, bad seed/hash, malformed or noncanonical payload, invalid signature, invalid endpoint declarations, catalog mismatch and rollback. Some error strings are specific, but the `decodeLinkPackage()` null case says “Package signature or canonical encoding failed,” and `reviewLinkPackage()` combines signature and top-level limits as “invalid signed package or limits.” There is no typed integrity result for UI, retry policy or tests to consume. A package `Ready` proves the current package checks passed; it does not prove enrollment or cited-document availability.                                                                                                                                                                                                                                                         |
| Exact publication citation            | `PublicationPin` in the signed package carries publisher, salt, infohash, sequence, version and title. The review form lists the pin and calls `PublicationInbox::submitPinned(pin)` on explicit Open. The inbox fetches `pin.hash`, checks publisher/salt/sequence and then the exact pinned version/title; it fetches and installs all declared dependencies before `Ready`. | Package review says cited bytes are checked separately when opened. Inbox status shows its phase and raw error; success offers “Open completed publication.”                                                                                                               | Cited availability is neither queried nor displayed beside each pin in package review. No request means “not checked,” not “unavailable.” `PublicationDownloadStatus` exposes URI/title/progress/version/sequence/manifest hash/error but not the original pin. The failed form does not show the exact expected carrier hash/version/title. Its single `Failed` phase covers unavailable transport or pieces, signature/name/sequence/pin mismatch, bad dependencies and install failures. A `Ready` inbox status means the exact opened request completed, but it is not clearly presented as a result for a named citation. On restart, completed snapshots are reconstructed from their manifest; the original `pin` is not retained in status, and `manifestHash` is not restored there. There is no exact-pin availability lookup/deduplication API for a package citation already downloaded locally. |

### Important distinctions already enforced

A received package cannot reach `Ready` with invalid bytes or invalid endpoint declarations. It
reaches that phase only after seed integrity, decode, signature/declaration review and
signed-catalog identity checks. `Unknown` enrollment is deliberately orthogonal to those checks.
Conversely, the outgoing `NeedsVerification` phase is an announcement gate and must not be reused as
the received package’s signature result.

A citation is an immutable exact pin. `PublicationInbox::submitPinned()` does not resolve a newer
mutable head, and wrong version/title or sequence fails. `LinkPackage::reviewLinkPackage()`
explicitly checks declarations and ranges, not remote availability. A missing citation fetch must
not make the package itself invalid, and the package being ready must not be rendered as evidence
that citation bytes are available.

## Smallest next increment

Keep the three facts separate at their current owning seams: transfer/acquisition, cryptographic and
payload verification, and identity enrollment. Do not add an identity simulation or make a signature
imply enrollment. Until a real enrollment lookup returns a verifiable result, show
`Unknown / not checked` for incoming curators. If a verified identity state is required, the
prerequisite is a real Session-level adapter to the existing identity ledger/Oracle proof path,
including the key binding and revocation/effective-consensus rules. The current callback seam alone
only permits an injected answer; it does not supply that proof.

Give package receipt a typed result beside its existing phase/error: at minimum transfer
pending/failed, package integrity verified/rejected, and catalog-advertisement mismatch. Keep the
underlying detailed diagnostic. Separate decode/canonical failure, signature failure,
payload/declaration failure and catalog identity/equivocation failure sufficiently for the form to
state what was rejected and whether retry can help. Do not offer Retain/Enable for a rejected
carrier. Keep unknown enrollment available for private review while saying it is unknown. Do not
infer these categories by parsing prose in the UI.

Carry the exact `PublicationPin` through the inbox status projection and expose a lookup for an
already completed local snapshot by the full immutable pin. For each package citation, show
`Not checked` until the reader explicitly opens it or an exact local snapshot is found; after a
request show progress, `Available` only when that exact pin and all dependencies verify, or
`Unavailable / retryable` for acquisition failure. Show integrity or pin mismatch as rejected with
its diagnostic, distinct from unavailable transport. The failure/status form should name the
requested publisher, salt, sequence, version, title and full infohash so the reader can see which
citation was checked. A retry must continue to use that same pin. This can use the existing
`PublicationInbox` and `PublicationDownloadStatus` seam; it does not require a new acquisition
service or silent network request.

The compact package review/comparison form should expose clear values for (1) endpoint
signature/payload verification, (2) curator enrollment state, and (3) each cited publication’s
availability. Where there has been no exact-pin request or local exact match, use “Not checked.”
Preserve the existing clear distinction between signature verification and enrollment unchecked. Do
not label a remote citation “unavailable” merely because it has not been requested.

Avoid a new durable preference or store format. Exchange error states are not serialized in its
format-1 request record, and failed inbox jobs are runtime status; a typed in-memory outcome can fit
those seams. If the implementation decides to persist new status or pin associations, bump that
owning format and refuse the previous version by number under the repository’s format rules rather
than silently inferring a pin or result.

## Owning verification

- Extend `tests/xudu/link_package_exchange_test.cpp` at its received-catalog path to cover a valid
  package with `identity == Unknown` at `Ready`, and distinct received failures for corrupt
  seed/content, malformed/canonical bytes, bad signature, invalid endpoint payload, signed catalog
  mismatch and rollback. Assert no invalid carrier becomes retainable. Keep the unknown identity
  assertion separate from every integrity failure. Existing
  `PieceCorruptionCannotProduceReadyPackage`, `FalseCatalogClaimsAndUnrelatedQueriesAreRefused`, and
  high-water tests are the closest owners; do not duplicate them in UI tests.
- Extend `tests/xudu/publication_outbox_test.cpp` under `PublicationInboxTest` for exact-pin status
  association: no request is `Not checked`; a successful exact pin is available; wrong
  version/title, sequence/hash mismatch, bad signature/dependency and unavailable peer remain
  distinguishable; retry uses the original exact pin; restored local snapshots can be matched by the
  full pin. Existing `PinnedSnapshotDoesNotResolveNewerMutableHead` proves exact resolution but not
  the visible result or stable pin association.
- Add a focused existing-form regression for the package review and download status paths. It should
  assert the three separate facts and exact pin identity, and verify retry/retain actions are
  offered only for the appropriate outcomes. Keep the test at the UI form seam; do not use a
  scripted journey as evidence of network trust or availability.
- Actual enrollment-positive, remote unavailability and remote bad-content claims require real
  configured identity/acquisition peers and captured status from those controls. Do not use fake
  callbacks, test swarm peers, fabricated status records or offline fixtures as native proof. If an
  outcome cannot be produced by the real environment, report that outcome as unverified. Unit
  fixtures may verify classification logic but must remain labeled as unit evidence.

The relevant design contracts are `design/ux_workflow_publication.md`,
`design/publication-implementation-progress.md`, `design/oracle-identity-model.md`,
`design/bep46-publication-and-discovery.md`, and
`design/reviews/spatial-ui-sixteenth-increment/README.md`. Source owners are
`link_package_exchange.{hpp,cpp}`, `link_package.{hpp,cpp}`, `publication_inbox.{hpp,cpp}`,
`publication_discovery.{hpp,cpp}`, `publication_outbox.{hpp,cpp}`, `views_publication_links.cpp`,
`views.cpp`, and `session.cpp`.
