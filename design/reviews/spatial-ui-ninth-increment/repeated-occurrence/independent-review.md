# Independent review: repeated quotation navigation

## Verdict

Pass for this bounded repeated-occurrence journey. The corrected run contains a real repeated
quotation, exposes its two occurrences through the accessibility tree, preserves both endpoint
choices while crossing between endsets, and activates the selected occurrence. Origin restores the
source reading place without adding another completed visit. The source document's operation dump is
byte-for-byte stable through all navigation checkpoints.

This is evidence for one native OpenGL run at 1440×1000, not a complete acceptance pass for all
navigation inputs, backends, or document arrangements.

## Journey and accessible state

The runner authors `Harbor left passage.`, selects bytes `[0,20)`, and invokes Ctrl+T. Its log
records `transcluded [0,20) of a1 into b1`; the resulting link has two left members and three right
members. Before endpoint choice, the a11y tree exposes `left member 1 of 2` with two distinct
occurrence items, and the second is selected as `occurrence 2 of 2`.

The captured keyboard sequence establishes left member 1, advances with L to occurrence 2/2, crosses
to the right endset, chooses right member 2/3, then crosses back. In the pre-Enter state, the
accessible labels report `Left 1/2 · place 2/2` and `Right 2/3 · place 1/1`; both chosen items
remain marked after crossing back. The visible panel's decoded RGB crop also remains present in the
pre-Enter, Enter, and Origin captures. For the same crop `(x=35..649, y=580..929)`, the three frames
have identical SHA-256 `8fd82a849f0a4851220678d951e400f0578286718dd511a8ef9e66e82b7d5f99`, with
76,078 pixels exactly `(30,41,59)` in each. This is a numeric pixel check against the archived PNG
files, independent of image previews.

Enter moves the reader to the selected destination: the activity record identifies store
`71d2d242cf32aca8f1461ecb09fe1c7b`, version `b1`, range `[0,20)`. It retains the chosen second left
occurrence and right member 2/3. The record's origin checkpoint points to version `a34`, range
`[0,20)`. Origin returns to that source context and leaves the same two endpoint selections
available.

## Activity and source-store evidence

The archived activity-store dumps distinguish visits from operations:

| Checkpoint                 | Activity operations | Completed visits | Current visit |
| -------------------------- | ------------------: | ---------------: | ------------: |
| Crossed back, before Enter |                  12 |                1 |             1 |
| After Enter                |                  16 |                2 |             2 |
| After Origin               |                  18 |                2 |             1 |

Enter adds one visit record, advancing the completed-visit count from one to two. Origin adds no
visit; its two operations update the current-visit pointer back to visit 1. Therefore the rise from
16 to 18 activity operations is not a third visit. The decoded visit record names destination
version `b1` and origin version `a34`.

The `source-0.ops.txt` SHA-256 is `49a12edef0113d9b6f6b7a120c8f2f7cbf7dd1f6d865e8606dc7dfaac4a164d6`
at every navigation and activation checkpoint (01–09). This supports the contract that live
exploration and activation write to the private activity store rather than the visited source
document.

## Scope

The evidence supports the repeated-occurrence selection, independent endset cursors, cross-back
retention, specific activation, and Origin return path. It does not exercise every input binding or
the broader real-work journeys. The archived `template-attempt` run remains invalid because it did
not contain a repeated quote; this verdict applies only to `valid/`.
