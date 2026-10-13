# Spatial journey prototype: proposed contracts

This is a scripted design prototype for J17–J32, not the Xuzz implementation. These decisions make
the missing transitions reviewable. All stores, receipts, keys and bytes below are fixture data.

## Identity and effects

Every subject has a fixture ID, authority/store, object, pinned state, ordered source ranges and a
separate occurrence or media locator. Version labels are local to that authority. Inspection,
folding, filtering, correspondence and media alignment never add a completed visit or source op.
Explicit Enter creates one reader visit; Resume/Back/Forward restores an existing visit without
adding one. Explicit Save/Annotate writes only the reader's collection/activity metadata.

Source content, reader activity, reader collections, private staging and authored destination are
separate effect domains. Every action shows its proposed effect record. The prototype does not
assert that current production staging is entirely write-free; its simulated staging is in memory.

## Query rules and lifecycle

Literal uses a substring with optional Unicode case folding, without numeric normalization.
Whole-token treats a decimal run such as 3.140 as one textual token, so it does not equal 3.14.
Number parses finite decimal literals exactly: 3.140 equals 3.14. Units remain typed groups; there
is no implicit conversion or equality between a unitless mathematical context and a pressure
context. A unit facet explicitly chooses those groups. Pi is not exactly 3.14. Related ideas
requires an explained alias/suggestion expansion and never creates source sharing.

A response belongs to a request ID and fixture generation. Superseded responses are logged and
ignored. Sorting/grouping never replaces the selected occurrence. If a filter excludes it, its
inspector stays pinned with Restore filter and Clear selection. Cancel retains found-so-far results
and partial coverage. Completed-empty, incomplete-zero, unsupported, missing state and locked are
different statuses. A saved recipe and a captured result snapshot are separate records; rerun is
explicit and does not populate a pouch or revise Link membership.

## Publication and package choices

A download pins publisher, immutable publication/manifest, edition and request ID. A mutable pointer
moving to V13 does not replace V12. Metadata, signature, payload validation, independent identity
trust and availability remain distinct. Invalid payload never enables Open. Ready means the declared
fixture's full store validation completed. Acquisition neither follows an author nor enables link
packages.

A retained immutable carrier has independent Enabled and Emphasis preferences. Quiet remains
inspectable through the link list. Unchecked signatures may be inspected but cannot be enabled until
required package verification succeeds; invalid signatures stay blocked. A candidate #13 has nine
links, is unretained initially, and never replaces enabled #12 automatically. Retaining it changes
inventory from 85 to 94. Switching disables #12 and enables #13 explicitly; a pinned old Link
retains its old carrier identity and explains its disabled drawing.

## Typed references and duplicate policy

Whole-cell, selected-content, operation, recipe, captured results, walk and draft descriptors keep
their own dispatch. Only validated concrete content spans may be offered as forge endpoints;
non-content locators require an explicit supported representation and are excluded here.

An exact duplicate descriptor reveals the existing saved reference by default. Keep a separate card
is an explicit alternative with a new membership ID. Equal text at different addresses is always
distinct. Shared addresses with different occurrences remain separately inspectable; no implicit
merge changes an occurrence or captured state. Dismiss moves reader membership into recoverable
dismissed items, never deletes the subject. Query refresh never changes curated members.

## Forge commit and unknown outcomes

The fixture authoring mode is Create child at pinned base, not append at an implicitly selected
head. The destination store/branch/base and author signing identity are pinned before commit.
Required reference identity, write permission, signing readiness and destination/base validation are
gates. An optional missing preview is not a required-identity failure.

Each attempt has a stable token binding draft ID, ordered members, relation, prominence,
destination, base and mode. Same-attempt retry checks that exact intent. Editing the payload after a
resolved attempt creates a new intent/token. Editing or blindly forging while an outcome is unknown
is blocked. An unresolved outcome must not become a fresh token merely because the user retries.

Proposed protocol: preflight; prepare destination-private unpublished operations; validate the
complete operation set; publish one receipt exposing one Link and one new child state. Staged
operations are not presented as a completed authored Link. This is a recoverable publication design
to validate later, not a claim of proven production atomicity.

A preflight failure is confirmed uncommitted and preserves the draft. A failure before publication
leaves a recoverable staging record; status resolves uncommitted before same-token retry. A lost
reply after publication is Outcome unknown, with Forge/Retry disabled and Check status available.
Check status returns Created with the existing Link receipt, Confirmed uncommitted with safe
same-token Retry, or Status unavailable with continued blocking. Created offers Open existing Link,
not Forge again. Discard removes only a resolved draft; an unknown attempt must be reconciled first.

## Recovery

Recovery uses the same explicit attempt/status contract. Its payload names exact historical source
ranges, source occurrence/state, receiving store, pinned base, author and Create child mode. The new
state retains all chosen base content and transcludes only the selected historical material.
Existing futures remain unchanged. Source A V27, target base B V31 and old tip B V40 are distinct.

If the receiving head advances, V31 remains fixed. Keep V31 authors a separate child there; Choose
new base makes an explicitly reviewed new draft before any attempt. No unavailable source, invalid
author/destination or unknown outcome permits commit. Cancel preview authors nothing. Returned
branch/state identity and source addresses remain available after simulated reopen.

## Manual cues, history and captured pages

Cues are reader-owned viewing descriptors with separate video/audio clocks, start/end bounds, image
geometry and passage ranges. Playback is explicit. Unsupported seek bases require correction or
retaining an unavailable cue, not inventing a time range. Save cues does not forge a Link.

Loom ancestry uses declared parent IDs. Same-op projections retain their source placement and
identity; filtered order is not ancestry. Effects name the owning frame and parent-state footprint.
Original source freezes ranges; Evolving passage includes insertions only under visible interior and
boundary rules. Own edits and affected topology use different scopes. Captured page contents retain
ordered ranges, snapshot and layout; later pagination never replaces that footprint.

## Representation boundary

The browser prototype supplies linked states, native focusable controls, list/keyboard equivalents,
narrow/enlarged/reduced-motion representations and a simulated session/effect record. It still does
not validate Xuzz input delivery, actual assistive technology, cryptography, networking, durable
stores, GPU/backend behavior or throughput. Those remain live-implementation tests.
