# Fifteenth spatial UI increment: independent UX and visual review

Independent review of the frozen build-4 native OpenGL captures at 1440×1000. I inspected the actual
pixels and the paired action/accessibility logs; this is a bounded package-lifecycle visual review,
not acceptance of full J23/J24 or the complete spatial workflow. No product files were changed and
no additional native actions were run by this reviewer.

## Visual result

The package review form fits the new preference state into the existing visual system. Frames
`09-package-review.ppm`, `10-retained.ppm`, `11-enabled.ppm`, and `12-quiet.ppm` keep the same slate
surface, title hierarchy, muted status line, blue focused Action field, and aligned label/value rows
as the fourteenth-increment forms. Retain, Enable/Disable, and Normal/Quiet are distinct menu
actions; the current values remain visible together in the status line. In frame 09 the full
40-character carrier `f985e7513550a08b7463fbbc7008e8cb931d75e0` wraps onto a second line and remains
legible with signature verification and enrollment status. The earlier downloaded-list row in
`06-review-unretained.ppm` truncates the hash with an ellipsis; the package review form supplies the
complete visible value and the full row value is also present in the recorded accessible label.
Frame names 05 and 06 are preserved as actual outcomes: 05 is source-publication selection and 06 is
the downloaded-package inventory, not a package-status detail.

The form status line is visually dense, but at this capture size it wraps cleanly without clipping
or colliding with the Action field. The inventory dialogs reuse the same centered form geometry.
Their count explanation wraps to two readable lines, and the carrier row remains a single line with
ellipsized tail. The full identifier is available after opening the review form. No new spacing,
colour or corner treatment appears out of family with the fourteenth baseline.

The link-panel action grid grows from the former four-row layout to five rows when retained package
controls are present. Frames `23-corrected-left2-right3.ppm`, `24-disabled-pinned-left2-right3.ppm`,
`25-removed-pinned-left2-right3.ppm`, `27-reenabled-quiet-left2-right3.ppm`, and
`34-root-restarted-pins.ppm` show consistent cell widths, alignment and spacing, with all labels
fitting at 1440×1000. Disabled and removed states keep both endsets, the origin and the selected
Link context visible; Enter becomes “Enter unavailable.” The established compact `×` remains in its
own button. It is terse, but this review does not treat that existing control as a blocking visual
issue.

One minor copy issue remains in the disabled/removed panel: “not retained” describes the inventory
preference while “pinned Link retained” describes the live selection context. The labels are
technically compatible, but they reuse “retained” for two nearby states. “Current Link context
preserved” would distinguish the latter more clearly. This does not prevent the reader from seeing
the package preference or the disabled state.

## Quiet emphasis comparison

Frames `35-normal-unselected.ppm` and `36-quiet-unselected.ppm` come from the same process after
closing the panel, with the package retained and enabled and no selected Link. The connector in the
dark gutter is visibly dimmer in Quiet while preserving its cyan identity; it remains visible. The
blue ribbons across the white document area remain strong in both frames, so the contrast is most
apparent in the gutter. Quiet lowers visual priority without erasing the link or changing the
document text.

The paired-region measurements in `quiet-normal-region-difference.json` support that reading: 1,032
pixels differ in the connector gap (`x=600–839`, `y=480–579`), with a maximum channel delta of 97;
the top chrome has no changed pixels. The full-frame difference includes 1,612 changed minimap
pixels with a maximum delta of 235, so it cannot all be attributed to Quiet. The right text region
has 38 changed mark pixels with a maximum delta of 43. I therefore judge the connector distinction
visible but modest, and do not use the full-frame count as evidence of a stronger beam effect.

## Lifecycle evidence reviewed

I treated the corrected action logs and snapshots as the source of state claims, not capture names
or process exit alone. The initial captures 15–22 showed Left 1/2 and Right 2/3 and are preliminary.
Corrected captures 23–28 show Left 2/2 and Right 1/3; the final exact-pin phase 29–34 shows Left 2/2
and Right 3/3, matching the explicit cursor changes in its action transcript. Restart frame 34
retains the exact selected carrier and reports retained, enabled, quiet. Its persisted format-2
choice is `retained=1`, `enabled=1`, `emphasis=quiet`.

The disabled and removed sequence preserves that same selected Link context. Frame 30 is retained
but disabled. Frame 31 reports not retained and disabled while leaving the pin visible. Frame 32
shows the carrier retained again but still disabled, and frame 33 shows explicit re-enable. The
reader inventory dialogs reconcile the choice transitions: frame 18 shows 0 retained / 0 enabled
while the removed carrier remains listed; frame 20 shows 1 retained / 0 enabled, 1 known retained
link / 0 drawable; frame 22 shows 1 retained / 1 enabled, 1 known retained link / 1 drawable. The
root sequence repeats the same disabled, removed, retained-disabled and re-enabled states at the
Left 2/2, Right 3/3 cursor position. These inventory counts and persisted preference snapshots
substantiate the lifecycle independently of exit status.

The source operation file hashes remain identical across the reviewed checkpoints. The decoded
activity snapshots keep one visit and current visit 1 across retain, enable, quiet, disable,
removal, restore and restart. These observations support the stated purity of this bounded
preference journey; they do not establish full J24, J25 or all endpoint-availability variants. The
native restart log reports that accessibility is not in this build, so this review makes no native
AT-SPI claim.

## Finding summary

No blocking aesthetic defect appeared in the reviewed package forms or the expanded panel grid.
Preference status is visible, the exact carrier hash fits in the package review form, and disabled,
removed and restored contexts remain visually coherent. The full hash is shortened in inventory
rows, the disabled panel repeats “retained” for two meanings, and Quiet has a modest visible effect
that is clearer in the gutter than over the white page. Final acceptance of complete J23/J24 and
other scales/backends remains outside this capture set.
