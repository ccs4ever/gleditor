# Round 3 critique: System Architect (epistemic arbitration)

Source shorthand `xanadu/` means `apps/common/xanadu/`.

Paths below are relative to the repository root (`/data/git/gleditor`). The v1.2 file is dirty in
the supplied worktree and was reviewed in place; no repository files were edited. Tally: 1 BLOCKING,
2 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R2 architect: CONVERGED (0 BLOCKING, 0 MAJOR). Its R1 rollback, profile retention, provenance, and
  layer-separation issues were reviewed again; the settle-queue rollback defect remains open
  (below).
- R2 writer: no report (quota), per `r2-disposition.md`.

## BLOCKING

### 1. (§6.3, lines 888–898) “Outside the intention frame” does not survive arena rollback

**Problem:** P19.5's central safety guarantee conflicts with ArenaManifold's global mark semantics.
A queue rank off the agent root still shares the same arena; any cell mint, link, or existing-cell
write made after `m_0` is undone by `release(m_0)`. The feature cannot provide rollback immunity as
described.

**Evidence:** §6.3 explicitly states turn failure invokes `arena.release(m_0)` (lines 888–889), but
then relies on graph placement on the same arena for safety (lines 893–898).
`ArenaManifold::release()` reverses the trail and truncates all arena slots and runs to the mark
(`xanadu/zigzag/arena_manifold.cpp:1653–1675`).

**Fix:** Design a genuinely non-speculative staging/commit path and spell out when it executes
relative to failure rollback. Include required core changes and costs in P19.5; graph topology alone
cannot cross a choice-point boundary.

## MAJOR

### 1. (§2.4, lines 375–378) `max_sources` does not bound retained arena memory

**Problem:** Pruning a profile from `+d.trust` with rank excision removes a link, not the profile
cell or its attribute subgraph. A long-lived actor that rotates through source names can keep
allocating profiles indefinitely while the visible rank stays below 128, so the claimed retention
cap is not a memory cap. `ArenaManifold::compact()` only rebuilds link/content arrays across
existing slots; it does not reclaim slots.

**Evidence:** §2.4 says the cap prunes profiles by rank excision (lines 375–378). The API's
`unlink()` only delegates to `link(..., nullopt)` (`xanadu/zigzag/arena_manifold.hpp:615–617`).
`compact()` iterates existing `slots_` and tightens content/link vectors without shrinking slots
(`xanadu/zigzag/arena_manifold.cpp:1702–1730`).

**Fix:** Define a real profile lifecycle strategy that reclaims or reuses cells (with a
remapping/identity policy), or bound total historical allocations and refuse/newly rotate only
through a mechanism that actually reclaims them. Account for retained clauses and provenance
references too.

### 2. Arbitration has no candidate or fuel bound

**Full title and location:** 2. (§3.5, lines 552–567; §§5.1–5.2, lines 735–757, 765–769) Arbitration
has no candidate or fuel bound

**Problem:** Single-link clustering requires pairwise adjacency work over candidates, and
`truest`/`ranked` then score and sort the resulting groups. The only explicit cap is on
source-profile count; there is no per-predicate clause/candidate limit, pair-comparison budget, or
defined exhaustion behavior. P19.4's estimate of ~35 instructions per query site (§8.4 line 1101)
ignores the data-dependent scan. A large or adversarial belief rank can consume the entire
configured actor-turn budget.

**Evidence:** The clustering definition is an epsilon-adjacency graph and connected components
(lines 552–567); the query pipeline scans candidate clauses and scores them (lines 735–757), while
`ranked` sorts them (lines 765–769). The live VM runs with a finite `maxCycles`
(`xanadu/vortex/vortex_vm.cpp:1246–1277`), but the design does not specify how its exhaustion
affects query/settle state.

**Fix:** Price complexity in candidate count and feature dimension, set a caller/grant-level
candidate and comparison budget (without silently dropping candidates), and define a typed
fuel-exhaustion result with rollback/queue semantics. Update the fixed per-query estimate to include
the dynamic bound.

## MINOR

### 1. Live-upgrade migration lacks stable source identity and clone remapping

**Full title and location:** 1. (§8.1, lines 1044–1050) Live-upgrade migration lacks stable source
identity and clone remapping

**Problem:** “Preserve source profiles” does not say how a new overlay identifies the same source
after its old `CellRef`s become invalid, nor how every migrated `+d.source` clone instance is
relinked to the newly created master. The prior architect review asked for source-master migration
before belief clauses and a mapping; v1.2 still does not specify it. This is a data-preservation
detail in a proposal, not a demand to implement the absent historical P17 runtime now.

**Evidence:** Profiles and source clone instances are defined in §2.2 lines 311–322; P19.1 merely
says to preserve profiles across P17 upgrades at §8.1 lines 1047–1050. The R2 architect report's
minor 1 described the missing source-master map and migration order.

**Fix:** Define a stable logical source key and migration phase order: migrate masters by key, build
`oldMaster -> newMaster`, then relink source instances and provenance tokens through that mapping.

### 2. Storage pricing excludes five listed fields and weight elements

**Full title and location:** 2. (§2.3, lines 328–342; §8.1, line 1051) Storage pricing excludes five
listed fields and weight elements

**Problem:** The profile contains thirteen fixed field cells and a variable weight vector, but P19.1
prices eight fields. This understates retained memory and the true cost per source.

**Evidence:** §2.3 lines 328–342, §8.1 line 1051; every cell is 64 bytes
(`xanadu/compact_op.hpp:78`).

**Fix:** Recalculate fixed and variable storage, then tie `max_sources` and context dimension to a
memory budget.

VERDICT: NOT CONVERGED
