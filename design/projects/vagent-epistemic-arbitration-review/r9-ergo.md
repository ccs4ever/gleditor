# Round 9 critique: Programming Language Design & Ergonomics Expert (P19)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R8.1 (P19 compiler and syntax work targets the wrong frontend): RESOLVED — P19.4/P19.5 now name
  `vagent/runtime/`, and v1.5 specifies that bounded `check` remains unchanged while the actor
  frontend owns the new syntax.
- R8.2 (Epistemic source, observation, and query types do not connect to runtime belief terms):
  RESOLVED — v1.5 defines typed AST bindings, checked ingress identity, predicate-policy resolution,
  and typed dynamic mismatch refusals.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## Verification

The Round 8 frontend concern is resolved at both specification and CLI levels. The current runtime
still has only the pre-P19 `EBelieves`/`EFor` forms
(`apps/common/xanadu/vagent/runtime/ast.hpp:184–195`), so v1.5 correctly describes the new typed
nodes as future P19.4 work rather than claiming they already exist
(`design/projects/vagent-epistemic-arbitration-review/r9-input-v1.5.txt:479–500`). The P19 register
now targets `vagent/runtime/` and explicitly leaves the bounded frontend unchanged
(`.../r9-input-v1.5.txt:930–937`). The actual CLI has separate `check`, `check-runtime`, and `run`
paths; its help distinguishes the bounded and runner dialects (`apps/vagent/main.cpp:3–8,610–646`).
The candidate separately requires runtime and bounded checks (`.../r9-input-v1.5.txt:1009–1011`).

The typed provenance/query contract is concrete enough for implementation: language source selection
may resolve only an authenticated ingress binding, while observation IDs, incarnations and
schema/model keys arrive through runtime ABI records; the checker resolves the policy and validates
types/purity/context (`.../r9-input-v1.5.txt:489–500`). This addresses the R8 gap between arbitrary
`from` expressions and trusted source identity. The source/observation lifecycle also distinguishes
exact retries from new same-tuple events, retires the previous current reading, preserves pinned
historical targets, and prevents corrections from reviving retired observations
(`.../r9-input-v1.5.txt:112–125`).

The scoped-query contract gives `Value`, `Empty`, and typed `Refused` separate outcomes, specifies
cleanup and publication marks, and forbids refusal from being collapsed into ordinary candidate
failure (`.../r9-input-v1.5.txt:713–738`). It is clearly marked as future P19.8 work. The existing
compiler's framed routine layout is cited as a baseline (`.../r9-input-v1.5.txt:713–720`); no claim
is made that this already implements scoped evaluation. Upgrade/resume translation likewise states
its prerequisites and refusal behavior. Existing restoration matches predicates by name and copies a
limited dimension set (`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3200–3203,3233–3247`);
the candidate explicitly requires a future full manifest/ref-map translator and atomic refusal
(`.../r9-input-v1.5.txt:192–232`).

VERDICT: CONVERGED
