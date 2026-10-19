# Completed Inbox root ownership audit

Read-only source audit, prepared for any future UI action that forgets a completed Inbox snapshot.
No application or build was run. This is a code-path inventory, not a deletion-safety proof from a
runtime ownership index: no such centralized index was found.

## Finding

A completed Inbox directory is shared durable content, not disposable retry scratch. Until a Forget
protocol inventories and releases its consumers, keep the whole `publication-inbox/<request-id>/`
root intact. It contains the signed `publication.xanadoc`, an installed native `store/`, and
retained `store/published/` dependency content. The journal's Ready record maps `storePath` back
into that root. Removing only the directory while retaining the Ready journal is inconsistent: Inbox
startup reconciliation changes a Ready record with a missing installed snapshot to Failed. Removing
both record and root can orphan other persisted references described below.

The existing `Discard partial data` boundary is materially narrower. In
`apps/common/xanadu/publication_inbox.cpp`, `ownedPartialAttempts()` selects only sibling
directories named `<id>.attempt-N` whose numeric attempt is no newer than the persisted attempt,
validates their paths, and `removePartialAttempt()` removes only those trees. `cleanupPartialData()`
syncs the Inbox parent and reports a transient result. `PublicationDownloadStatus::cleanup` is
explicitly excluded from XPI1 in `publication_inbox.hpp`. `discardPartialData()` accepts only
Failed/Cancelled requests and queues this attempt cleanup; it does not delete the journal job or
completed `<id>/` install. This operation is not a template for Forgetting a Ready root.

## Persistent owners and lookup paths

| Owner                    | Durable reference                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             | What removing a completed root breaks                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Open session             | `Session::openDownloadedPublication()` loads `status.storePath` through `loadAuxiliaryStore()` and registers that same path in a `StoreEntry`. Open document/view records retain a store index. `Session::save()` writes to the registered path.                                                                                                                                                                                                                                                                              | A live Store remains backed by the removed path; reads, offline dependencies, or later saves can fail or recreate an incomplete directory. The `stores` vector has no removal path in the audited Session code. Closing a document view is not evidence that its StoreEntry has been unloaded. Refuse root deletion while the Session has that path open.                                                                                                                                                                                                                                                                                               |
| Last saved reader place  | `Views::currentPlace()` records `session.path(view.storeIndex)` for each document. `ReadingPlace::DocumentPlace::storePath` and `ReadingPlace::zigzagStore` are serialized verbatim by append-only `recordPlace()` into `system://activity`. On startup, `xuzz_app.cpp` calls `lastPlace()`, checks `<storePath>/ops.nodes`, and reopens the store from that exact path; the slice restore also matches `zigzagStore` against `Session::path()`.                                                                              | Forgetting the root makes persisted resume paths unusable. `recordPlace()` appends place records; older records remain in the activity store, while `latestPlace()` reads the newest. No place-pruning or owner-release protocol was found. At minimum protect the latest place, and treat older places as retained history until their retention semantics are defined.                                                                                                                                                                                                                                                                                |
| Saved navigation visits  | `StoreActivityLog` records `Visit::target` as `DocumentSite` or `CellSite` containing store `DocumentId`, version, range, and optional cell. `LinkVisitContext` also persists link authority/id, origin/checkpoint, and exact `OccurrencePin`s (member `PrimediaSpan` plus occurrence site). These contain no Inbox request ID or filesystem path. `Session::storeIndexForAuthority()` resolves only among currently loaded stores.                                                                                           | Visits and references can outlive open views. Removing the root leaves the durable authority/version/span address but no local store to resolve it. Activity does not identify which Inbox request root supplies that authority, so Forget must map the root's store identity to all visit/link targets or preserve the content elsewhere.                                                                                                                                                                                                                                                                                                              |
| Update subscription      | `PublicationSubscriptionStatus::snapshotId` names its accepted baseline Inbox job. Every update notice stores `snapshotId` and `previousSnapshotId`; an in-flight subscription stores `pending`. `subscribe()` captures the baseline ID. `decode()` calls `inbox.status()` and checks exact URI, sequence, version, and `PublicationPin` for baseline and notice snapshots.                                                                                                                                                   | Pausing a subscription with `setEnabled(false)` keeps the record and its references; acknowledging a notice marks flags but does not immediately erase it. The baseline remains referenced, and notices are removed only during later accepted-update processing when delivered and acknowledged. No subscription delete API was found. Preserve every current, previous, notice, and pending snapshot until a removal operation releases those references. Missing IDs break update open/review; a missing retained root demotes its request from Ready and prevents `openDownloadedPublication()`.                                                    |
| Independent link package | The signed `LinkPackage::publications` list stores exact immutable publication pins (publisher, salt, carrier hash, sequence, version, title), but not Inbox IDs or paths. `reviewIndependentLinks()` correlates those pins against Inbox statuses for “Complete local copy” and opens a cited publication by calling `submitPinned(pin)`. Package preferences are keyed by package carrier hash in `package-visibility`; the verified package payload is retained separately by `LinkPackageExchange` under `link-packages`. | Removing an exact-pin Ready root removes the local complete-copy status/content for a package citation (availability becomes “Not checked” unless another request for that exact pin exists). Reopening the citation can create/reuse a request but does not preserve the completed bytes. `setRetained(false)` disables reader retention but does not delete the signed package cache or release its publication pins. Package endpoint scroll metadata names global scrolls/ranges, not payload bytes; if the Inbox install is the only local source for an endpoint, deleting it also removes the local basis for resolving/rendering that endpoint. |
| Package source chooser   | `Session::packagePublicationSources()` reads signed manifests from every Ready Inbox status at `status.storePath.parent_path()/publication.xanadoc` and offers them as sources for independent link packages.                                                                                                                                                                                                                                                                                                                 | Deleting the root removes that source from the chooser. This is an availability dependency even when no package pin has yet been emitted.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |

Relevant implementation locations include `apps/common/ui/xanadoc/session.cpp` (`publicationInbox`,
`readerLinkPackages`, `openDownloadedPublication`, `loadAuxiliaryStore`, `storeIndexForAuthority`,
`packagePublicationSources`, `rememberPlace`); `apps/common/ui/xanadoc/views.cpp`
(`publicationDownloadStatus`); `apps/common/ui/xanadoc/views_publication_links.cpp`
(`reviewIndependentLinks`); `apps/common/xanadu/publication_inbox.cpp` (`ownedPartialAttempts`,
`cleanupPartialData`, startup reconciliation); `apps/common/xanadu/publication_subscriptions.cpp`
(`decode`, `subscribe`, update acceptance, `setEnabled`, `acknowledge`);
`apps/common/xanadu/link_package.hpp` (`LinkPackage::publications`);
`apps/common/xanadu/reading_place.{hpp,cpp}`; and `apps/common/xanadu/store_activity_log.cpp` /
`link_navigation.hpp`.

## Ownership rules needed before Forget

A Forget action must distinguish at least three things: canceling a worker, discarding partial
attempt directories, and deleting a completed installed snapshot. For a completed snapshot, derive
its exact publication pin and installed store authority from the validated manifest/store, then
check all of these before removing any bytes:

1. No live Session StoreEntry or open document/view still uses the canonical store path.
1. No retained current or historical reading-place record names the store path in `documents` or
   `zigzagStore`; if history is intentionally exempt, define how the user releases it.
1. No saved activity visit, origin checkpoint, occurrence pin, or package link authority depends on
   the store identity/version unless the user explicitly releases that navigation history.
1. No subscription baseline, pending request, current notice, or previous-notice snapshot ID points
   at the Inbox job; paused and acknowledged records still count until deleted/pruned.
1. No retained package cites the exact pin, and no retained package endpoint depends on this being
   the last local payload for its scroll ranges. Package citation refs must be matched by full pin,
   since the package stores no request ID.
1. No other local source owns the same required signed manifest/dependency closure. Availability
   should be computed by immutable identity and verified bytes, not title or mutable name.

If any consumer remains, either refuse and name the owners or provide explicit detach/release
actions that update those records before deletion. The cleanup and release steps need crash-safe
ordering so a restart cannot observe a surviving reference to a half-deleted root. Keep the inbox
journal/root consistent across restart and preserve the full pin for any history the user chooses to
retain.

## Unknowns

- There is no completed-snapshot Forget API or centralized reference count. Current sources do not
  establish whether a future UI operation should remove the journal job, the content root, or both.
- Saved `system://activity` place history is append-only; only the newest place resumes
  automatically. The product has no evidenced user action for pruning old places that name a
  completed Inbox path.
- Visit records store store authority/version and span pins, while ReadingPlace stores direct paths.
  No registry currently maps those authorities back to an Inbox request ID or exact publication pin.
- Link-package publication pins match request status exactly but not request IDs. Package payloads
  can remain retained after their source publication ceases to be locally Ready. The intended owner
  and release rule for those citations is not yet specified.
- A package may reference global scroll ranges available from multiple stores or sources. No
  ownership index was found that proves an Inbox install is or is not the last local source for each
  range.
- Deleting a completed root may race with active session persistence or a subscription worker. A
  lifecycle lock/refcount spanning these owners is not present in the audited interfaces.

Until these unknowns are resolved, future Forget must not remove a completed Ready root. The native
zero-byte `Discard partial data` journey only covers safe partial-attempt cleanup and does not
establish completed-root ownership.
