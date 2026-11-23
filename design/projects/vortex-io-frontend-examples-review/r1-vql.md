# R1 VQL review

Target: `design/vortex-io-frontend-examples.md`, SHA-256
`2b90cb6a675e12d1ace92a4b222744bc44fa3ce9e4541f8146693c0563034d93`.

## Verdict: revise

The VQL sketch now covers the intended single script action site and keeps it outside FLWOR row
traversal. It explains that the surface syntax is proposed (current VQL uses `weave`, and its
current effect items do not include sequential match arms), and the runner re-awaits the same
request after Pending without repeating Submit. The refusal arm remains an inspectable `refused`
value. It also covers the `ReadBytes` terminal tags `Complete`, `Partial`, `EndOfStream`, `Failed`,
`Cancelled`, `TimedOut`, `Unknown`, `Overflow`, `MessageTooLarge`, and `Stale`; host-only explicit
capture and pure result rendering are correctly scoped.

One status detail remains under-specified. In the sketch, `failed($cause)`, bare `cancelled`,
`timed_out`, and `unknown($detail)` do not show retention or inspection of the accompanying progress
and optional payload. The plan's §3.2 outcome definitions allow payload-bearing `Failed`,
`Cancelled`, and `TimedOut` results and require their known progress to be preserved; `Unknown`
carries `lastKnown` progress. The helpers are described as pure transformations over cells, but
their shown arguments do not establish that those fields remain available. Make the patterns carry
the full outcome (or include the optional payload and progress fields) so this source example
demonstrates the planned observable result without losing data.

Evidence checked: implementation plan §3.2 (payload and progress rules), §10 (VQL placement,
refusal, Pending resumption, and status-gating contract), current VQL `EffectClause`/`ActionClause`
definitions in `apps/common/xanadu/vql/ast.hpp`, and `Parser::parseEffectClause` /
`Parser::parseActionClause` in `apps/common/xanadu/vql/parser.cpp`.
