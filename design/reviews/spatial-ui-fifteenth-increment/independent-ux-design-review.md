# Fifteenth spatial UI increment: independent UX design review

Read-only UX review at `d41664a3ba2f57ddb1e08fd082cf10a40bf67185`. This review uses the fourteenth
checkpoint, the thirteenth publication-pin lifecycle audit, J23/J24, the Xuzz UI design and UX
validation contracts, and the current review form and `ReaderLinkPackages` implementation. No
product code or native session was changed or exercised.

## Finding

The next increment should give the reader three separately visible, carrier-specific preferences:
**Retain**, **Enable**, and **Emphasis (Normal / Quiet)**. Keep these controls in the existing
“Review independent links” form, beside the selected immutable package and Link identity. Do not
make opening or inspecting a downloaded package retain it or enable its links. The exchange cache
answers whether verified bytes are available; the reader inventory answers which immutable packages
the reader chose to keep in their working set.

The current form says “reader layer enabled/disabled” and offers a single toggle. The code persists
`InfoHash -> bool` in `package-visibility` format 1, reconstructs the in-memory retained package map
from exchange statuses, and only offers navigation for enabled packages. That cannot represent
independent retention or emphasis, and it couples durable inventory to acquisition cache state. The
fourteenth checkpoint correctly preserves a disabled selected Link's context, so the new form should
extend that same pinned context rather than replace it.

## Recommended form and state model

Keep the selected Link selector and both ordered endsets in view. Add three adjacent, explicit
controls, each reporting its current value and using its own user-facing name:

| Control          | Values and effect                                                                                                                                        | User explanation                                                      |
| ---------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| Retain in reader | On / Off. On places this exact package in the reader's recoverable inventory. Off moves it to the dismissed inventory.                                   | “Retained packages stay in your reader inventory even when disabled.” |
| Enable links     | On / Off. On permits the package's links to draw, be picked, and enter navigation. Off suppresses its beams and picks while keeping the pin inspectable. | “Enable controls whether these links participate in the view.”        |
| Emphasis         | Normal / Quiet. Applies only to this package's link presentation while enabled.                                                                          | “Quiet lowers visual priority; links remain available from the list.” |

These are independent persisted preferences keyed by the full immutable package carrier `InfoHash`,
never by title, curator name, sequence alone, a mutable catalog entry, or Link ordinal. Enabling
requires the exact verified package bytes to be available and the package to be retained. A package
can be retained but disabled; its emphasis remains remembered for later enablement. A quiet package
remains selectable from the link list and retains readable text and accessible names. Emphasis does
not rewrite `GlobalLink::tier`, signed package bytes, source documents, or the selected Link's
identity.

Present identity close to the controls: package title and sequence, full carrier hash available in
the inspection affordance, curator key, signature result, and enrollment state. Keep signature
verification distinct from enrollment and from payload availability. For an unavailable exact
carrier, show its pin and the reason it cannot currently be enabled; never resolve a newer package
with a matching name as a substitute.

The control group should remain in the existing form, not create a new modal workflow. This form
already keeps selected Link identity, both endsets, curator and cited publication together. Use the
existing choice/action widgets and theme metrics; expose accessible labels such as “Retain package
`<title>` hash `<prefix>`”, “Enable links from package …”, and “Package emphasis …”. Pointer,
keyboard and accessibility actions should call the same preference commands and leave the current
Link selection and member cursors intact. A preference change must not enter an endpoint or append a
reader visit.

Keep these actions reachable from the visible form and register their named commands and default key
bindings through the existing system keymap. Retain, Enable/Disable, Normal/Quiet, Remove from
reader and Retain again must all reach the same command boundary from pointer, keyboard and
accessibility input. In particular, Enable on an unretained carrier must refuse; the visible Retain
control is the explicit preceding action.

## Reader inventory and exchange cache

Provide two distinct package-list destinations from the existing package review entry points:

- **Reader inventory** lists retained packages and their Retain, Enable and Emphasis values. It
  includes retained packages even when their exchange job is no longer present or their verified
  bytes are temporarily unavailable; those rows remain diagnostic pins and can be retried when
  content becomes available.
- **Downloaded packages** lists exchange jobs and cache states such as resolving, verifying, ready,
  failed or cancelled. Inspecting, downloading or verifying a package does not add it to Reader
  inventory. From a verified package's existing review form, the reader can explicitly retain it.
- **Removed packages** is a recoverable view over persisted preference records whose Retain value is
  off, separate from both lists. It shows the exact carrier hash and remembered Enable/Emphasis
  values, with “Retain again” and “Forget removed choice” actions. Retain=false disables the package
  but preserves the other carrier-specific choices; Retain again restores the same immutable
  carrier's choices and never silently enables a newer carrier. If exact payload bytes are
  unavailable, the record remains named and inspectable as an unavailable choice.

Use counts only after the inventory distinction is visible: “Reader inventory: N retained, M
enabled” and a separate downloaded-cache count. Include a named unavailable count so a missing
payload is not silently omitted from the retained-choice ledger. Do not label exchange statuses
“Retained packages.” In J23, merely reviewing Lab review #13 leaves it out of retained counts;
explicit Retain adds that exact carrier and its nine links to retained inventory. Disabling affects
enabled counts without removing those links from retained counts. Dismissing moves the carrier out
of active retained counts while preserving a visible recovery record.

## Remove from reader versus Dismiss selected

These actions have different scope and should not share a label or handler:

- **Remove from reader** is a package-level inventory action. It turns Retain off and moves the
  exact carrier's Retain choice to Off. It disables the package, stops its links participating in
  drawing and picking, and preserves its Enable/Emphasis choices in the Removed packages view. It
  does not delete downloaded exchange bytes, signed content, source operations, or visit history.
  The row remains recoverable.
- **Dismiss selected link** is a navigation-context action on the currently selected Link. It clears
  that Link's selection/context from the reader view. It does not change package retention,
  enablement, emphasis, exchange cache, or authored relations. It creates no visit. The package and
  the same Link remain findable from their lists.

The existing navigation label “Dismiss selected link” should stay scoped to clearing selection. In
the package review form, use “Remove from reader” for inventory removal and state its effect in the
confirmation/status text. Do not use bare “Dismiss” for the package action: users could reasonably
read it as clearing the selected Link context, which is the established navigation meaning.

Removing a package that owns the currently pinned Link should preserve the disabled pin until the
reader explicitly chooses “Dismiss selected link” or selects another Link. This lets the reader
understand why the beams disappeared and recover by restoring/retaining and re-enabling the same
carrier. If the model cannot represent a dismissed-but-pinned transient state, keep the selected
Link context readable with a clear “Removed from reader; drawing disabled” explanation instead of
silently clearing it.

## Persistence and refusal behavior

Bump reader preference storage from format 1 to format 2 because the durable shape now carries
separate Retain, Enable, Emphasis and recoverable removed-choice state. Follow the repository's
format rule: write the new shape, remove the old reader, and refuse format 1 by name and version. Do
not add an implicit migration or shim. Preserve the old file when refusal occurs and report that
reader package preferences are unreadable, including the expected and observed format numbers; never
silently reset the reader's choices. An explicit user-facing recovery/export path can be designed
separately if required, but is not a hidden compatibility reader.

Format 2 should encode separate `retained`, `enabled`, and `emphasis` choices keyed by exact
immutable `InfoHash`; a retained=false entry is the recoverable removed choice. Link context
identity remains the hash-derived package authority plus ordinal, so retaining a newer carrier with
the same title or curator cannot replace or inherit the selected Link identity. Persist each
preference transition atomically as the current writer does. Bound retained and removed choices
consistently, and make failure to persist leave the visible controls at their previous values with a
diagnostic.

Package acquisition/cache state must not be the source of truth for Retain. A cache eviction or
exchange-job cleanup must leave the reader inventory record intact. Conversely, a downloaded package
is not retained until the reader acts. If payload bytes are unavailable, keep the preference record
and exact identity visible with a named unavailable count while disabling actions that require
payload inspection or navigation. Enabling a carrier with retained=false must refuse with a direct
message; it must not implicitly retain it.

## Journey and purity criteria

J23 should exercise all three controls independently, inspect a disabled package without enabling
it, keep package #12 enabled while reviewing and retaining #13, close and reopen, then reconcile
retained, enabled and downloaded counts from the carrier-keyed ledger. Quiet must reduce visual
priority without making text unreadable or removing list access. An unchecked enrollment state must
remain distinct from a valid signature.

J24 should disable and then remove/dismiss the selected package while its Link is pinned. The reader
must retain the exact carrier hash, Link ordinal, both complete endsets, side cursors and origin
context until the explicit selected-Link dismissal or a deliberate new selection. Restore the
package and re-enable that same carrier; do not substitute by mutable name. A disabled, quiet or
dismissed package must not intercept pointer picks. Unavailable targets and cancellation must not
append visits.

Across Retain, Enable, Emphasis, Remove from reader, Restore, Forget dismissed record, and Dismiss
selected link, verify that visited source documents and slices receive no operations and that
`system://activity` receives no new completed visit. Only an explicit successful endpoint Enter may
append the reader's completed-visit record. Preference changes and inventory browsing are
reader-local state.

## Recommendation

Proceed with the existing review form as the single place to inspect package identity and govern
Retain, Enable and Emphasis. Add explicit Reader inventory, Downloaded packages and Dismissed
packages lists so acquisition, reader choice and recoverability are legible. Make package removal
recoverable and keep “Dismiss selected link” reserved for navigation context. Format 2 should encode
those distinctions and reject format 1 loudly, preserving the refused file. This is a design
recommendation only; it does not establish native J23/J24 acceptance or validate rendered controls.
