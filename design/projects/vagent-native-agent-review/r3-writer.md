# Round 3 critique: Technical Writer (vagent-native-agent.md v0.3)

Tally: 0 BLOCKING, 2 MAJOR, 3 MINOR. Verdict below.

## Status of previous BLOCKING/MAJOR findings

- R2.1 (checkpoint vs grants): RESOLVED. 12.3, 12.4 (three signals), 10.3 and P20.6 now agree;
  `checkpointed` is gone.
- R2.2 (stale-token test cannot fail): RESOLVED. `curses_keep` variant (13.1) and exact ledger rows
  for both variants (13.2 steps 8-9).
- R2.3a (sequence grammar): PARTLY. Grammar block, `finally` semantics and `delscreen` are in, but
  the example no longer type-checks against 11.2 (M2 below).
- R2.3b (`programRef` parts): RESOLVED. N6 says four parts with the manifest inside the log; 7.1/6.2
  agree.

## MAJOR

### 1. (line 101) The document fails the format and lint gates

`mdformat --check --wrap 100 ...` fails and `mdl -s .mdl_style.rb` reports MD013 on line 101 (the Q2
bullet, 104 columns). `make format-check` and `make lint` are blocking CI gates. Fix: run
`mdformat --wrap 100` with the three extensions (the diff is only lines 101-104).

### 2. (11.2 vs 11.4) The worked sequence does not match the declaration it claims to be checked against

11.2 declares `mvinnstr(y, x, s: cstring_out(n), n: c_int)` (four parameters). 11.4 calls
`mvinnstr(y, x, len(text))` (three arguments) and rule 1 says "Arguments are positional" and "every
call is type-checked against its declaration". Nothing says an out-parameter is omitted from the
call and returned in the step record (rule 2 only says it is bound by name in the result). As
written the example is an arity error under its own rule. Fix: state in rule 1 that `cstring_out`
parameters are not passed, and that `n` is the argument naming their size; or declare
`mvinnstr(y, x, n: c_int, s: cstring_out(n))` and say out-parameters are skipped positionally.

## MINOR

- 10.3 defines `stale` as including "revoked", while 10.1 says `close_terminal` on an already
  revoked token succeeds; say `close_terminal` is exempt from `stale`.
- 11.4 rule 5: `finally endwin()` mentions no `let` name, so it runs even when `newterm` failed; say
  so or add `sp` to the guard.
- 13.2 step 9 uses outcome `granted`; the outcome vocabulary is still not listed in 10.5.

VERDICT: NOT CONVERGED (two mechanical MAJORs; both are one-line fixes)
