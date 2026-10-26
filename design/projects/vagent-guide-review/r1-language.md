# R1 language and teaching review

Verdict: CONVERGED — no actionable language-contract or teaching defects found.

Compared the frozen R1 guide with vagent v3.9's truth/failure, assignment/position, value, scope,
belief, goal, intention, suspension, effect, communication, and scheduling contracts. The guide's
beginner-facing rules and examples preserve the design's key distinctions:

- Bool values succeed regardless of `true`/`false`; comparisons and optional reads are success
  tests.
- `:=` is bounded and `<-` is reversible across search, while both remain subject to enclosing
  statement, handler, and turn rollback.
- Statements cut their alternatives; handler failure undoes the handler, while an ordinary later
  plan failure preserves earlier successful statements.
- `adopt` returns without waiting, local `achieve` suspends for a subgoal, and top-level/delegated
  goal outcomes remain distinct.
- Plan locals survive suspension as values; suspension points and effect recovery are kept within
  the specified statement positions and deadlines.
- Belief queries retain source provenance, waits distinguish existing beliefs from future events,
  and deadlines/cancellation are described at the right boundaries.

I found no guide explanation or keyword combination that warrants a change against the frozen
contract. Runtime and checker limitations are consistently labeled as implementation status rather
than presented as language defects or available behavior.
