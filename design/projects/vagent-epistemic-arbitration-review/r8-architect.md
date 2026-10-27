# Round 8 critique: System Architect (vagent epistemic arbitration)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R7 findings are preserved in `r7-architect.md`; this review focuses on the integrated runtime
  baseline and does not reopen findings already dispositioned there.

## BLOCKING

None.

## MAJOR

### 1. (§7, lines 724–728) P19 compiler targets omit the integrated runtime frontend

**Problem:** P19.1 and P19.4 name `vagent/compiler.cpp` and the sibling
`vagent/{token,lexer,ast, parser,checker,compiler}` files as implementation targets. Those paths are
the preserved bounded frontend, while actor execution is compiled through the distinct
`vagent/runtime/` compiler. The document itself says `check` uses the former and
`check-runtime`/`run` the latter (line 35–36 references the split, and the Round 8 brief states the
dispatch). Implementing epistemic syntax and query lowering against the listed frontend would leave
the actor runtime unable to execute it, or would silently introduce a second, divergent language
surface. This affects the approved P19 scope; approval does not resolve the file ownership
ambiguity.

**Evidence:** The proposal register lists `vagent/compiler.cpp` for P19.1 and the unqualified
`vagent/{token,lexer,ast,parser,checker,compiler}.{hpp,cpp}` for P19.4
(`r8-input-v1.4.txt:724, 727`). The source tree has separate compiler implementations at
`apps/common/xanadu/vagent/compiler.cpp` and `apps/common/xanadu/vagent/runtime/compiler.cpp`. The
runtime program is lowered by its runtime compiler
(`apps/common/xanadu/vagent/runtime/compiler.cpp`; the separate frontend is explicitly noted in
`apps/common/xanadu/vagent/runtime/agent_executor.cpp:1` and its runtime includes). The design
acknowledges two compiler frontends in `r8-input-v1.4.txt:35–36`, but the register does not select
the runtime files.

**Fix:** Amend P19.1 and P19.4 to list `vagent/runtime/{token,lexer,ast,parser,checker,compiler}.*`
for actor-language syntax, checking and lowering. State explicitly whether the bounded frontend is
unchanged or receives a separately scoped feature. Keep runtime ABI/executor integration and
`system_docs.cpp` changes in P19.1. Add an acceptance check that `check-runtime` and `run` compile
and execute the new constructs through the runtime compiler, rather than accepting a pass from the
legacy `check` path.

## MINOR

None.

VERDICT: NOT CONVERGED
