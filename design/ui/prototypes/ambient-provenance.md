# Prototype: Ambient Provenance

**Status:** Proposed on-demand explanation of a link and its content.\
**Depends on:** [selected link context](link-context.md) and exact occurrences.

## Reader walkthrough

1. A reader selects a link whose right member quotes primedia from another author's document into
   both a xanadoc and a ZigZag cell. The compact context shows the link's type, owner, curator when
   present, and prominence tier. The endpoint preview names its current document or cell and version
   without filling the page with metadata.
1. **Reveal provenance** expands a card for that endpoint. It distinguishes the primedia origin and
   authorship evidence, this manifestation's document/cell and version, and the link owner or
   curator. The reader can see that the two manifestations share the same address even though their
   surrounding contexts differ. A link created after an older manifestation is labeled as such
   rather than presented as an authored fact in that old version.
1. The reader closes the card, selects the opposite wing, and reveals its provenance independently.
   Neither action moves reading focus or changes the chosen endpoint. Enter still requires an exact
   occurrence; if origin metadata is unavailable, the card says what could not be verified and the
   link remains navigable when its content resolves.

## Technical explanation

Build provenance in layers. The stored [`Link`](../../../apps/common/xanadu/ops.hpp) supplies link
ID, type, owner, curator, prominence tier, and ordered primedia endsets. A selected occurrence adds
store authority, document or cell identity, version, exact range, and the `PrimediaSpan` that
matched. Scroll registry and publication/authorship records can supply original-source and signer
information when available. These are different claims: link ownership describes who asserted the
relationship; primedia provenance describes where addressed content came from; the current version
describes one manifestation. Prominence is a ranking signal, not a permission check.

Resolve only the requested card and cache by stable link/occurrence/version identity. Never log
document text or key material as routine diagnostics. An absent signature, unknown publisher, or
remote scroll is reported as unknown or pending, not inferred from the link owner. A card may show
an exact quotation relationship by shared primedia address; equal text without shared addresses is
not evidence of transclusion. The card is a presentation layer over the same session and adds no
operations or activity visit. Screen readers get separate labeled groups for link attribution,
content origin, and current manifestation; the sovereign keymap gets Reveal/Close provenance actions
without a fixed chord.

## Prototype proof

Use one span transcluded into a document and cell, one equal-text span at a different address, a
curated link, and a missing remote source. The card must group the first two manifestations by their
actual primedia source, keep the equal-text span distinct, and separate content author from link
owner and curator. Opening, switching sides, and closing preserve focus and activity count. Verify
pointer, keymap, and accessibility announce the same facts and unresolved state.
