# Sixteenth spatial UI increment: independent UX and visual review

Independent visual review of the native OpenGL captures at 1440×1000 across the 31fb baseline,
98610d53 reviewed phase, and final compact build 19792347. I inspected the actual PPM pixels and
paired action/UI-tree transcripts for frames 30–49, plus decoded carrier preference and activity
checkpoints. I did not interact with the app or edit product files. Frames 14–16 and 20–25 are
excluded as proof of the corrected edition switch; that sequence is evidenced by frames 27–33.

## Result

The 31fb corrected sequence establishes the lifecycle: frame 30 shows Spatial review #1 retained,
enabled and quiet, with #2 retained, disabled and normal. Frame 31 shows the inverse enabled state
while preserving retention and emphasis. Frames 32 and 33 preserve the original #1 Link through
switch and restart, with Left 2/2, Right 3/3, active Right and origin. Decoded checkpoints preserve
the source operation hash and one activity visit.

The 98610d53 reviewed phase added curator/signature/enrollment/publication status fields, but at
default Action focus frames 38–43 clipped the lower fields, including Comparison status, below the
visible viewport. The UI-tree transcript contained them, but that did not make the visual finding
pass.

The final compact build addresses that issue. Frames 44–46 place Comparison status directly after
Action so it is visible in the normal form view. Frame 45 visibly says “Switch refused. Pair
refreshed; review before switching.” The same frame shows the refreshed pair. Frame 47 shows an
explicit switch back, and frame 49 restores the old pinned Link after restart. The corrected visual
guard and recovery now pass in this captured route.

## Findings

1. **Resolved in final compact build — stale-baseline refusal was previously invisible.** In the
   98610d53 phase, the reader tried a switch with an outdated baseline; the app refused it, but
   frame 39 did not show the status because the row was below the viewport. Final frames 44–46 move
   Comparison status beside Action. Frame 45 visibly explains that the switch was refused, the pair
   refreshed, and review is needed before switching. The refreshed pair is visible in the same form.
1. **Minor — the complete curator key takes two selections to inspect.** Final frame 44 shows the
   signed package salt (`curations:links`) in the “Curator package name” row and adds the curator
   key as a numbered two-part choice. Selecting each part exposes the full key through the existing
   key-parts control. This is enough to distinguish the signed curator and salt, while requiring a
   reader to open the control twice to inspect the complete key. Consider presenting the whole key
   in one read-only detail view if this becomes a frequent review task. The exact carrier hashes
   remain the preference and identity keys.
1. **Resolved in final compact build — comparison summary was truncated.** Frames 44–47 use the
   concise visible subtitle “Endpoints verified; curator unchecked; cited bytes checked when
   opened,” followed by both editions' title/sequence, preferences and link/member counts. The text
   fits without an ellipsis and keeps citation availability distinct from endpoint signature
   verification.

## Visual assessment

The final compact comparison keeps the centered slate surface, title scale, muted status text,
aligned label/value columns, and blue focused Action field. All seven fields fit in the default
1440×1000 frame. Both full carrier hashes are visible; the two edition summaries and link/member
counts remain readable. The refusal is a normal row with the same weight as other fields; it is
clear without adding a competing modal. The disabled pinned Link inspector uses the established
button grid and keeps its quiet blue/cyan identity. “Link context kept” is clear, and no new colour
or corner treatment appears out of family.

Final frame 44 shows the #2/#3 candidate pair before switching; frame 45 demonstrates stale-pair
refusal and refresh; frame 46 shows the refreshed enabled pair; frame 47 records the explicit switch
back. The preference records preserve retention and each edition's emphasis. Frames 48–49 keep the
old pinned context intact through restart. The final route supports explicit same-name edition
comparison and switching without changing the pinned Link identity.

## Evidence limits

The final UI run shows a successful drawing switch, visible stale-baseline refusal/recovery,
preserved old pinned context and restart state. The baseline refusal at frame 28 and successful
re-enable at frame 29 replace the earlier misleading attempt. The native run reports that
accessibility is unavailable in this build; no native AT-SPI claim is made. These captures do not
establish full J23/J24 or other backend/scale acceptance.
