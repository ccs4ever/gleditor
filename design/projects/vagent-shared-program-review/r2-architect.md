# Round 2 critique: System Architect (`design/vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root.
Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (Runaway COW-Shadowing on Opcode Outputs and Broken Turn Cleanup): RESOLVED — Opcode outputs route to ephemeral registers and activation frames on `d.vars`/`d.values`; base routine cells remain immutable and broken turn-end mark/release cleanup was eliminated.
- R1.2 (Unjustified C++ State Reconciliation Violating `GEMINI.md`): RESOLVED — §5 and P17.4 explicitly partition C++ supervisor grant/turn management from Vortex stdlib (`std:agent`) reconciliation logic.
- R1.3 (Missing Retention Bounds on Base Manifolds in `LocalHost`): RESOLVED — Bounded LRU cache specified in `LocalHost::Options::maxCachedPrograms` (default 32) and priced in Proposal P17.3.
- R1.4 (Magic Sentinel Zero for Unstarted Intentions): RESOLVED — Sentinel integer `0` for unstarted plans eliminated; §4.3.4 now uses clean plan hash comparison and explicit intention lifecycle states.
- R1.5 (Error Type Mismatch and Hallucinated C++ Signatures in Proposals): RESOLVED — P17.4 signature updated to return `std::expected<void, ActorError>`, and file/method citations aligned with repository truth.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§2.1, line 155 & §5, line 465) Clarify Active Actor Pinning Invariant in `LocalHost` Program LRU Cache

**Problem:** §2.1 specifies that base manifolds are retained in an LRU cache bounded by `LocalHost::Options::maxCachedPrograms` (default 32), while Proposal P17.3 defines `ProgramManifoldResolver` returning a raw pointer `const zigzag::Manifold *`. To ensure mechanical sympathy and prevent use-after-free, the specification should explicitly note that LRU eviction applies only to unreferenced program manifolds whose active actor count is zero.

**Evidence:**
- Doc §2.1 (lines 154–156): "Base manifolds are retained in an LRU cache bounded by `LocalHost::Options::maxCachedPrograms` (default 32)."
- Doc §5 Proposal P17.3 (lines 465–467): `using ProgramManifoldResolver = std::function<const zigzag::Manifold *(std::string_view programRef)>;`
- Repo `apps/common/xanadu/vortex/worker_context.hpp:52`: `WorkerContext` stores `const Manifold *baseManifold`.

**Fix:** Add a sentence to §2.1 clarifying that `LocalHost` pins base manifolds for all currently running actors, applying LRU eviction only to cached programs when their active agent count reaches zero.

### 2. (§5, line 420) Method Name Precision in Proposal P17.1

**Problem:** Proposal P17.1 states that `ArenaManifold` seeds `arenaDims_` with `base->dimensionNames()`. In the live repository, `zigzag::Manifold` provides `dimensions()` returning `std::span<const DimRef>`, not `dimensionNames()`.

**Evidence:**
- Doc §5 Proposal P17.1 (line 420): "seed `arenaDims_` with `base->dimensionNames()`"
- Repo `apps/common/xanadu/zigzag/manifold.hpp:472`: `[[nodiscard]] std::span<const DimRef> dimensions() const;`

**Fix:** Update line 420 to state: "seed `arenaDims_` by walking `base->dimensions()` and resolving dimension names via the attached reader/store".

______________________________________________________________________

VERDICT: CONVERGED
