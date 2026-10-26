# Round 9 critique (vortex role): vagent-agent-language.md v3.6

Scope: the v3.5 -> v3.6 diff (d27a9ff7) plus a re-check of P12-P14 against the code. Paths under
`apps/common/xanadu/`.

## Previous findings

- R8: no BLOCKING or MAJOR findings; both minors (ChildStopping `cause`, executor error citation)
  were applied in v3.6's predecessor. Nothing OPEN.

## Checks of the v3.6 revisions

- P12 (Dewey path only when used): `currentDeweyPath_` is touched only in `pushChoicePoint`
  (vortex_vm.cpp:183-185), `backtrack` (:204), the oracle branch of `Choice` (:949-951),
  `setOraclePath` (:245-247) and `setCurrentDeweyPath` (:251). Its only consumers are `cutCallback_`
  (:223-234) and the oracle. Skipping maintenance when `oraclePath_` is empty and `cutCallback_` is
  unset is sound. Wording ("no oracle path or cut callback set") matches the code.
- P13 (clear trail when last mark discarded): `trail()` returns early at `outstandingMarks_ == 0`
  (arena_manifold.cpp:1506), so with no mark outstanding no entry can be replayed; clearing is safe,
  and `discard` (:1595-1607) is the right place. `hydratePrefix` sets `outstandingMarks_ = 1`
  (:2119) but it is used only by or_parallel.cpp:344, not by checkpoint/resume, so the "last mark"
  condition is reachable in vagent's path. No other caller is harmed.
- P14 (`forgetSender`): `Actor::remembered` is a `std::map<std::string,...>` keyed by request id
  (mesh_actor.cpp:47, :333-352), so erasing by sender prefix is a range erase, consistent with P3's
  ordered-range design. Mesh-runtime-only, as the register states.
- Staleness sweep: no remaining "pending approval", "fallback", "until P12/P13/P14" or "unless P13"
  wording outside the change history (lines 2074 and 2080 are history entries and correctly say
  "pending" for earlier versions). Header, §1, §4.4, §11 intro, §11.2 and §15 are mutually
  consistent.

## New BLOCKING / MAJOR findings

None.

## Minor

- §11.2 P12/P13 "Without it" cells still describe a fallback the design no longer carries; harmless
  as a statement of what the change buys, but could be reworded "what the change buys" to match the
  lead-in.

VERDICT: CONVERGED
