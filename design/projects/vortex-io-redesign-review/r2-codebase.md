# Round 2 critique: Codebase Expert (vortex-io-redesign.md)

Paths below are relative to the repo root. Target SHA256:
`12c3db6de5c7d1b471b568b6c4b876ec4fd1f65d99f2b0017c278cc121b623fe`. Tally: 0 BLOCKING, 1 MAJOR, 0
MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (missing Proposal Register): RESOLVED — P1–P4 name the current VM/host, arena resolver,
  capture, and frontend migration files with rationale, costs, and fallbacks
  (`design/vortex-io-redesign.md:625–640`). These paths exist in the checkout; P2 and P3 correctly
  leave conditional files subject to evidence gates.
- R1.2 (dense-slot ABA without arena lease hooks): RESOLVED at the design level — P2 identifies
  arena mark/trail/release ownership and U-2 prohibits relying on `contains(ref)` alone and requires
  a same-index reuse and exception-path proof before implementation
  (`design/vortex-io-redesign.md:635, 648`). This remains an implementation gate, not a claim that
  current `ArenaManifold` already provides lease callbacks.
- R1.3 (capture/promotion and recovery route): RESOLVED at the design level — P3 names permascroll,
  store, and arena-promotion paths, gives a no-capture fallback, and U-3 requires failure injection
  and restart reconciliation before implementation (`design/vortex-io-redesign.md:636, 649`). The
  journal representation remains open as intended.

## BLOCKING

None.

## MAJOR

### 1. (§8.3, lines 457–469; §13 U-4) Configurable system budgets lack a numbered settings proposal

**Problem:** The design requires every user-adjustable budget class to receive a settings schema,
default, and notes, and U-4's resolution gate says to add those system settings. That requires
changing the existing settings emitter, but P1–P4 do not list it. The blanket statement that newly
discovered files become a separate proposal before implementation
(`design/vortex-io-redesign.md:639–640`) prevents an unreviewed edit, but this file is already
identifiable from the explicit U-4 work and should be in a numbered, costed proposal now.

**Evidence:** “Every user-adjustable class requires a setting schema/default/notes entry when
implemented” (`design/vortex-io-redesign.md:465–469`); U-4 requires experiments “then add system
settings with schema/default/notes” (`design/vortex-io-redesign.md:650`). The current settings
registry is `defaultSettingSpecs` in `apps/common/xanadu/system_docs.cpp:538–540`, and
`ensureAllSettings` emits those specs (`apps/common/xanadu/system_docs.cpp:2356–2367`). The
repository instructions state that adding a setting requires a `defaultSettingSpecs()` entry. P1–P4
list VM, host, arena, permascroll/store, and language files, but not the system-settings emitter
(`design/vortex-io-redesign.md:632–637`).

**Fix:** Add a numbered proposal naming `apps/common/xanadu/system_docs.cpp` and any required
settings declarations in `system_docs.hpp`, with the exact budget setting scope, why host-only
configuration is insufficient for user-adjustable limits, schema/default/notes and migration cost,
and a fallback that keeps the relevant budget immutable in the host/service snapshot until that
proposal is approved. Keep the per-request immutable snapshot rule in §8.3.

## MINOR

None.

VERDICT: NOT CONVERGED
