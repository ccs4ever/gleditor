# Vortex I/O frontend review

This folder contains a fresh stakeholder critique of `design/vortex-io-redesign.md`. The frozen
round-one target is v0.9, SHA-256
`a6325b19e99bdec6f7f6cb6724ed7a6b5ddecf1e7152e345194ae6872eb58535`. Its examples are proposed
pseudocode, not implemented or frozen syntax/API. The historical six-critic convergence in
`design/projects/vortex-io-redesign-review/` covers earlier snapshots; it does not review v0.9 or
satisfy this review. The round-one editor revision was v0.10, SHA-256
`fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`; the eight R1 reports remain
anchored to the v0.9 snapshot above. Round two reviewed v0.10 and produced one consolidated MAJOR
finding about Pending/terminal control flow in the VPL example. The corrected editor revision is
v0.11; its final hash and round-three status are recorded below.

## Latest convergence matrix

| Role             | Latest review                  | Reviewed target | Tally    | Current status                                                   |
| ---------------- | ------------------------------ | --------------- | -------- | ---------------------------------------------------------------- |
| VQL              | [R2](r2-vql.md)                | v0.10           | 0B/0M/0m | CONVERGED; retained because its example was unaffected by v0.11. |
| VPL              | [R3](r3-vpl.md)                | v0.11           | 0B/0M/0m | CONVERGED.                                                       |
| vprolog / Vlog   | [R2](r2-vprolog.md)            | v0.10           | 0B/0M/0m | CONVERGED; retained because its domain was unaffected by v0.11.  |
| VIcon            | [R2](r2-vicon.md)              | v0.10           | 0B/0M/0m | CONVERGED; retained because its domain was unaffected by v0.11.  |
| VLace            | [R2](r2-vlace.md)              | v0.10           | 0B/0M/0m | CONVERGED; retained because its domain was unaffected by v0.11.  |
| VAgent           | [R2](r2-vagent.md)             | v0.10           | 0B/0M/0m | CONVERGED; retained because its domain was unaffected by v0.11.  |
| Wildcard         | [R2](r2-wildcard.md)           | v0.10           | 0B/0M/0m | CONVERGED; retained because its domain was unaffected by v0.11.  |
| Technical writer | [R3](medium-confirm-writer.md) | v0.11           | 0B/0M/0m | CONVERGED.                                                       |

R2 had zero BLOCKING, two MAJOR, and zero MINOR raw findings, consolidated to one duplicated VPL
example finding. The R3 reviews independently confirmed that the v0.11 correction resolves it. See
[r2-disposition.md](r2-disposition.md) and [r3-disposition.md](r3-disposition.md) for verified
findings, peer dialogue, and dispositions. The eight latest role verdicts are all converged, but six
are retained R2 reviews of v0.10; only VPL and the technical writer reviewed v0.11.

## Roster

Six implemented language stakeholders: VQL, VPL, vprolog/Vlog, VIcon, VLace, and VAgent. Each
standalone runner/compiler pair shares one language report. Direct Vortex is the generic protocol
baseline. `VWeave` is proposal-only with no current source frontend. The additional critics are
wildcard (unmodeled transports and future cases) and technical writer (examples and contract
clarity). Exact source inventory, constraints, and report protocol are in
[brief-common.md](brief-common.md).

## Review process

Rounds one and two used GPT-6 Luna at high effort. The focused round-three VPL and technical-writer
confirmations used GPT-6 Luna at medium effort, as requested for the resumed review. At most three
critics ran concurrently. Each reviewer read and responded to substantive peer points; the editor
verified evidence and recorded dispositions. The final latest-role gate is zero unresolved
BLOCKING/MAJOR findings across all eight roles, with each row's reviewed snapshot stated above.
Production ABI compatibility is not required, while existing on-disk/wire version rules and
`CompactOpNode` invariants remain.

## Files

- `brief-common.md` — assignment, exhaustive roster, owner constraints, and protocol.
- `r1-*.md` — initial stakeholder reports.
- `rN-disposition.md` — editor's verified findings and revisions for round N.
- Later `rN-*.md` — follow-up dialogue and convergence reports.

All six language needs briefs and the initial writer/wildcard synthesis informed the examples. The
target stayed fixed at v0.11 for both round-three reviews. Its SHA-256 is
`7a4a0d94ea6e0c36f45bad608f0df1c943d8e63c6304d3fb984620ed660e0e07`. See
[r3-disposition.md](r3-disposition.md) for the final convergence record and the distinct effort and
snapshot history across rounds.

Markdown formatting (`mdformat` with gfm, dollarmath, and frontmatter) and `mdl -i` passed for the
v0.11 target and complete review set after final metadata updates. The proposal remains a design
artifact; no source or runtime implementation was changed by this review.
