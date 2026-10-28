# I3-1a review: vagent compiler and codebase persona

Scope: the uncommitted diff, `epistemic_policy.{hpp,cpp}` and `vagent_epistemic_policy_test.cpp`,
against i3-design section 2. Method: read the diff, then built a throwaway probe test in
/tmp/vagent-p19-i31a/cb (no repo files touched). It linked against the existing `vagent_actor_test`
objects.

## Probe results (all behave correctly)

- Checkpoint on host A and resume on host B with an arbitrated predicate: the policy master survives
  (10 fields, min_confidence 0.7).
- Live upgrade with an identical policy succeeds, with no duplicated or orphaned master.
  `d.epistemic` on the predicate cell does not trip the UpgradeGraph.
- Live upgrade with a changed `min_confidence` is refused by the `compareUpgradeSchemas` /
  `upgradeChangeRequiresRefusal` check (agent_executor.cpp:3746).
- Control: the same Hub/Worker program with a non-arbitrated predicate gives `plus == 1`. The test
  `PeerTellCannotWriteAnArbitratedPredicate` therefore asserts 0 for the right reason, not because
  the tell never ran.
- Parser: `arbitrated` is contextual and a bare `arbitrated truest()` gets a clear error. Newlines
  between the words are fine. An empty arg list reports every missing key, with
  `MissingClusterTolerance` for the continuous case. A trailing comma is accepted. Non-literal
  values are refused. `Opt<Float>` is discrete. `List` is not a type in this dialect.
- Guards: the `rtBSet_` / `rtBDrop_` call sites in `beliefChange` and `emitTellBodies` bail on an
  arbitrated predicate, and `emitTellTests` skips it. All three `callLabel(rtBSet_)` sites that
  exist are covered. The retire-turn `rtBDrop_` is by design.

## BLOCKING

None.

## MAJOR

None.

## MINOR

1. **Negative values cannot be written, so two domain checks are unreachable.** parser.cpp (new
   policy loop) calls `parsePrimary()`, which has no unary minus. `min_confidence: -0.5` or
   `clock_skew: -1s` therefore dies with the parse error `ExpectedExpression: found '-'` and never
   reaches `InvalidEpistemicPolicy`.
   - Dead code: the `clock_skew >= 0` check (epistemic_policy.cpp `domains()`) and the `>= 0` halves
     of the `min_confidence` and `score_tolerance` checks.
   - The design says out-of-domain values refuse `InvalidEpistemicPolicy`, and no test covers
     negatives.
   - A side benefit of this limit: `-0.0` and NaN cannot reach `canonicalPolicy`, so the 0.0 vs -0.0
     hash split is not live today.
   - Fix: fold a leading `-` on a numeric or duration literal in the policy loop, add a
     negative-value test, and make `canonicalPolicy` normalise -0.0 (add `+ 0.0`) if the parser
     change lands. Or state in the i3 doc that negatives are a parse error.
1. **Continuity is derived twice.** The rule "non-key Float parameter" is computed in checker.cpp
   (~1163, over `PredicateParamInfo`) and again in compiler.cpp `mintPredicates` (~3996, over
   `Ty::K::Float`). The compiler also re-runs `checkArbitrationPolicy`, so the checker's
   `PredicateInfo::policy` is never consumed by the compiler. They agree today, but a future type
   alias or `Opt<Float>` rule can drift apart, and only a bail with "compiler defect" would show it.
   Fix: pass `CheckedUnit` policy into codegen, or share one `isContinuous(params)` helper.
1. **The key table is wider than design section 2.** It admits `decay`, `alignment`,
   `score_tolerance`, `max_cluster_diameter`, `max_slew` and `innovation`.
   - They match the long-form proposal (the `max_slew` example) but not the accepted I3 text, which
     lists exactly the keys above plus `clock_skew`.
   - The invented domain `max_cluster_diameter >= cluster_tolerance` and the `max_slew` /
     `innovation > 0` rules are also in the hash, so changing them later is a format change.
   - Fix: update i3-design section 2 to list them, or drop them until their consuming slice.
1. **`<bit>` is not included.** epistemic_policy.cpp:311 uses `std::bit_cast` and gets the header
   only transitively. Add `#include <bit>`. `<optional>` and `<string>` come via the header, which
   is fine.
1. **Test gaps (they pass, but leave behaviour unpinned).**
   - No test resumes or live-upgrades an agent with an arbitrated predicate. My probe shows it
     works, but it is the highest-risk interplay.
   - There is no restore-side test that a changed policy gives a typed mismatch, as the design says.
     It is refused today only because the program ref changes with the source.
   - `EveryKeyRuleRefusesWithItsCode` has no case for `cluster_tolerance` or `innovation` on a
     discrete predicate, nor for a wrong-typed `model` or `frame`.
   - Nothing tests the new `apps/vagent/main.cpp` `check-upgrade` message path.
   - The tell test has no non-arbitrated control in the same file, so it could be mistaken for
     vacuous. Add the control (`plus == 1`).
   - The compiler guards are unreachable through the checker, so they are untested by construction.
     Consider one test that forces them, or accept that.
1. **Duplicate predicate names.** `mintPredicates` mints the policy master cells before the
   `preds_.emplace` duplicate-name bail (compiler.cpp ~3995-4014). Harmless today, because the
   checker reports `DuplicateName` first, but the master is minted before the failure can be known.
1. **Format not verified.** clang-format-19 is not on PATH here, so I could not run the format gate.
   No new line exceeds 80 columns. The checker.cpp `Overloaded{...}` block is re-indented wholesale,
   which is expected clang-format churn.

## Conformance summary

- Every required key and the optional `clock_skew` are present, with the correct literal types.
- Variance domain: 1e-150..1e150 and min \<= max are enforced, with boundary values accepted.
  `MissingClusterTolerance` is distinct from `InvalidEpistemicPolicy`.
- Consensus on a discrete predicate and continuous-only keys on a discrete predicate are refused.
- `believe`, `forget`, `revise`, keyed `believe` and `believes ... from` give `EpistemicIngressOnly`
  only for arbitrated predicates. `+pred` handlers and queries are untouched.
- The master record sits on the predicate's `d.epistemic` rank with typed scalar cells, absent
  optionals are omitted (no sentinel), and the hash is stored in the master.
- `EpistemicPolicySchema` carries the full policy and its hash. `EpistemicPolicy` is a breaking
  upgrade change in both the library and the CLI.
- Canonical encoding is length-prefixed and versioned, with doubles by bits, and is stable against
  key order (tested). I found no bug in the encoding.
- Comments explain why, and there are no sentinels.

Tally: BLOCKING 0, MAJOR 0, MINOR 7.

VERDICT: PASS
