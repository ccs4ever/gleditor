# Round 4 critique: Xanadu / ZigZag Purist (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (Occurrence ranks lack a concrete reciprocal graph): RESOLVED — §6.2 still specifies
  per-request private occurrences, posward clone views, and a reciprocal endpoint queue; the P5
  actor event does not add cross-arena `CellRef` links
  (`design/vortex-io-redesign.md:321-330, 235-247`). This remains consistent with
  `ArenaManifold::link` reciprocal displacement
  (`apps/common/xanadu/zigzag/arena_manifold.cpp:2201-2214`) and negward clone-master traversal
  (`apps/common/xanadu/zigzag/arena_manifold.cpp:1760-1764`).

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

P5 keeps staged handler effects reversible until commit, delivers outcomes at a later actor message
boundary, and explicitly leaves a lost completion as an unknown original attempt without restore
resubmission (`design/vortex-io-redesign.md:235-247`). The general service contract reserves
terminal result capacity and charges completion transport
(`design/vortex-io-redesign.md:359-367, 481-489`), so the actor mailbox's explicit `MailboxFull`
refusal (`apps/common/xanadu/vortex/mesh_actor.cpp:415-424`) does not introduce an unacknowledged
retry or false-success rule. The actor-boundary gate requires checking delivery, checkpoint refusal,
restore, and late generations (`design/vortex-io-redesign.md:640-641`). P6 puts user-adjustable
budget definitions in the system configuration layer and gives the host an immutable
generation-scoped snapshot; it does not relocate cell-visible request/outcome state into native
settings tables (`design/vortex-io-redesign.md:477-490, 664, 677`). Capture identity and permanent
Structure publication remain unchanged from the converged Round 2 design
(`design/vortex-io-redesign.md:517-559`).

VERDICT: CONVERGED
