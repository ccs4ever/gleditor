# Independent Back/Origin pixel audit

This audit supersedes the earlier visual conclusion in
[`spatial-ui-eighth-increment/independent-review.md`](../spatial-ui-eighth-increment/independent-review.md)
that the Link panel was missing from some Back or Origin frames. That conclusion came from
misleading image previews. Numeric inspection of the PNG pixels shows the panel band is present and
identical in all reviewed captures. No panel-rendering defect is established by those images, and
the earlier recommendation to change panel rendering is withdrawn.

## Method and results

I decoded each PNG as RGB and compared the full panel band at `x=0..799`, `y=316..550` (800 × 235
pixels). The region's SHA-256 is `7e81dc163aa69b2a1930d5c0eba34713752d39250de708177c3dee0de55c9a51`
for every image below. Each image has 705 unique RGB colors in this band, including 112,850 pixels
of the dark panel surface `(30, 41, 59)` and 75,150 pixels with other colors. Every pair differs in
zero RGB pixels in this region.

| Capture                                                                   | PNG SHA-256                                                        | Panel-band SHA-256                                                 | Dark panel pixels |
| ------------------------------------------------------------------------- | ------------------------------------------------------------------ | ------------------------------------------------------------------ | ----------------: |
| Eighth pointer Back, `pointer/png/11-back.png`                            | `b3775b1fcba9c0fbba7ffa3a87753c89e6979ac1607e6282fe80e4f4c7c76304` | `7e81dc163aa69b2a1930d5c0eba34713752d39250de708177c3dee0de55c9a51` |           112,850 |
| Eighth pointer Origin, `pointer/png/12-origin.png`                        | `878314d0c7e097ebe5df6303402a6d8acbcabad78db646527a1a5ce3e74534f1` | `7e81dc163aa69b2a1930d5c0eba34713752d39250de708177c3dee0de55c9a51` |           112,850 |
| Eighth candidate Back, `candidate-pointer/png/11-back.png`                | `b3775b1fcba9c0fbba7ffa3a87753c89e6979ac1607e6282fe80e4f4c7c76304` | `7e81dc163aa69b2a1930d5c0eba34713752d39250de708177c3dee0de55c9a51` |           112,850 |
| Eighth candidate Origin, `candidate-pointer/png/12-origin.png`            | `878314d0c7e097ebe5df6303402a6d8acbcabad78db646527a1a5ce3e74534f1` | `7e81dc163aa69b2a1930d5c0eba34713752d39250de708177c3dee0de55c9a51` |           112,850 |
| Eighth enlarged text-GL Back, `text-gl-link/evidence/png/11-back.png`     | `ad581e87c61cf93ecff02a2a29824698a904b1cb35e3f11d4866da43947f81b7` | `7e81dc163aa69b2a1930d5c0eba34713752d39250de708177c3dee0de55c9a51` |           112,850 |
| Eighth enlarged text-GL Origin, `text-gl-link/evidence/png/12-origin.png` | `b731d2ec65d689d49059f7378b7c0baa5ee696b3b57cab30c26d740a84709a72` | `7e81dc163aa69b2a1930d5c0eba34713752d39250de708177c3dee0de55c9a51` |           112,850 |
| Ninth diagnostic, `diagnostic-panel/png/11-origin.png`                    | `b4f043db2ed0cf1dc7abee38499b35da09edf9c220e0888593a3f15b46b866de` | `7e81dc163aa69b2a1930d5c0eba34713752d39250de708177c3dee0de55c9a51` |           112,850 |
| Ninth diagnostic settled, `diagnostic-panel/png/12-origin-settled.png`    | `90da4fef147d60e259e31c6f73d1394eb3a393dd153705bd1bb227165da0b2f1` | `7e81dc163aa69b2a1930d5c0eba34713752d39250de708177c3dee0de55c9a51` |           112,850 |

The default-size pointer Back/Origin crops and the enlarged text-GL Back/Origin crops are
byte-identical within the panel band even when whole PNG hashes differ. Those full-image differences
lie outside this panel band and do not show panel disappearance. The ninth diagnostic independently
captured two raw PPM frames five seconds apart; its `raw-final-ppm-sha256.txt` records the same
SHA-256, `5af5444d38385cca737b2eca0a0667c8d6bdfa5f243d4a45e250a604dca6d2ff`, for both. Its exported
PNGs also have identical panel-band crops.

## Corrected conclusion

The a11y transcripts and numeric pixels agree that the Link context remains visible through the
recorded Back and Origin captures. The apparent omissions in image previews were a review/display
error, not a product defect or a capture-sequence defect. The prior suggestion to change the
`LinkPanelOverlay::drawFrame` reservation gate is unsupported and should not be implemented on the
basis of this evidence. This audit covers these archived 800 × 600 frames; it does not claim broad
accessibility or layout acceptance beyond them.
