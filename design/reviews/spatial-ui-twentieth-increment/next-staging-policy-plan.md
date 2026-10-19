# Aggregate publication staging policy plan

## Evidence from the current implementation

`PublicationInbox::Options` has a 16 MiB manifest-carrier limit, a 1 GiB `maximumDependencyBytes`
limit, a 4,096-dependency limit, and a 128-request limit. `DownloadTransport::fetch()` checks the
torrent metainfo's payload `totalLength` against the caller's limit before starting payload
transfer. The worker then checks each verified seed's byte length and subtracts it from one
remaining dependency allowance. This is an aggregate payload limit for one request's unique
dependency torrents; it does not include the manifest, torrent metainfo files, filesystem overhead,
other requests, retained installations, or prior failed attempts.

Each retry creates a new `<request-id>.attempt-N` directory and its cache. Failed and cancelled
attempts remain until explicit partial-data discard. There is no automatic eviction, and the
128-entry limit bounds receipts, not bytes. Thus repeated failed requests can each retain close to
the current per-request allowance. The current worker serializes downloads, but completed roots and
old attempts still accumulate across requests.

On success, the worker downloads the manifest and dependencies under the attempt's `cache/`.
`installPublication()` copies verified torrent seeds into `store/published/`, reconstructs and saves
the native store, and verifies an offline load. Only afterward does the inbox remove the cache,
retain the signed manifest, and commit the immutable request root. The install peak therefore
contains both dependency cache payloads and retained copies, plus native store files. The code has
no aggregate admission check for this peak or for retained root plus attempt usage.

The increment-20 failing-attempt fixture measured **1,360 logical bytes**. That failed request had
cached the small signed manifest carrier, then failed before its first dependency was copied into
the cache. It had no retained dependency copy or native store. This is useful as a regression
measurement, not as a real request-size estimate or a bound for successful installs.

Existing authored sample trees give a wider, still small lower-bound range:

| Sample                         | Whole tree | `published/` retained dependencies |
| ------------------------------ | ---------: | ---------------------------------: |
| core hypertext unified store   |   79,075 B |                     not applicable |
| multimedia audio document      |  420,720 B |                          352,987 B |
| multimedia video document      |   89,621 B |                           21,900 B |
| multimedia embedded-media page |  444,859 B |                          375,268 B |
| multimedia floating image      |   66,399 B |                              348 B |

Across the nine multimedia `published/` trees, `du -sb` reports 1,126,634 B. For the embedded-media
page, files in that tree total 375,268 B: three metainfo files total 418 B, and the three payload
files total 374,850 B. These fixtures contain actual authored sample dependencies, but they do not
include large user documents and cannot justify a production default budget. No larger authored
publication carrier with realistic media was found in the existing sample corpus.

## Governed limit and admission contract

Keep the budget in the existing owner chain: add an integer `publication.stagingBudgetBytes` setting
to `defaultSettingSpecs(Settings)`, read it when `Session::publicationInbox()` constructs options,
and enforce it inside `PublicationInbox`. Do not put accounting in the view or add a storage
service. The normal session currently configures a transport only for its explicit test swarm; this
setting must not be presented as evidence that normal publication reception is already enabled.

Use a safe default of **zero admitted bytes**, which rejects every non-empty carrier before payload
fetch. The reader must choose a positive finite value before ordinary reception is enabled. Zero is
a literal zero-byte admission limit, not an unbounded/default sentinel. Preserve the 16 MiB manifest
and 1 GiB per-request dependency ceilings as independent ceilings: the effective request limit is
the minimum of per-request capacity and aggregate remaining capacity. The settings note and failure
status should state that no payload was fetched when the configured budget was zero or insufficient.

Budget usage must include validated installed request roots and all validated attempt trees,
including retry caches; completed roots count permanently and are never implicitly removed. Receipts
remain under the separate 128-request limit, and explicit partial-data discard frees only validated
partial files, not a request slot or completed copy. No aggregate quota failure may trigger
automatic cleanup or lower the user's configured limit.

Before creating a transport or writing payload, the worker should scan owned roots with
overflow-checked logical file-size accounting and fail closed on unsafe trees. Check the carrier
metainfo total before its payload, then check each dependency's metainfo total before payload and
reserve its eventual retained copy. The operation segment descriptors carry operation counts, so the
exact `ops.nodes` node payload and fixed segment header reservation can be derived without a guessed
expansion factor. The estimator must also account for `store.tables`, retained metainfo files, and
the signed manifest; verify the estimate against the actual staged files before the final
immutable-root rename. If it cannot establish that the whole install peak fits, refuse before
install writes and preserve cache files for explicit cleanup or a later, larger user-selected
budget.

The UI should show one inbox-level `used / budget` value from the inbox owner, plus the existing
per-request dependency progress. Do not make the UI walk directories. A budget refusal should name
the measured requirement, current usage, configured budget, and identify the request's partial files
as preserved. A completed-root reservation must reduce remaining capacity for later requests; no
control may offer to discard it in this increment.

## Smallest viable implementation boundary

Use `PublicationInbox` as the sole admission authority and keep all writes on its worker. Add a
checked owned-tree usage scan and one configured aggregate limit. At each fetch, reserve the
remaining cache payload plus its later retained copy; before installation, reserve the exact
generated native-store files and manifest. Reconcile the reservation against actual attempt-tree
bytes before the ready/root-rename transaction. Reservations are in-memory only because the worker
is single-threaded and sequential; durable per-request budget fields would add no guarantee. On
restart, recompute from retained trees and the existing attempt roots before any transport is
created.

One unresolved detail must be answered during implementation review: an exact pre-write bound for
`store.tables` and torrent `metainfo.torrent` bytes is not currently exposed by
`PublicationDownloadTransport::fetch()`. `fetch()` checks payload size, but its interface returns no
measured byte count and does not expose metadata size. The existing direct swarm transport can
inspect `torrentMetadata()` before payload and should provide checked size accounting at that same
point. For the native store, use the verified publication's operation counts and deterministic
tables serialization inputs to derive the exact output size, or add a small dry-run size query to
the existing installation seam. Do not claim strict pre-write enforcement using a guessed factor or
a post-install scan alone.

## Tests and claims

- Add an admission test with a small configured budget proving that an oversized manifest and an
  oversized dependency are refused before the mock transport's payload-copy path; preserve exact
  request state and bytes.
- Seed a real completed root plus a failed attempt, assert the usage scan counts both, and show the
  second request is refused before transport creation when the combined installation reservation
  would exceed the budget.
- Verify retry admission includes all older attempts; after explicit partial discard, the same retry
  can proceed if its reservation now fits.
- Exercise the install-copy peak with a fixture whose dependency cache and `store/published` copies
  are both present; assert Ready never occurs when the combined exact reservation exceeds the
  configured limit and that the attempt remains intact for explicit cleanup.
- Verify a completed root consumes budget permanently and is not deleted when later admission fails.
  Verify malformed/symlink trees refuse accounting and preserve every byte.
- Compare predicted install bytes against actual staged logical file sizes for the signed sample
  publications above and the existing unit publication fixture. Keep the 1,360-byte
  interrupted-dependency number labeled as a failure fixture only.

Tests can use the existing deterministic test transport for byte-copy accounting; do not claim
network availability or native swarm proof. Existing real sample payloads are too small to set a
default. Production sizing remains user-governed, and network reception remains disabled until a
positive finite budget is chosen. The budget is an admission cap over inbox-owned files, not a
filesystem quota: unrelated profile data, filesystem allocation overhead, and power-loss behavior
are outside its guarantee.
