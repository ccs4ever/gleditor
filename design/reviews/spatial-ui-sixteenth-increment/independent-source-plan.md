# Independent source plan: immutable package succession

Read-only audit at `49b82077`. This plan identifies the smallest next package-lifecycle increment
from the existing implementation. It does not propose a network mock or claim native acquisition
evidence.

## Finding

The immutable identity and reader preference model is already sound for the requested lifecycle:
`ReaderLinkPackages` keys choices and synthetic Link authority by carrier `InfoHash`; format 2
persists retained, enabled and emphasis separately. Caching a later same-name package does not alter
the earlier carrier's preference. Navigation pins a selected Link by its exact `LinkKey`; disabling
keeps the selection, endsets, cursors and origin, refuses Enter, and permits explicit re-enable or
dismissal. The fifteenth checkpoint and `tests/zigzag/reader_link_packages_ui_test.cpp` cover these
behaviors.

The gap is the comparison and trust/availability presentation around that model.
`Views::linksAndResponses()` lists candidates and the reader inventory, and
`reviewIndependentLinks(id)` reviews one exchange job at a time. It exposes title, sequence, carrier
hash, curator, signature statement, link/endsets and citations, plus Retain, Enable, Emphasis and
Select for navigation. There is no paired same-name old/new comparison and no explicit “switch to
this immutable carrier” action that makes the transition and unchanged old preference/pin legible.
The current separate review/navigation controls can technically reach a new carrier, but they do not
express succession as a deliberate choice.

Trust has a related display seam. `LinkPackageStatus.identity` is populated by the optional identity
callback only on the outgoing announce path (`LinkPackageExchange::step`); incoming catalog fetches
leave it `Unknown`. Incoming transfer reaches `Ready` only after seed hash, canonical decoding,
package signature, payload and advertised catalog identity checks. Failures become `Failed` with an
error string. The UI currently presents “signature verified; enrollment unchecked” in the verified
endpoint review, but lacks a compact explicit distinction among identity unknown, signature/payload
invalid, and a valid package whose cited publication is not locally available. A citation is a
signed `PublicationPin`; opening it launches a separate exact pinned
`PublicationInbox::submitPinned()` request. Package `Ready` therefore does not mean cited
publication bytes are available.

## Smallest implementation seam

Keep `InfoHash` as the sole immutable carrier identity and keep the persisted preference format
unchanged. Add a succession comparison/action to the existing reader inventory or package review
flow, using the existing `LinkPackageStatus` records and `ReaderLinkPackages::preference(hash)`
values. Group candidates by the existing package name/curator identity, but compare and act on
explicit carrier hashes. Show old and candidate sequence, full hash, signature/payload result,
identity state, and retained/enabled/emphasis state. The action should be explicit and
candidate-specific: open/review the candidate and select one of its Link ordinals for navigation
only after the user chooses it. It must not copy preferences from the old hash, silently enable the
candidate, rewrite the old choice, or retarget an existing `LinkKey`. Preserve an already pinned
selection until the user explicitly selects a different Link or dismisses it.

Represent the display facts at the existing status boundary instead of inferring them from the phase
label. `Unknown` identity should remain “identity unknown / enrollment unchecked”; verified signed
bytes should say signature and payload valid; invalid signature/canonical payload remains a failed
candidate with its reason; citation acquisition should report the exact `PublicationPin` and
separate inbox status (unrequested, unavailable/failed, or locally available). Do not equate
identity with signature validity. Avoid a new network service or a new persistence format:
`LinkPackageExchange` already supplies candidate status and exact carrier; `PublicationInbox`
already supplies pinned citation acquisition status.

The acquisition/display classification must not let an invalid package appear retainable. Existing
review gates already restrict Retain to `Ready` or `Published`; keep that condition. Unknown
identity on a received package is an enrollment fact and must not be rendered as invalid signature.
Conversely, a valid curator signature does not establish identity enrollment or truth.

## Owning regressions

Add focused coverage at existing owning seams:

- `tests/xudu/reader_link_packages_test.cpp`: two same-name packages at later sequences have
  distinct keys; retaining/enabling/quieting the later candidate leaves the earlier carrier's
  retained/enabled/emphasis tuple byte-for-byte/state-wise unchanged. Reopen preferences and confirm
  both exact tuples survive independently.
- `tests/zigzag/reader_link_packages_ui_test.cpp`: with old carrier selected and pinned,
  inspect/compare a later same-name candidate. Before explicit switch, old `LinkKey`, endsets,
  cursors, Origin, current visit, activity operation count and source store operation count remain
  unchanged. Choosing a new candidate Link changes selection only at that explicit action; the old
  carrier remains retained/enabled/emphasis as before. A disabled or unretained new carrier cannot
  become the implicit replacement.
- A view/form regression at the existing links-and-responses UI seam: candidate rows and comparison
  expose full carrier identity and separate retained, enabled and emphasis state; “switch” is
  explicit; unknown identity is not called invalid; failed signature/payload is not retainable;
  cited publication availability is tied to the exact pin and remains independent of package
  readiness. Exercise the status projections with actual exchange/inbox status values, not invented
  service responses.
- Preserve current coverage for exact retry, wrong-version refusal, selected-disabled pinning,
  restart restoration and explicit dismissal. Do not duplicate transport correctness tests to prove
  the UI wording.

## Evidence boundary

Unit tests can verify the classification and state invariants. Native proof of package succession,
unknown enrollment, bad signature/payload and referenced-publication availability must come from
actual controls and the real configured acquisition path. Do not use the existing test swarm, fake
transports, scripted journey data or fabricated status fixtures as native proof. Keep native states
isolated and record the exact candidate hashes, exchange status, citation pin/status, reader
preference before/after, selected `LinkKey`, activity/source operation counts and restart result. If
no real peer/source supplies a state, report it as unverified rather than synthesizing it.

Relevant existing evidence: `design/reviews/spatial-ui-fifteenth-increment/README.md` (independent
preferences and native carrier lifecycle);
`design/reviews/spatial-ui-fourteenth-increment/README.md` (disabled pinned Link);
`design/reviews/spatial-ui-thirteenth-increment/publication-pin-lifecycle-audit.md` (trust and
citation boundary); J23/J24 traces under `design/prototypes/spatial-journeys-2026-10-13/evidence/`
(same-name carrier lifecycle, complete Link identity, exact unavailable/wrong-version behavior).
