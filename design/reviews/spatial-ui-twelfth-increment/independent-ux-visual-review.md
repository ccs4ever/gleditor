# Independent UX and visual review

I reviewed the candidate source diff and the archived native restart evidence from the reader's
seat. I did not change product code or run the native application. The native candidate's two core
processes and frame captures are recorded in
[`native-restart/README.txt`](native-restart/README.txt); this review independently inspected the
closed-target frame, activity/source snapshots, and frame verification record.

## Finding

**No blocking UX or visual issue found in the exercised closed-target recovery path.** The saved
Left occurrence remains the exact `b1` document site across fresh resolution and process restart.
When `b1` is closed, the Inspector retains its store authority, version, byte range, and reference
failure. Enter reports that the exact saved version must be opened and offers Cancel; the attempt
does not add a visit. The existing Hypertime Map control reopens `b1`, after which explicit Enter
reaches the pinned bytes and adds exactly one visit.

## Visual evidence

I inspected
[`11-identity-closed-page1.png`](native-restart/candidate-v3/frames/11-identity-closed-page1.png) at
its native 1440×1000 resolution. The panel shows both endpoint cursors and the selected Link, then
identifies the Left occurrence as local store `33db7f71defbaeb61f4b64f00540d85d`, state `b1`, bytes
0–20. It states that the exact source reference is unavailable because the matching frame is closed.
The Right occurrence is separately identified as `a34`, bytes 55–65. This makes the saved target,
its unavailable state, and the independent opposite-side choice legible without relying on colour
alone. The `Back`, `Enter`, `Origin`, `Cancel`, and link navigation controls remain visible. The
panel uses the existing dark surface and blue link/status treatment; the status and identity lines
fit at this capture size without overlap or clipping.

The main refusal frame,
[`07-unavailable-closed.png`](native-restart/candidate-v3/frames/07-unavailable-closed.png), shows
the exact-version instruction and Cancel beside the selected context. The reopened frame and retry
frame are also archived. Each archived PNG is checked against decoded RGB from its PPM source in
`candidate-v3/frame-verification.tsv`.

The known-place count includes the retained historical site: after restart the selected Left place
is shown as 3/3, labeled “a closed version,” while the two currently resolved places are cell 24 and
the document. This is understandable with the adjacent closed-state label and matches the chosen
known-place snapshot semantics. Keep that distinction explicit in future UI wording; a place in the
count means previously observed, not currently open or enterable.

## Behavioral evidence

The candidate's saved activity uses `visit3` and `selection3`. Its closed-target checkpoint retains
two visit records and 73 activity operations. The refused Enter, map open, and exact `b1` reopen do
not add a visit. Successful retry moves from two visits to three, appends one visit record targeting
`b1`, and leaves the source dump hash unchanged at
`49a12edef0113d9b6f6b7a120c8f2f7cbf7dd1f6d865e8606dc7dfaac4a164d6`. This agrees with the contract
that only completed focus transitions create visits and visited stores receive no navigation
operations.

The frozen native candidate hash differs from the final rebuilt app hash. The archived
[`native-final-elf-equivalence.json`](native-final-elf-equivalence.json) reports all allocated ELF
sections equal except the build-ID note; the native result therefore belongs to the frozen binary,
with product sections matching the final rebuild.

## Scope limits

This is a bounded J25 check, not a full navigation acceptance. It covers a fresh v3 profile, one
closed document endpoint, refusal, exact-version reopening through the visible map, and retry. It
does not cover returning to an earlier branch and choosing among every retained forward child, all
saved-side combinations, exact cell-history recovery, remote or locked packages, naturally
interruptible pending pages, native AT-SPI, other native backends or scales, or the complete J24/J25
journeys. The optional Inspector page capture contains valid pages 1/2 and 2/2; its extra request
for disabled page 3 exited 1, so I do not treat it as a successful pagewalk. The native frames are
OpenGL at one size; no cross-backend visual claim follows.

The source review separately records a remaining semantic validation boundary: decoding checks the
pin's numeric shape, and entry validates a cell range against its content, but the saved cell range
is not checked against source content before the historical pin is shown as visible/highlightable in
the exact bound frame. That concern applies to malformed persisted pins; it does not affect the
valid resolver-authored document target exercised here.
