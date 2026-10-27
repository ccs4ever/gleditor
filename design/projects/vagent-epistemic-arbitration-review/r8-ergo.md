# Round 8 critique: PL Design and Ergonomics (P19)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 2 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- No prior R7 ergonomics report was supplied in the Round 8 brief; status not assessed.

## BLOCKING

None.

## MAJOR

### 1. (§1, lines 30–37; §8, lines 724–728) P19 compiler and syntax work targets the wrong frontend

**Problem:** The newly integrated actor runtime has its own AST, parser, checker, and graph
compiler, while the P19 proposal register still assigns surface syntax and verification
implementation to `vagent/{token,lexer,ast,parser,checker,compiler}.{hpp,cpp}` and
`vagent/compiler.cpp`. That is the preserved bounded frontend, not the actor frontend. As written,
an implementer can finish the listed P19 language work while `check-runtime`/`run` remain unable to
parse or execute it. The document acknowledges two frontends, but does not resolve this ownership
mismatch in its proposal/register or acceptance plan.

**Evidence:** The proposal notes that the runtime is integrated and that the current
specification/progress docs describe “the two compiler frontends and their call boundary”
(`design/projects/vagent-epistemic-arbitration-review/r8-input-v1.4.txt:30–37`), but P19.4 still
lists `vagent/{token,lexer,ast,parser,checker,compiler}.{hpp,cpp}` and P19.5 lists
`vagent/compiler.cpp` (`.../r8-input-v1.4.txt:724–728`). In the runtime AST, `Term` only stores a
name and arguments, and `EBelieves` stores a term plus optional `from` expression
(`apps/common/xanadu/vagent/runtime/ast.hpp:52–57,192–195`); the runtime parser recognizes only
`believes Term [from Postfix]` (`apps/common/xanadu/vagent/runtime/parser.cpp:1285–1293`). Its
compiler lowers that to clause scanning/source filtering and binds a matching term
(`apps/common/xanadu/vagent/runtime/compiler.cpp:4444–4478`); there are no modifier, uncertainty,
verification, or source-directive fields in those runtime nodes. The bounded compiler has a
different AST/compiler surface, as evidenced by the historical target citation in the design
(`.../r8-input-v1.4.txt:319–322`).

**Fix:** Update P19.4/P19.5 file ownership to the actor runtime frontend
(`vagent/runtime/{token,lexer,ast,parser,checker,compiler}.{hpp,cpp}` as applicable) and name the
bounded frontend only if compatibility is intentionally required. Add runtime
parser/checker/compiler acceptance tests for each new node and diagnostic, and state which CLI
command compiles the new syntax. Preserve the bounded frontend's existing behavior unless a
separately scoped compatibility change is approved.

### 2. (§2.1, §4, §8) Bind epistemic types to runtime belief terms

Full title: Epistemic source, observation, and query types do not connect to runtime belief terms

**Problem:** The design specifies an extensive typed observation/source model, query modes, and
policy checking, but the live actor runtime's belief language is currently predicate terms plus an
optional source expression. The v1.4 proposal does not make explicit how a typed observation
identity, provenance, source incarnation, schema/model epoch, or query result mode is represented in
the actor compiler's AST and checked against predicate declarations. The generic prose saying P19.4
“adds a typed query modifier enum” is insufficient to bridge the existing `Term`/`EBelieves`
representation. This leaves the central language contract underspecified for the frontend that `run`
actually uses, and risks a stringly typed source filter being mistaken for stable authenticated
identity or an ordinary belief match being treated as an arbitrated query.

**Evidence:** P19 describes observations and query tokens with stable event IDs, source
identity/incarnation, domain/model epochs, feature/schema versions, and query/model version
(`design/projects/vagent-epistemic-arbitration-review/r8-input-v1.4.txt:81–105`). Its grammar adds
`truest`/`ranked`/`consensus`, source filtering, and uncertainty binding, and later states that
P19.4 adds typed AST nodes and policy records (`.../r8-input-v1.4.txt:324–332,364–372`). By
contrast, the runtime AST's `Term` is only `name + args`, `EBelieves` is
`term + optional Expr from`, and `BeliefDecl` is only a fact
(`apps/common/xanadu/vagent/runtime/ast.hpp:52–57,192–195,402–405`). `parsePrefix` consumes no
modifier between `believes` and the term (`apps/common/xanadu/vagent/runtime/parser.cpp:1285–1293`),
while `believesExpr` scans predicate clauses and applies `from` via `rtHasSrc_`
(`apps/common/xanadu/vagent/runtime/compiler.cpp:4444–4478`). The runtime checker validates
predicate arity/argument types, but has no epistemic policy/query-mode checking in its term check
(`apps/common/xanadu/vagent/runtime/checker.cpp:1527–1558`).

**Fix:** Specify the actor-language binding explicitly: use typed AST fields for mode, source
selector, uncertainty result, and policy reference; give assertion/observation provenance a typed
ingress/runtime binding that cannot be supplied as an arbitrary source-name expression; define which
predicate declarations opt into arbitration and what ordinary `believes` means on such predicates.
Then specify static and dynamic errors for mixing modes, unsupported result types, and attempts to
use an untrusted or stale source/observation key. Add parser, checker, and compiler examples/tests
against `check-runtime` and `run`; do not rely on the bounded frontend tests as evidence.

## MINOR

None.

VERDICT: NOT CONVERGED
