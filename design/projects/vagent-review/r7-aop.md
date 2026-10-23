# Round 7 critique: aop (focused; BLOCKING and MAJOR only)

Document: design/vagent-agent-language.md v3.4 (2118 lines). Read: §4.2 step 9 (line 331), §5.3
(lines 500-525), §8.9 grammar (line 1225), §12.3 (lines 1660-1830), `from self` uses (lines 696,
1727, 1768).

## Status of round-6 BLOCKING/MAJOR findings

- R6 #1 (emergency stop mechanisms only in prose): RESOLVED. `effect preempts` is in the keyword
  list (line 1010) and the grammar (`MethodSig`, line 1225), the contract in §5.3 says a preempting
  effect starts at once and ends the one in flight with `EffectFailed{cause: "preempted"}`, the
  drain order is now spawns, messages, effect cancels, preempting effects, other effects, timers
  (line 331), and `halt` is declared `effect preempts def halt()` (line 1676). The step 8 wording in
  §12.3 matches. Both minors were also taken (`from self` on battery reads; back-off set by the
  handler).

## New findings

None at BLOCKING or MAJOR. I re-checked the interactions this change could break: a preempted
effect's failure arrives as a late delivery for a dropped intention (counted, no replan); a
preempting effect on a live intention is an ordinary plan failure with cause `preempted`; the staged
cancel and the preempting call reach the device in the specified order. The agent model remains
sound.

## MINOR

- §15 could say that the constant 30 s maintain back-off retries a permanently broken dock forever.

VERDICT: CONVERGED
