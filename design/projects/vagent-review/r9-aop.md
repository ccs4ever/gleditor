# Round 9 critique: aop (focused; BLOCKING and MAJOR only)

Document: design/vagent-agent-language.md v3.6. Reviewed the v3.6 diff (d27a9ff7) and grepped the
whole document for stale "pending", "await approval", "until P1x" and fallback wording.

## Status of earlier BLOCKING/MAJOR findings

All earlier BLOCKING/MAJOR findings were RESOLVED at round 7 and nothing in v3.6 reopens them.

## New findings

None at BLOCKING or MAJOR.

- The P12-P14 approval edits are consistent: header (line 10), section 1 (lines 61, 93-96), section
  4.4 cost bullets (411-414), section 11 intro and register (1481, 1534, 1550-1552) and section 15
  (1988) all describe approved fixes. Remaining "pending" hits are change-history entries for
  earlier versions (lines 2074-2080), which is correct.
- Agent semantics unaffected: P12 touches only a private VM path vagent never sets; P13 clears the
  trail only when the last mark is discarded, which is the commit point, after which the turn's undo
  (release) is no longer possible, so hard-failure recovery (section 4.2) is not weakened.
- P14: forgetting a stopped sender makes a late duplicate be processed once more (line 1988). This
  is stated and bounded to a sender whose actor is gone, so at-most-once is weakened only for
  already-stopped peers; acceptable.

## MINOR

- Line 2074/2080 history entries still say "pending"; fine as history, but a "(approved in 3.6)"
  pointer would spare readers a double take.

VERDICT: CONVERGED
