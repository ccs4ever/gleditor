# Round 2 critique: System Architect (vagent-extended-capabilities-and-belief-vlog-unification.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (Two-phase query solver duplicates logic, misattributes SLD resolution to `zigzag/vlog`):
  RESOLVED — SLD resolution is unified with `VortexStdLib::solveQueryHelper` in `vortex_stdlib.cpp`,
  clause freshening is explicitly specified on caller activation frames, `vortex_stdlib.{hpp,cpp}`
  is added to Proposal P18.2 in the Proposal Register, and two-phase evaluation is soundly justified
  by Rule V12 topological partitioning of base rules and overlay facts.
- R1.2 (Genesis-woven `std:goal` routines hardcode fixed cursor cells violating Rule V12): RESOLVED
  — Scanning cursors are allocated on caller activation frames on `+d.scan` and passed dynamically
  as operands to `std:goal` routines in P18.4, guaranteeing multi-agent reentrancy, nested scan
  support, and Rule V12 compliance.
- R1.3 (Retracted belief clauses unlinked to limbo leak cells monotonically): RESOLVED — §2.4, §5.1,
  and §5.3 explicitly specify standard 1D rank excision (`prev.pos = next <=> next.neg = prev`) and
  immediate recycling into the overlay free-list, excising the inaccurate claim regarding
  `ArenaManifold::compact()`.
- R1.4 (Unbounded multi-source rank growth creates DoS vector): RESOLVED (DEFERRED to `U-B2`) —
  Cataloged in §1.5 Open Questions Table under `U-B2` with a concrete default (all source assertions
  coexist along `+d.clause` with plan-level arbitration).
- R1.5 (`NativeSkill::cancel` non-blocking invariant and thread safety): RESOLVED — §4.3 sequence
  diagram and text clarify that host completions deliver asynchronously to the adapter queue before
  mutating the agent arena on the runtime thread during delivery dispatch.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§4.2, line 776) Explicitly state that `NativeSkill::cancel` must be non-blocking

**Problem:** §4.2 line 776 mandates that `NativeSkill::start` must return immediately to the caller,
but does not explicitly mention `cancel`. Although `apps/common/xanadu/vagent/host_bindings.hpp:17`
establishes that `cancel` is best effort and idempotent, stating non-blocking execution explicitly
for both `start` and `cancel` in §4.2 clarifies the host contract.

**Fix:** Update §4.2 line 776 to read: "`NativeSkill::start` and `NativeSkill::cancel` must return
immediately to the caller."

### 2. (§1.5, `U-B2` & §2.3, line 330) Track retention limits for open swarm deployments

**Problem:** In open swarm networks (`xudu-swarm-peer`), peer assertion rates could cause long
`+d.source` or disagreement ranks. While `U-B2` establishes coexistence as the default design,
production deployments will benefit from configurable limits (`vagent.max_source_disagreements = 8`,
`vagent.max_sources_per_fact = 32`).

**Fix:** Add a brief implementation note in `U-B2` recommending that runtime caps and eviction
policies be evaluated when benchmarking multi-peer swarm stress tests.

### 3. (§2.6, line 485) Explicitly mark `from deduced` as a contextual keyword

**Problem:** `believes P(...) from deduced` uses `deduced` to restrict queries to Horn clause
solutions. Marking `deduced` as a contextual keyword in the parser prevents shadowing or collision
with variable identifiers named `deduced`.

**Fix:** In §2.6, add a parenthetical note indicating that `deduced` is parsed as a contextual
keyword in the `from` clause.

VERDICT: CONVERGED
