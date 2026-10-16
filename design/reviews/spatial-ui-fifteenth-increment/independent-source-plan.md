# Independent source plan: reader retain, enable, and emphasis

Audited worktree: `.worktrees/spatial-ui-overhaul`, HEAD `d41664a3ba2f57ddb1e08fd082cf10a40bf67185`.

## Existing model and call flow

`ReaderLinkPackages` currently has two distinct stores in memory: `choices` maps immutable carrier
`InfoHash` to a Boolean, while `retained` maps that hash to a verified `LinkPackage`. Its public API
exposes retain, enable/disable, lookup, and enabled links
(`apps/common/xanadu/reader_link_packages.hpp:15-49`). This split is sound: identity comes from the
torrent hash, and `keyOf(hash, ordinal)` derives a stable `LinkKey` authority plus the package link
ordinal (`reader_link_packages.cpp:58-69`). `retain()` re-encodes and re-hashes the verified package
before accepting it (`:71-94`).

The on-disk `package-visibility` bencode is currently strict format 1: exactly `format` and
`choices`, up to 128 hash-to-Boolean entries, with collision checks. Any other version is refused by
name; writes use a `.partial` file and rename (`reader_link_packages.cpp:20-56,102-142`). This is
the right boundary for format 2: bump to 2 and refuse format 1 without migration/shim, as required
by the repository's format policy.

`Session::readerLinkPackages()` loads that preference file and rehydrates payloads from
`LinkPackageExchange::statuses()` only when the preference map is nonempty; eligible ready/published
or locally cached packages are re-verified by `retain()` before they enter the in-memory cache. It
also refuses authority collisions with open stores (`apps/common/ui/xanadoc/session.cpp:703-722`).
This makes persisted preference and verified payload availability intentionally different facts.
After restart, `enabled(hash)` can be true while `find(key)` is null because the exact payload has
not been recovered. Existing tests assert precisely that and then assert the same key resolves after
`retain(pkg, hash)` (`tests/xudu/reader_link_packages_test.cpp:39-71`). Preserve this
missing-payload state; do not silently turn it into disabled, deleted, or a partially trusted link.

The UI currently offers package-level “Enable reader layer” / “Disable reader layer” and link
selection for navigation only while enabled (`apps/common/ui/xanadoc/views_publication_links.cpp`,
`reviewIndependentLinks`). `Session::setLinkPackageEnabled()` refuses enabling unverified packages,
retains the verified package first, persists the bit, and invalidates the view
(`session.cpp:724-755`). Selection is keyed by `LinkKey`; disabled package selection can remain
pinned and recoverable, and existing navigation tests cover disabling/re-enabling a pinned link and
dismissing it without writing to the visited store or activity
(`tests/zigzag/reader_link_packages_ui_test.cpp`). “Dismiss” should therefore continue to clear only
the transient selection; it must not erase retained package data or reader preferences.

Text decoration builds package occurrence matches from `readerLinkPackages().links()`, which
enumerates only enabled package links, then adds cached `SpanStyle`s
(`session.cpp:2988-3004,3046-3067`). Package text colour currently forces `ProminenceTier::Curated`
(`:3057-58`); the selected link colour does likewise (`:769-775`). The package's source
`ProminenceTier` expresses authorship/curation and must stay semantic. Reader emphasis is a separate
presentation choice.

Beam construction has two paths. Native links use `placeGroupedLinks`; package navigation resolves
the selected `LinkKey` and creates `Strand`s with the package link's type and tier
(`apps/common/ui/xanadoc/beams.cpp:165-187,228-244,270-315`). Drawing computes colour from
`strand.tier` and then applies document opacity and active/inactive state (`:2018-2025`). `Strand`
stores the semantic tier (`apps/common/ui/xanadoc/beams.hpp:263-290`). The selected-package beam
path can carry an additional reader emphasis field or precomputed quiet factor alongside that tier;
do not rewrite `ProminenceTier` to encode “quiet.”

## Recommended minimal model/API

Persist one strict format-2 record per exact carrier hash, with independent fields:

- `retained`: reader's durable keep/dismiss intent; a dismissed record remains recoverable and can
  be shown again from the retained-package review surface.
- `enabled`: whether this verified package contributes links to the reader layer.
- `emphasis`: `normal` or `quiet`, default `normal`.

Keep these package-level initially. They align with the existing package-level controls and avoid
adding a per-link state matrix before the UI can explain it. If independent link emphasis becomes a
concrete interaction, key it by the existing immutable `LinkKey`
(`authority = package hash domain separation`, `id = ordinal`), never the mutable package title,
salt, sequence, or catalog entry index.

A small value type such as
`ReaderPackagePreference { bool retained; bool enabled; ReaderEmphasis emphasis; }` can replace
`map<InfoHash, bool> choices`. Keep the verified payload cache (`map<InfoHash, LinkPackage>`)
separate. Add direct preference lookup plus setters that persist atomically and return `expected`;
`setEnabled(true)` still requires the exact payload to be present and verified. Expose retained
identities independently from drawable links so the UI can list a retained-but-unavailable item with
an explicit “payload unavailable; fetch/verify to restore” state. Do not make `links()` allocate on
a frame path; cache the enabled key list or provide a view/range over stable entries, rebuilt only
when preferences or payload cache change.

`retained` is intent, not evidence. Payload availability has at least three states: no retained
preference; retained identity with no local verified payload; retained identity with verified
payload. Only the third state may be enabled or resolved. Missing bytes must never be represented by
an empty `LinkPackage`, because that would conflate absence with a verified empty package. A fetched
package is accepted only after the current full torrent hash and package review checks pass. A
disabled pinned `LinkKey` remains addressable through its persisted carrier identity and selection
context; it can show its disabled state and be re-enabled once the exact verified payload is
recovered.

Write format 2 with the same bounded count, byte limit, symlink refusal, atomic partial/rename
discipline, unknown-field rejection, and explicit format-specific diagnostics. Reject format 1 as
unsupported; do not reinterpret its Boolean map or provide a compatibility reader. The first
successful format-2 write replaces the whole file only after the new encoding is complete.

## Quiet presentation seam

Treat `normal`/`quiet` as a reader presentation multiplier applied after semantic color selection.
In `Session::decorate()`, apply it to the packed RGBA alpha of package `SpanStyle::colour` after
`linkColourWithInstanceShift(..., ProminenceTier::Curated)`. `SpanStyle` already carries RGBA8 color
and edge/background emphasis but no separate opacity field
(`include/gleditor/span_decorator.hpp:32-65`), so this needs no library API expansion. Preserve the
RGB hue and `ProminenceTier`.

For beams, add a compact emphasis value to the selected package strand description, or keep a
`LinkKey -> emphasis` presentation lookup alongside the strand. Multiply the already computed
`docAlpha` / packed-color alpha by the quiet factor before active/inactive styling at the draw seam
(`beams.cpp:2018-2025`). Active selection should remain legible: apply quiet before the
selected-link visibility boost, while retaining the same link colour and geometry. All enabled
package links already contribute strands. Cache the package's quiet bit into each `Strand` while
`presentationLinks()` rebuilds it; the frame draw reads that bool and
`BeamConfig::packageQuietAlpha`, with no per-frame preference lookup. The current selected-link
boost (`colour | 0xFFU`) then keeps an active ribbon at full alpha while retaining its RGB and
semantic tier.

Avoid per-frame allocation by building stable preference and package-link presentation tables when
`ReaderLinkPackages` changes, then invalidating Session decorations and the beam description once.
The existing `Session::decorate()` is epoch-cached, but currently constructs a `vector<LinkKey>`, a
`vector<PackageDocumentView>`, and a `unique_ptr<PackageOccurrenceIndex>` on each cache rebuild
(`session.cpp:3046-3053`). A change intended to avoid frame allocations should store/reuse the
occurrence index or build it at the same view-generation invalidation boundary; merely adding a
quiet lookup inside the loop does not address those existing allocations. The render loop itself
should read immutable preference values and never persist them.

## Implementation boundary and acceptance notes

Keep the preference file private reader configuration under `xanadocsDirectory().parent_path()`, not
in authored `Store` operations and not in the publisher's `Publication` or `LinkPackage`. These are
reader-local choices; publishing them would confuse one reader's visibility and emphasis with link
authorship. Continue to use `LinkKey` as the navigation and presentation identity so package
authority collision checks, pinned disabled context, and activity records retain their present
semantics.

The UI should expose retain/remove, enable/disable, and normal/quiet as separate actions or choices.
Remove is recoverable: persist `retained=false` and `enabled=false` while preserving emphasis; do
not delete the preference record. A missing payload should show retained identity and retrieval
state, with enable unavailable until verified. The removed inventory keeps the carrier hash
available for restoration when its verified status is present. A disabled pinned selection stays
visible with its existing disabled treatment and re-enable action; dismissing the selection does not
change preference state.

No product code was changed and no tests were run; this is a source/architecture plan only.
