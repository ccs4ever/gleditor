# Round 2 critique: Xanadu / ZigZag Purist (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (Occurrence ranks lack a concrete reciprocal graph): RESOLVED — §6.2 now gives the
  two-request shared endpoint/capability graph, private occurrences, per-occurrence posward clone
  views, reciprocal queue rank, lookup path, and requires the transient spike to demonstrate it
  (`design/vortex-io-redesign.md:297-309`). This agrees with `ArenaManifold::link`, which writes
  reciprocal links and clears displaced occupants
  (`apps/common/xanadu/zigzag/arena_manifold.cpp:2201-2214`), and with `cloneMaster`, which follows
  the clone rank negward (`apps/common/xanadu/zigzag/arena_manifold.cpp:1760-1764`).

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

The capture model preserves identity by source-generation interval and range, explicitly
distinguishes equal content at different occurrences, and refuses to infer identity from hashes
(`design/vortex-io-redesign.md:406-413, 493-508`). Its capture boundary publishes references only
after permanent bytes and mappings are known durable (`design/vortex-io-redesign.md:512-526`),
consistent with the store's MakeCell-backed CellRefs and refusal to persist ephemeral targets
(`apps/common/xanadu/zigzag/manifold.cpp:586-608`). Remaining ABI, allocation-incarnation, and
recovery details are explicitly gated by U-1 through U-3 rather than hidden as settled mechanics
(`design/vortex-io-redesign.md:643-649`). Cell-visible state is authoritative while OS handles,
queues, and buffers remain private service state under explicit reclamation rules
(`design/vortex-io-redesign.md:311-322`), which preserves the cell-only public interface without
pretending operational resources are cells.

VERDICT: CONVERGED
