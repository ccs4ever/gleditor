# Store telescope: remote discovery design study

Design proposal, 2026-10-13. These images are generated concepts, not running screenshots. No
application code changed. Names, identities, editions and counts are illustrative.

## 1. Discover and inspect

Image: [Discovery telescope](01-discovery.png).

The telescope is a suspended elliptical aperture with a bounded fan of six publication cards. Topic,
followed-author and recent-local docks occupy its left edge. Selection pulls one card forward and
unfolds a calm identity inspector on the right; the reading place stays retained. A synchronized
list exposes the same records without requiring depth navigation.

The example distinguishes 18 discovered records from six visible previews: eight more resolved
results and four awaiting metadata remain compact packs. Three pending channels describe search
coverage, not extra publications. Found-so-far counts never claim an exhaustive global catalog.

The discovery guide is a view association. It must be seamed and neutral, with a label such as
“Discovered under this topic”; it asserts neither stored ancestry nor an explicit content link.
Spatial placement has no implied relevance or semantic-distance metric. Grouping by author/topic
must not collapse different publication identities or editions into one subject.

The inspector separates catalog signature, publisher key, independent author trust, payload
integrity and current availability. A signed catalog does not prove the payload has arrived or that
a display name has been independently authenticated. Unknown availability is not zero peers. A
publisher-provided preview remains labelled unverified.

**Download pinned edition**, **Save reference**, and **Follow author** are independent actions.
Selection downloads nothing. Acquiring or opening does not subscribe. The abbreviated identity opens
a full copyable identifier; a title or matching text is never the identity.

## 2. Acquire, verify and open

Image: [Acquisition and opening](02-acquisition.png).

The same Spatial notebooks publication, publisher key and V12 request remain pinned through both
states. An interrupted transfer retains its subject and progress with explicit retry and cancel. The
progress display measures verified content pieces, not total acquisition progress. Manifest, history
receipt, content-piece verification and complete-store validation remain separate gates.

The unresolved page/cell silhouettes are seamed placeholders, not authored objects or usable
content. Receipt of a history carrier is not a claim that store validation has passed. Only the
complete validated state enables **Open pinned edition**. Opening explicitly changes the reading
place and retains a return point through the navigation/activity contract.

A newer publication must be offered separately rather than replacing V12 in the active request. A
cached verified edition may open offline. Follow/update subscriptions and third-party link layers
remain separate choices, with no layers enabled by acquisition alone.

## 3. State and input contract

| State                        | Visible behavior                                                             |
| ---------------------------- | ---------------------------------------------------------------------------- |
| Empty completed search       | “No matching publications”; offer another topic, author or URI.              |
| Searching                    | Found-so-far coverage, pending channels and Cancel search.                   |
| Metadata pending             | Retain publication identity; no fabricated preview or verification.          |
| Unavailable peers            | Distinguish unchecked availability, no current peers and transport failure.  |
| Invalid signature or payload | Preserve request identity, explain failed gate; opening disabled.            |
| Interrupted transfer         | Retain verified progress where supported; explicit Retry and Cancel.         |
| Ready offline                | Name exact cached edition and successful complete-store validation.          |
| Dense results                | Counted packs, windowed fan and synchronized searchable list.                |
| Reduced motion               | Immediate unfolding and stable framing with the same selected subject.       |
| Keyboard / accessibility     | Same channel, result, inspector and acquisition commands with visible focus. |

Escape closes inspection or returns to the telescope without discarding a retained request.
Cancelling a request and removing its cached data require separately described effects. Save
reference writes only to the reader's collection, with exact publication identity and state. The URI
input must accept supported publication identifiers; free text must not pretend that unsupported
author/topic discovery succeeded.

## 4. Existing foundation and proposed additions

The current [SwarmTelescopeOverlay](../../../apps/common/ui/xanadoc/swarm_telescope_overlay.cpp)
already presents search, category/channel, publication and inspector controls, a
selected-publication action, asynchronous catalog results and narrow-screen pages. Its existing
verification label needs careful mapping to the separate signature/payload/trust states proposed
here.

The [download validation](../../ux_publication_download_validation_2026-10-05.md) records a
known-publication URI acquisition path, complete-store verification and retained offline snapshots
in an isolated local DHT. That evidence does not establish a production topic or author catalog.

This study proposes the spatial aperture/fan, provenance wing, detailed coverage/status
presentation, publication reference saving and integrated transfer controls. Author discovery,
following, subscription behavior and piece-level retry/progress must be verified against current
services before these pictures become implementation promises. The pictured wireframe-to-preview
transition does not require incremental entry into an incompletely validated store.

Reuse the spatial plan's placed-plane/picking/material seams and the same models for list and
spatial views. Resolve network metadata and preparation outside draw work. Measure layout and
visible bounds rather than treating illustrative counts as performance evidence.

## 5. Visual review

The images successfully reuse the forge/pouch glass thickness, silver hinge treatment, bowed
surfaces, restrained violet accents and calm reading faces. The selected store and acquisition
actions have clear hierarchy; the source companion and explicit return remain visible.

Before implementation, correct these image-level discrepancies:

- Discovery has a solid violet topic conduit. Use a neutral seamed discovery guide, distinct from
  authored link colors and ancestry.
- The discovery fan displays publisher thumbnails without per-card unverified labels. Each preview
  needs its own provenance status, not just the selected inspector's label.
- Recent-local examples include pouch/draft-like names. Real rows must represent reopenable local
  stores, with type and location rather than ambiguous collection objects.
- The first caption's “Open only after verification” refers to acquired remote payloads; it must not
  prevent reopening an already validated local snapshot.
- The acquisition heading says transfer is active while the detailed status says interrupted. An
  implementation must use one consistent interrupted state.
- Ready preview cells are illustrative and do not show a topology contract. Actual cell previews
  must preserve recorded dimension neighbors rather than inventing spatial connections.

Gold remains reserved for actual shared primedia; incidental warm reflections in the discovery
background convey no relation. Optical highlights must stay outside readable glyphs. The images
establish design intent only; contrast, picking, accessible focus, backend rendering, cold discovery
and restart/retry behavior still need implementation validation.

Exact generation inputs and prompts are in [prompts.md](prompts.md).
