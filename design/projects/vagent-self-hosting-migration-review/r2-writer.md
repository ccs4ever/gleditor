# Round 2 critique: Technical Writer (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.B1 (Complete Absence of Proposal Register): RESOLVED — Formal Proposal Register `P18`–`P25`
  added in §6 with concrete `file:line` citations, cost pricing, and fallbacks.
- R1.B2 (Incomplete Bootstrapping and Shim Specification): RESOLVED — Compilation scratch arena
  lifecycle defined in `P21`, C++ runtime shims specified in `P23`, Phase 5 quine fixed-point
  verified via `FragmentStructure`, and resource limits set in §7.
- R1.B3 (C++ Accounting Contradiction and Omissions): RESOLVED — Section 1 and Section 2 reconcile
  all 54 files (25 `.cpp` / 24,983 LoC; 29 `.hpp` / 4,087 LoC; 29,070 total); ungrounded claims of
  libtorrent wire packet marshaling, POSIX sockets, and SDL3 events in `vagent/` removed; Tier 3
  grounded in VM step, p16 relocation, and turn pump.
- R1.M1 (Markdown Hygiene Failure: 43 mdl Violations): RESOLVED — Document passes
  `mdl -s .mdl_style.rb` and `mdformat --check` cleanly with 0 violations.
- R1.M2 (Broken API Reference: Non-Existent `zigzag::assertRankInvariants()`): RESOLVED — Replaced
  in §4 line 197 with `zigzag::crdt::CrdtEngine::verifyManifoldInvariants()`.
- R1.M3 (Broken Test Reference: Non-Existent `tests/xuzz/vagent_self_hosting_test.cpp`): RESOLVED —
  Replaced in §4 line 280 and §5 line 347 with `tests/xuzz/vagent_metacircular_quine_test.cpp`.
- R1.M4 (Companion Document and Baseline Omissions): RESOLVED — Frontmatter companion row updated
  with all 5 documents, and §1 line 17 points to
  `design/vagent-metacircular-compiler-implementation.md`.
- R1.M5 (Contradictory Phasing, Wave, and Track Taxonomies): RESOLVED — Section 4 phases map
  directly to Section 5 waves and tracks (Wave 1: Tracks A1/A2, Wave 2: Tracks B1/B2, Wave 3: Tracks
  C1/C2, Wave 4: Tracks D/E).

## BLOCKING

None. All previous BLOCKING issues have been resolved.

## MAJOR

None. All previous MAJOR issues have been resolved.

## MINOR

### 1. (§5, lines 319, 327–347) Explicit Track F in Section 5 for Phase 6 C++ Deprecation

**Problem:** In Section 5 line 319, Track E is labelled `Track E: Self-Hosting Quine Suite (P24)`.
Proposal `P24` actually specifies deprecating and retiring the C++ compiler frontend
(`Makefile:apps/common/xanadu/vagent/*.cpp`), not the quine verification test suite. Furthermore,
while Section 4 defines 6 sequential phases ending with Phase 6 (C++ Engine Decoupling &
Deprecation), Section 5's execution topology only models Waves 1–4 ending at Track E, leaving Phase
6 without an explicit track.

**Evidence:**

- `design/vagent-self-hosting-migration.md:319`:
  `T_E["Track E: Self-Hosting Quine Suite (P24)<br/>(Fixed-Point Verification Harness)"]`.
- `design/vagent-self-hosting-migration.md:431–440`: `P24` is "Deprecate and Retire C++ Compiler
  Frontend in `Makefile`".
- `design/vagent-self-hosting-migration.md:284–292`: Phase 6 defines C++ decoupling and Makefile
  deprecation.

**Fix:** Remove `(P24)` from Track E's label, and add `Wave 5: C++ Deprecation & Hardening` with
`Track F: C++ Deprecation & Shim Cutover (P23, P24)` to Section 5's flowchart and breakdown to
reflect Phase 6.

### 2. (§4, line 248) Ungrounded citation of "Vortex/Vlog logic unification" in Track 4.1

**Problem:** Track 4.1 line 248 states: "Belief pattern matching: queries agent belief manifold
using Vortex/Vlog logic unification." The VAgent cognitive loop evaluates beliefs and plans through
in-manifold graph traversals and pattern filters (`meta.cpp`, `executor.cpp`), not via a full Prolog
resolution/unification engine.

**Evidence:**

- `design/vagent-self-hosting-migration.md:248`: `using Vortex/Vlog logic unification`.
- `apps/common/xanadu/vagent/meta.cpp:210–260`: Belief matching traverses `+d.step` and `+d.values`
  cells directly.

**Fix:** Replace "using Vortex/Vlog logic unification" with "using in-manifold pattern matching and
belief rank traversal".

### 3. (§4, line 197) Namespace and class casing for `zigzag::crdt::CRDTEngine`

**Problem:** Validation Gate 1 cites `zigzag::crdt::CrdtEngine::verifyManifoldInvariants()`. The C++
class declaration in the ZigZag engine is `zigzag::crdt::CRDTEngine` (with uppercase `CRDT`).

**Evidence:**

- `apps/common/xanadu/zigzag/crdt_zz.hpp:101`:
  `class CRDTEngine { ... verifyManifoldInvariants ... };`
- `apps/common/xanadu/zigzag/crdt_zz.cpp:134`: `bool CRDTEngine::verifyManifoldInvariants(...)`

**Fix:** Change `CrdtEngine` to `CRDTEngine` on line 197.

### 4. (§2, line 96; §6, line 383) Citation of non-existent file `marshal.cpp`

**Problem:** Section 2 line 96 lists `marshal.cpp` in the Execution Loop row, and Section 6 line 383
cites `marshal.cpp` under Proposal `P20`. The repository contains
`apps/common/xanadu/vagent/marshal.hpp`, but no `marshal.cpp`.

**Evidence:**

- `ls apps/common/xanadu/vagent/marshal.*` returns only `apps/common/xanadu/vagent/marshal.hpp`.
- `design/vagent-self-hosting-migration.md:96, 383`.

**Fix:** Change references to `marshal.cpp` to `marshal.hpp`.

______________________________________________________________________

VERDICT: CONVERGED
