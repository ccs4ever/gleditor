# Independent native UX and visual review

I reviewed the final frozen candidate frames 04–08 at their original 1440×1000 pixels, the matching
accessibility output and exact argv, the before/after activity decode, and baseline frame 05. The
archive's PNG derivatives were verified lossless against the original PPM RGB data.

The final failure form fits the status message, action field, verification, exact carrier hash,
edition, publication name, publisher field and bottom keyboard help within the captured window. The
labels are readable at native size, and the carrier and edition strings are not truncated. The
status clearly says “Not checked” and “No publication connection is configured. Content availability
has not been checked.” This is a concrete configuration failure; it does not imply that a peer was
reached or that the content is unavailable.

The pre-retry frame `04-open-failed-before-retry` and its a11y dump show the failed status and
`Retry download` in the Action choice before the recorded Right+Enter. The immediate post-action
frame `05-retry-immediate` shows `Queued` while retaining the same carrier, edition, publication
name and publisher. After Refresh progress, frame `06-retry-result-refreshed` returns to the same
not-checked no-connection failure. The screenshot keeps the Action choice collapsed on
`Refresh progress`; the accessibility output includes the available `Retry download` option. The
retry is explicit in the recorded input and state transition.

The review frame `07-review-after-failure` correlates the same `document #2 / a34` citation with
`Request failed; open status`. Selecting it again leads to the same failed pinned status in frame
`08-same-citation-reopened`, with no queued/fetching state. Source code confirms full-pin
de-duplication and that this review selection opens the existing status without invoking Retry.

Trust and availability remain separate in both the interface and the evidence. The carrier review
says its signature is verified and its curator enrollment is unchecked. The cited publication status
separately says verification is `Not checked`; the failure explicitly says content availability has
not been checked. Nothing in these frames borrows the package signature as proof of the cited
publication's signature or content.

The baseline frame `05-citation-open-refreshed` showed only “No publication swarm configured. Set up
the local test swarm, then retry.” The final form improves the user-facing diagnosis and carries the
immutable citation identity through retry and review. One visual limitation remains: the publisher
key field displays its first 32-hex-character segment at once; its dropdown and a11y tree expose the
second segment separately. The carrier hash, edition and name are fully visible, but a reader cannot
visually scan the full publisher fingerprint in one line.

The final before/after activity decode is identical: one pre-existing visit, current visit 1,
unchanged visit payload, no references and no annotations. The source document's operation dump,
`ops.nodes` and `store.tables` hashes also match. The activity operation dump changed during
shutdown reading-place metadata, while decoded visit state remained unchanged.

Reviewed frames and corresponding source PPMs:

- `build/validation-seventeenth/native-candidate-final/raw/04-open-failed-before-retry.png`
- `build/validation-seventeenth/native-candidate-final/raw/05-retry-immediate.png`
- `build/validation-seventeenth/native-candidate-final/raw/06-retry-result-refreshed.png`
- `build/validation-seventeenth/native-candidate-final/raw/07-review-after-failure.png`
- `build/validation-seventeenth/native-candidate-final/raw/08-same-citation-reopened.png`
- `build/validation-seventeenth/native-baseline/evidence/05-citation-open-refreshed.png`
