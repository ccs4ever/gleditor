# I3-1 review: compiler and codebase persona

Base: apps/common/xanadu/vagent/runtime at design/vagent-epistemic-arbitration.

## Findings

1. BLOCKING: the only ingestion that changes a reading is keyed replacement, which I3-1 refuses.
   Design s4 step 4 reuses `rtBSet`; s4 last paragraph refuses keyed replacement. A continuous
   sensor's second reading has the same key and a different value. With `replace=0` (compiler.cpp
   rtBSet 4131) the source ends up holding two clauses for one key, so "one current observation per
   source/key" is false. With `replace=1`, `rtBDrop` (4089) runs `unlinkFromRank(s, dims_.source)`
   (4106) and drops the old support cell, orphaning its `d.epistemic` history. The retire/transfer
   logic of s4 step 5 only works for a same-tuple repeat. Fix: pull the narrow case of own-source
   replacement inside the ingestion routine into I3-1. Before `rtBDrop`, splice the old support's
   `d.epistemic` head onto the new support (retire the head, keep the rest, then apply the bound).
   Leave I3-2 with only user-code `retract`/`revise` and cross-key loss. Otherwise shrink I3-1 to
   discrete same-tuple predicates, which defeats its purpose.

1. MAJOR: `rtBSet` cannot return the support cell. The support is `alloc(sc)` at 4199 and 4209, and
   `r0_` carries only a 0/1 changed flag. The routine must walk the clause's `d.source` rank with
   the `src?` text Eq (as `emitRtHasSrc` 4066 does) or `rtBSet` must leave `sc` in a global
   register. Add a small `rtFindSrc(clause, name) -> cell` and an `rtFindClause(pred, term)`; both
   are about 20 ops. Idempotency ("support for this key already holds this event ID") also needs a
   key-level lookup (the first nk args), not the tuple-level `Unify "dup"` in `rtBSet`. That loop
   (4145-4170) exists only under `replace`; factor it.

1. MAJOR: alias-based identity is forgeable by program code. `rtBSet` identifies a source by text
   (`Eq "src?"`). User `believe p(..) from "sensor:gps"` (parser.cpp 1511) on an arbitrated
   predicate creates a support with that alias but no profile or record, and `bindSource` only
   authenticates host ingress. s2.1 says legacy and unbound facts are unscored. I3-1 has to decide
   now: either the checker refuses `believe/forget/revise/retract from` on an arbitrated predicate
   (`UnsupportedEpistemicChange`, checked statically), or the routine marks recordless supports.
   Also `bindSource` must refuse two principals on one alias per predicate (`UnknownSource` is
   specified for ambiguity but no binding-time check exists).

1. MAJOR: restoreState cannot carry this by editing a few lines. `restoreState` (agent_executor.cpp
   3385\) copies beliefs with `cloneGraph(arena, clause, termFrom, termInto)` where `termFrom` is
   `{grab, step, d.source}` (3430). It follows no `d.epistemic`, `d.vars`/`d.values` for records or
   `d.trust`, and `d.clone` is nowhere in the file's restore path. `cloneGraph` (219) is
   positive-only and builds a private `copy` map per call, so profile views reached from two
   supports would be duplicated, and the profile master (reached only by negative or `d.clone`
   edges) would not exist. Fix: add a single-session graph copier with one shared ref map (extend or
   reuse `UpgradeGraph` 263, which already has a `clone_` pair, an ownership check ("unowned clone
   edge", 359) and an `extensionTranslator_` hook for `d.epistemic`/`d.trust` at 319). Order: copy
   `$agent +d.trust` profiles (masters) first, then per-clause supports with their `d.epistemic`
   records, rebuilding each view onto the new master's clone rank from the map. It must throw when a
   record points to a missing profile. This is roughly 150-250 lines plus tests, the bulk of the
   work.

1. MAJOR: resume under a changed policy is not detected. `restoreState` matches predicates by name
   only (3464-3470). A checkpoint taken under `frame:"m"` resumes silently into a program that
   declares `frame:"ft"` with old records. `EpistemicPolicySchema` (upgrade_schema.hpp 40) holds
   only strategy and model, and nothing in upgrade_schema.cpp populates it (grep: declared, never
   read). I3-1 must (a) extend that struct to the whole `ArbitrationPolicy` (all closed-table keys,
   durations in ms as int64, no strings except model/frame/units), (b) populate it from the checked
   policy, (c) have restore compare the policy master record and refuse populated predicates with a
   changed policy, and (d) make the schema hash include it. The s7 sentence "live upgrade keeps
   refusing populated roots" covers upgrade but not restore.

1. MAJOR: checkpoint size contradicts the defaults. `AgentSettings::valueOperations` is 65536
   (agent_executor.hpp 61) and bounds what `resume` rebuilds. A record is about 24 fields, each a
   name cell, a value cell and edges, plus a profile view, so roughly 55-65 cells. At 16 records per
   support that is about 1000 cells per support, so about 65 supports exhaust the checkpoint,
   against `max_sources` 128 and many keys. Either state the budget and pick defaults that fit
   (history 4?), or raise `valueOperations` with an argument. Add a test that a full-budget actor
   resumes. This is the "no ceilings users work around" memory rule.

1. MAJOR: entry mechanism is unspecified, and the settle step is easy to forget. `TurnSpec::entry`
   (agent_executor.cpp 559) is an `optional<CellRef>` so a second entry is possible: add
   `CompiledAgent::observeEntry` beside `dispatchEntry` (compiler.cpp 5953). Do not use a reserved
   event type through `deliverHost`/`dispatch` (1471, emitDispatch 5222). That would put it in user
   handler tables and `mergedFor`, make it spoofable by `send`, and bypass the
   metadata-only-from-host rule. The routine must also end by calling `rtSettle_`, because `rtBSet`
   raises `+pred` into `d.pending` and `emitSettle` (4228) drains it. Pass inputs via
   `spec.turnFields` (exists, 577) as a record carrying predicate name, tuple, metadata and the
   three limits, since `maxSources`/`epistemic_history` are runtime settings Vortex cannot otherwise
   see. The term must be built from `WireRecord` with `writeValue` as a "fresh ground term
   (adopted)", matching `rtBSet`'s a1 contract.

1. MAJOR: `observe` threading and result reporting. `percept()` (2696) only pushes to the
   mutex-guarded `PostBox`; state is touched on the owner thread in `drainPosted` (2711).
   `bindSource`/`observe` run on host threads, so the binding table needs its own mutex or must be
   posted too. `PostBox::Item` has no observation variant, so add one (`kind` enum instead of the
   existing `bool percept`; do not add another bool). A queued observation that the turn refuses
   (`StaleSequence`, `EpistemicBudget`, fuel) has no outward channel: the receipt is already
   returned, `TurnOutcome` is dropped, and test s8 "stale sequence undoes the turn" has nothing to
   assert. Specify a stat counter (cf. `kStatMalformed`) or an optional observer callback plus
   `EpistemicRefusal` code per turn. Receipts must say "queued" in the type.

1. MINOR: `by` is a reserved keyword, not a contextual word. `TokenKind::By` (token.hpp 30) and
   `atCont(TokenKind::By)` (parser.cpp 1057) already exist; `atWord("by")` would never match. Only
   `arbitrated` and the strategy names are identifiers (`atWord`, parser.cpp 170). No ambiguity with
   the next top-level declaration, because `arbitrated` cannot start one. Keep policy keys as
   `expectName`, and reject a `PolicyArg` value that is a bare identifier. Correct the s2 prose.

1. MINOR: checker placement. `PredicateInfo` (checker.hpp 160) and `PredicateDecl` (ast.hpp 328)
   gain `std::optional<ArbitrationPolicy> policy`, built in the predicate pass at checker.cpp 1140.
   Put the closed key table as one `constexpr std::array` of `{name, kind, required, lo, hi}` in a
   new `epistemic_policy.hpp` (also needed by the upgrade schema and compiler), not scattered
   switches. `CheckErrorCode` needs `InvalidEpistemicPolicy`, `MissingClusterTolerance` (and their
   names/tests); `AlternationOfLiterals` shows the style. The bounds (confidence in [0,1], positive
   durations) are the "no naked numbers" case: name them.

1. MINOR: clone helpers. Vortex has `OpcodeKind::Clone` (vortex_vm.cpp 761) but it appends to the
   clone tail by walking the rank (O(L), no splice-after-master), and only the compiler's
   skill-instance code builds clone chains (5554). `unlinkFromRank(x, dim)` (4023) is
   dimension-generic and works for excising an occurrence from `d.clone`. Add
   `emitRtCloneView(master) -> view` (either the tail append that `Clone` gives, which is simpler
   and equals "posward of the master", or a true splice; say which, because s2.1 wording differs)
   and `emitRtExciseRecord(rec)` (unlink rec from `d.epistemic`, each field's view from `d.clone`,
   then `d.vars`). Excision must also free the field cells for the cell budget; arena undo is by
   truncation only, so say what reclaims them.

1. MINOR: settings plumbing. Adding `epistemicHistory` changes `checkedEpistemicLimits`'s positional
   overloads (epistemic_limits.hpp 60-80; tests in vagent_epistemic_abi_test.cpp 45-82 pin them).
   Prefer a designated-initialiser struct and update the tests. Add `kVagentEpistemicHistory` plus a
   `defaultSettingSpecs()` entry and schema note in system_docs.cpp (near 120) and the expectation
   in tests/xuzz/system_docs_test.cpp 172, or the setting is undiscoverable. The 16 default and the
   128 limit currently live as bare constants; name them in `epistemic_limits.hpp` like the others.

1. MINOR: API style. `ObserveReceipt` and `EpistemicRefusal` must be defined (an enum plus
   `std::string_view` name, no reserved value). `CheckedObservation::clockBasis` as `std::string`
   and `acquiredAt` as a raw `double` seconds invite unit bugs; use a small struct with
   `std::chrono` types or name the unit in the field. `variance` is already an optional. Public
   structs belong in a new `epistemic_ingress.hpp`, not the 643-line agent_executor.hpp.

1. MINOR: tests. Makefile lists vagent test objects explicitly (line 684 region), so new files need
   a Makefile entry. Place: parser cases in tests/xuzz/vagent_runtime_parser_test.cpp, checker in
   vagent_runtime_checker_test.cpp, ingress/atomicity in a new
   tests/xuzz/vagent_epistemic_ingress_test.cpp, resume in vagent_resume_test.cpp, upgrade schema in
   vagent_upgrade_schema_test.cpp. The test "fuel exhaustion leaves nothing" needs a grant with
   small `maxInstructions` and a spy on arena cell count; state it.

## Implementation plan (file level)

- `epistemic_policy.hpp` (new): `ArbitrationPolicy`, key table, bounds constants.
- `ast.hpp`, `parser.cpp`: `PredicateDecl::policy` raw args; `parsePredicate` after `)`:
  `atWord("arbitrated")`, `expect(By)`, strategy word, `(`, args.
- `checker.hpp/.cpp`: validate and attach policy; static refusal of user mutation (finding 3).
- `compiler.cpp/.hpp`: policy master per predicate on `d.epistemic`; `rtFindSrc`, `rtFindClause`,
  `rtCloneView`, `rtExciseRecord`, `rtObserve` and `observeEntry` (ends with `rtSettle_`);
  own-source replacement with history transfer (finding 1).
- `epistemic_ingress.hpp` and `agent_executor.cpp`: grants table under a mutex, `observe`, a PostBox
  kind, `drainPosted` branch, `TurnSpec{entry=observeEntry, turnFields}`, outcome channel.
- `agent_executor.cpp restoreState`: single-session copier, trust and epistemic graphs, policy
  comparison.
- `upgrade_schema.*`: full policy in the schema and hash.
- `epistemic_limits.hpp`, `system_docs.*`: `epistemic_history`.
- Tests as in finding 14.

## Size and split

Estimated 1500-2200 lines of C++ plus 600-900 of tests; the compiler routine alone is about 500-700
ops-emitting lines. Too large for one slice. Split I3-1 into:

- I3-1a: syntax, checker, policy master, upgrade-schema policy, settings (no runtime behaviour).
- I3-1b: grants, `observe`, `observeEntry`, ingestion routine with own-source replacement and
  history transfer, budgets, atomicity.
- I3-1c: restoreState and resume of profiles, supports and histories, plus the checkpoint-size test.

I3-1a can land and be reviewed independently; I3-1c is the riskiest and should have its own gate.

## Tally

BLOCKING 1, MAJOR 7, MINOR 6.

VERDICT: FAIL
