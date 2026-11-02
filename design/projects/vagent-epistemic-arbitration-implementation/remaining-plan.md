# P19 remaining-work plan

Status: draft 1, for review by an architect, an ML expert and a distributed-systems expert.
Authority: [P19 v1.5](../../vagent-dynamic-source-weighting-and-epistemic-arbitration.md) and the
[traceability matrix](traceability.md), which this plan consumes: every open row of the matrix is
closed by exactly one slice below. Code baseline: HEAD `3ae8d384`, taken as the code of record (34
commits after `8ec71fc6` were made outside the gate process). Process: the [README](README.md)
review-gate rule, unchanged.

## 1. Summary

Of 468 requirements, 90 are Done, 354 are open (111 Partial, 231 Missing, 12 Designed), and the rest
are deferred by the spec, standing constraints or informational. What is built is the ingestion half
of P19: checked ingress, per-source observation history, profiles, the policy syntax and checker,
checkpoint and resume, the math opcodes and the scoped evaluation primitive. What is missing is
everything that reads that state: no arbitrated query, score, verdict, gate, label or fusion exists,
so no actor program reaches a scope. Two defects are live today: a second upgrade path skips the
populated-epistemic-root refusal (matrix X-1, reproduced by a probe in the I3-2 round-2 codebase
review), and control posts do not wake the reactor (X-27).

The plan has 36 implementation slices of at most about 1,500 lines each and six design slices. In
order:

1. **Foundations.** E0 reconciles spec and records (documents only). T0 closes the upgrade defect
   and the missed wakeup. E1 moves the epistemic code out of `compiler.cpp` and `agent_executor.cpp`
   into new files, changing no behaviour, so the remaining work does not grow two files that are
   already 7,617 and 5,673 lines.
1. **I3-2, source health** (4 slices, after a third design revision).
1. **T1, translator spine**, so every later owned structure is born with its upgrade path.
1. **I3-3, clock mapping, heartbeats, sensor reports and the signed-ingress adapter** (4 slices).
1. **I4, first half, the discrete query path** (7 slices): calibration records, scoring, pooling,
   dependence groups, syntax, `truest` lowering, query tokens and `ranked`.
1. **Verification loop** (3 slices of I3-4, then I5-1): grants, intake journal, processing turn,
   then Beta label updates and corrections. This is the first point where a source's score can
   differ from its prior.
1. **I4, second half, the continuous path** (4 slices): verdicts and gates, clustering and fusion,
   directives, close-out.
1. **I5, the rest** (4 slices): defeaters, features, online SGD, drift epochs.
1. **I6** (2 slices): native model completions.
1. **T2, translator completion** and the live-upgrade claim.
1. **F1, F2: documentation, guards and the final acceptance.**

Eight owner decisions are needed (section 7). Three block work: O2 (where scoring lives) and O3
(where query tokens hang) block I4; O4 (the signed-ingress adapter) blocks I3-3a.

## 2. Standing rules for every slice

These apply to each slice below and are not repeated in it.

- **Process.** A slice has a design (or a section of one), an implementation, and a validation
  record appended to the iteration's validation file. The reviewers named for the slice read the
  exact input and write reports to the project folder. A slice is accepted only with no BLOCKING and
  no MAJOR finding open. The lead builds and commits. The matrix is updated in the commit that
  accepts the slice, with evidence for each row it closes.
- **Headless and isolated.** Every run uses the Makefile's XDG redirects and offscreen drivers.
- **Where code goes.** Learned state and every algorithm are emitted Vortex IR over arena cells. C++
  is allowed only for what the repository's rule allows: the VM, clocks, transport, authorization,
  native effects, and the compiler front end that emits the IR. Each slice states its C++ and why.
- **No sentinels, no shims.** Optional values are `std::optional` or `std::expected`. A changed
  encoding bumps its version and deletes the old reader. A reader that cannot parse a state it is
  handed refuses by name and number.
- **Settings.** Every new limit is a `defaultSettingSpecs()` entry with a schema, a `settings::`
  name and an expected-list entry in `system_docs_test.cpp`. No naked constants in algorithms.
- **Translator parity (from T1).** A slice that adds an owned structure also extends the translator
  and adds a populated-state test for both resume and upgrade. Without it the slice is not accepted.
- **Budget parity.** A slice that adds cells re-measures `AFullEpistemicBudgetStillCheckpoints` with
  its structure at its bound, and records the worst-case VM cycles for default bounds and the margin
  to the actor's instruction grant. A slice whose worst case exceeds half the default turn grant
  fails review.
- **File growth.** After E1, a slice adds at most 400 lines to `compiler.cpp` or
  `agent_executor.cpp`; the rest goes into the new files named in the slice.
- **Format.** clang-format 19 on C++, mdformat and mdl on Markdown, in the same commit.

## 3. Ordering questions

### A. Verification percepts and the journal (I3-4) against I4's query tokens

Spec 6.3 requires a verifier's ground values to match a query token. Tokens are an I4 product.

| Option                                                   | Gain                          | Cost                                                                                                    |
| -------------------------------------------------------- | ----------------------------- | ------------------------------------------------------------------------------------------------------- |
| 1. I3-4 first, matching observation IDs only             | transport and journal earlier | a weaker check ships and is tightened later; no query exists to produce the observations being verified |
| 2. I3-4 after the token slice (I4-7)                     | one check, tested end to end  | I4's first half cannot show learning                                                                    |
| 3. Split: grants and journal first, the token match last | parallel to I4                | the same stub as option 1 for the part that matters, and compiler and executor files are shared by both |

**Recommendation: option 2.** The grant, journal and processing turn do not need tokens, but they
cannot be exercised end to end without an observation that a query produced, and the parts of them
that carry meaning (labels, `ExpiredVerificationTarget`) need the token. The cost in option 2 is
bounded: I4-1 to I4-7 are accepted on analytic vectors, ties, freshness, refusals and enumeration,
and I5-1 follows I3-4 immediately. The design for the verification loop (V-D) is written and
reviewed earlier so the executor changes it needs are known while I4 is built.

### B. Where Beta label updates land

With only priors, every source has the same weight. Symmetric priors (alpha = beta = 1) give weight
0.5 and a pooled score of 0.5 whatever the supporter count. A declared asymmetric prior (say mean
0.7) makes counts and decay visible, but still cannot separate one source from another.

| Option                                    | Gain                                         | Cost                                                                                                            |
| ----------------------------------------- | -------------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| 1. In I3-4 (labels applied on processing) | the loop closes with the transport           | mixes authorization and transport review with learning review; I3-4c exceeds its size                           |
| 2. In I4, with a test-only label injector | I4 shows reputation                          | a second write path to learned state that production never uses, against the one-mutation-path principle of 5.1 |
| 3. I5-1, right after I3-4c, before I4-8   | one path; learning reviewed by the ML expert | I4's first half is accepted provisionally on its ML gate                                                        |

**Recommendation: option 3.** I4-2 tests scoring against calibration cells whose alpha and beta the
test writes directly (a fixture writing arena cells, not a product path). The I4 ML gate is closed
only at I4-11, after I5-1, when the whole learning loop (observe, query, verify, label, re-query)
exists and can be reviewed. Claims that hold with priors alone: eligibility, freshness decay toward
0.5 under an asymmetric prior, tie abstention, refusal paths, token and ranked enumeration.
Reputation separation is claimed only from I5-1.

### C. Calibration records and dependence groups

The default is I4, and the plan keeps it: calibration records in I4-1, dependence groups in I4-4.
Two refinements. I4-1 freezes the **whole** record shape, with the SGD and ledger fields present but
absent-valued, so resume and the translator copy the shape once and I5 does not change it.
Dependence groups could wait until fusion (I4-9), because the fallback (one group, rho = 1) is
complete without them; the plan builds them before fusion so pooling and fusion share one
admitted-partition code path, and the reviewers may move I4-4 later without changing any other
slice.

### D. The translator against I5 and I6

| Option                               | Cost                                                                                           |
| ------------------------------------ | ---------------------------------------------------------------------------------------------- |
| 1. After I6                          | ten or more structures translated at once, with no earlier test; T0's defect stays a long time |
| 2. First, complete                   | rewritten for every iteration, since shapes are not final                                      |
| 3. Spine after I3-2, parity rule, T2 | each slice pays a small test; T2 audits                                                        |

**Recommendation: option 3.** T0 fixes the live defect now by routing both upgrade paths through one
guarded implementation. T1 builds the translator over the shapes final by then (profile, history,
retired chain, health records, policy master, state version) reusing the copier that resume already
uses (`restoreState`, one reference map), so resume and upgrade cannot diverge. From T1, translator
parity is an acceptance condition. T2 completes typed migrations (epoch change, feature-schema
change) and proves the whole claim.

### E. What "complete" means for live upgrade

Spec 8.2 says to complete the independent full-vagent upgrade prerequisite before claiming P19
upgrade support. P17 goal/plan lowering is not implemented, so a populated intention rank refuses
(README, I2U). Options: (1) implement P17 first, which is large, outside P19, and unscheduled; (2)
claim **supported-agent upgrade** (agents with no active intention and no native skill state),
refuse populated intention ranks by a typed, tested refusal, and amend the 8.2 wording; (3) hold
P19's final acceptance until P17 lands.

**Recommendation: option 2, with the owner's approval (O1).** The claim at F2 reads: "P19 state
(profiles, calibration, journals, ledgers, tokens, epochs) survives live upgrade and resume of a
supported agent; an agent with an active intention refuses with a named error." The consequence for
query tokens is in O3.

## 4. Order, dependencies and slices

### 4.1 Order at a glance

```text
E0 ─ T0 ─ E1 ─ I3-2a ─ I3-2b ─ I3-2c1 ─ I3-2c2 ─ T1 ─ I3-3a ─ I3-3b ─ I3-3c ─ I3-3d
   ─ I4-1 ─ I4-2 ─ I4-3 ─ I4-4 ─ I4-5 ─ I4-6 ─ I4-7 ─ I3-4a ─ I3-4b ─ I3-4c ─ I5-1
   ─ I4-8 ─ I4-9 ─ I4-10 ─ I4-11 ─ I5-2 ─ I5-3 ─ I5-4 ─ I5-5 ─ I6-1 ─ I6-2 ─ T2 ─ F1 ─ F2

designs, each reviewed to PASS before its slice starts:
  I3-2D (before I3-2a)   I3-3D (before I3-3a)   I4-D (before I4-1)
  V-D (before I3-4a)     I5-D (before I5-2)     I6-D (before I6-1)
```

Designs (`*-D`) can be written and reviewed while the previous implementation slice is in review,
because they touch disjoint files. Implementation slices are strictly sequential: the work shares
`compiler.cpp`, `agent_executor.cpp` and `runtime_abi.hpp`.

Hard dependencies that fix the order:

- E1 before any slice that edits ingestion or health IR (all of I3-2 onward).
- I3-2c1's state-version cell before I4-1 (calibration bumps it), I4-7 (`StaleEpistemicQuery`), I5-1
  and I6-1.
- I3-2 (`rtHealth`, grants) before I4-6 (eligibility recomputes the state at query time) and before
  I4-10 (directives lower onto `healthEntry`).
- I3-3b (clock-mapping snapshots) before I4-6 (query-time age and alignment read the snapshot).
- I3-3a (signed ingress) before I3-3d (sensor reports are authenticated input) and before any claim
  that 2.1-45 holds outside tests.
- I4-1 (calibration shape) before I4-2, I4-3, I4-4, I5-1.
- I4-5 (syntax) before I4-6 (lowering). I4-6 before I4-7 (tokens, generators).
- I4-7 (tokens) before I3-4a (the token match) and before I3-4b (journal pins).
- I3-4a to I3-4c before I5-1 (labels are applied from processed verifications).
- I5-1 before I4-11 (the I4 ML gate closes on the learning loop) and before I5-2.
- I4-8 (verdicts, reference) before I4-9 (consensus needs `ACCEPTED` and a reference).
- I5-3 (features) before I5-4 (SGD) before I5-5 (drift epochs).
- I6-1 after I5-5 (completions are keyed by model epoch).
- T2 after I6-2 (it translates the last structures); F1 after T2; F2 last.

### 4.2 Totals

| Group                                           | Slices                                                             | Count  | Rows closed | Estimated lines of code and tests | ML gate |
| ----------------------------------------------- | ------------------------------------------------------------------ | ------ | ----------- | --------------------------------- | ------- |
| Foundations (E0, T0, E1)                        | E0, T0, E1                                                         | 3      | 0           | 500                               | 0 of 3  |
| I3-2 (source health)                            | I3-2a, I3-2b, I3-2c1, I3-2c2                                       | 4      | 12          | 4,500                             | 1 of 4  |
| Translator (T1, T2)                             | T1, T2                                                             | 2      | 29          | 2,400                             | 1 of 2  |
| I3-3 (clock, heartbeat, reports, ingress)       | I3-3a, I3-3b, I3-3c, I3-3d                                         | 4      | 18          | 4,400                             | 2 of 4  |
| I4 (queries, scoring, gates, fusion)            | I4-1, I4-2, I4-3, I4-4, I4-5, I4-6, I4-7, I4-8, I4-9, I4-10, I4-11 | 11     | 163         | 14,000                            | 8 of 11 |
| Verification loop (I3-4 grants, journal, queue) | I3-4a, I3-4b, I3-4c                                                | 3      | 48          | 4,200                             | 2 of 3  |
| I5 (labels, defeaters, features, SGD, drift)    | I5-1, I5-2, I5-3, I5-4, I5-5                                       | 5      | 61          | 6,700                             | 5 of 5  |
| I6 (native models)                              | I6-1, I6-2                                                         | 2      | 15          | 2,300                             | 2 of 2  |
| Final (F1, F2)                                  | F1, F2                                                             | 2      | 11          | 1,000                             | 2 of 2  |
| **Total**                                       |                                                                    | **36** | **357**     | **40,000** (E0 and E1 excluded)   |         |

Estimates are judgement, anchored on the accepted I3-1 slices (135 to 1,700 lines of code and tests,
median about 500) and I2U (2,246). A slice that grows past about 1,500 lines in review splits. "Rows
closed" counts matrix rows whose last slice is this one, including Informational rows; Deferred,
Standing and Done rows need no slice.

### 4.3 Design slices

Each design is a document in this folder, reviewed to PASS before its implementation starts, as I3-1
was (five rounds) and I3-2 now is (two rounds failed, the second with 47 MAJOR findings). Estimates
are one author round plus the persona reports.

| Design | Covers                                                                                                                                              | Must settle first                                                                                                    | Reviewers                            |
| ------ | --------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- | ------------------------------------ |
| I3-2D  | revision 3 of `i3-2-design.md`: the 47 MAJOR findings of round 2 (seven reports, all FAIL, none BLOCKING)                                           | state-version cell (X-12), reason origin, reset epoch kept in the arena, health settings channel into the query turn | ML, ARCH, DIST, EPI, SENS, SEC, CODE |
| I3-3D  | ingress adapter, clock-mapping snapshots, heartbeats, sensor reports                                                                                | O4; the classification of time refusals as device or path faults; relay declaration on the binding                   | SENS, DIST, SEC, ARCH, ML            |
| I4-D   | calibration shape (frozen), policy encoding v2, score composition, query frame ABI, tokens, eligibility, gates, fusion, refusal vocabulary, budgets | O2, O3, O6, O7; the numeric reference and tolerances                                                                 | ML, NUM, EPI, ARCH, CODE, VM, DIST   |
| V-D    | verification loop: I3-4a to c and I5-1: grants, journal, queue, ledger, corrections                                                                 | label semantics for competing values; the processing outcome before I5-1; idempotency keys; retry and backoff        | DIST, SEC, EPI, ML, ARCH             |
| I5-D   | defeaters, features and normalizer, SGD and replay, drift epochs                                                                                    | determinism across hosts; replay horizon; abstention rule                                                            | ML, NUM, EPI, DIST                   |
| I6-D   | native model effects, keyed completions, cancellation                                                                                               | model-epoch keys; capacity and latency bounds; sandbox                                                               | ML, DIST, SEC, ARCH                  |

### 4.4 Implementation slices

#### E0. Reconcile the spec and the records

*Foundations. Size: about 400 lines of Markdown, no code.*

- **Content.** Apply the O5 bundle to the spec as v1.6: replay returns `StaleSequence`; the health
  model of I3-2; refusal charging; bindings not checkpointed; `clear`; P19.5 names
  `agent_executor.*`; P19.2 and P19.3 name the files O2 decides; the extra scope-only opcode
  meanings; slot reclamation by checkpoint; a truthful baseline and section 9 row; a complete
  section 4 example policy. Update the README iteration table to the order in this plan, and record
  the 34 foreign commits and the code baseline.
- **Closes.** No rows; it enables the slices after it.
- **Files.** `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md`; `README.md` of
  the project folder.
- **Depends on.** O5 approved.
- **C++ and Vortex.** None.
- **Tests.** `make format-check` and `make lint` on the changed Markdown.
- **Gate.** ARCH, EPI read the amendments against the code.

#### T0. Close the upgrade defect and three small gaps

*Foundations. Size: about 500 lines.*

- **Content.** Make `AgentExecutor::upgrade(pid, CompiledProgram&)` use the same preconditions as
  the shared-pointer overload: refuse a populated `d.trust` or `d.epistemic` root, a changed policy
  (`compareUpgradeSchemas`), flights, requests, outstanding marks and choice depth. One function,
  called by both, so they cannot diverge (X-1). Make a policy-hash mismatch on resume a typed
  refusal instead of `std::runtime_error` (X-9). Make `AgentSettings::fromModel` return
  `std::expected` through `checkedEpistemicLimits` instead of falling back to defaults (X-7).
  Relabel the TUI and user-guide "Bayesian certainty" figure as acquisition uncertainty (X-2). Make
  `PostBox::pushControl` and `putBack` wake the reactor as `push` does, so a bind, a retire or a
  health action is not left waiting (X-27).
- **Closes.** No rows; it enables the slices after it.
- **Files.** `agent_executor.{hpp,cpp}`; `program.cpp`; `vagent/tui/agent_tui.cpp`;
  `docs/vagent-user-guide.md`; `tests/xuzz/vagent_upgrade_test.cpp`,
  `vagent_epistemic_resume_test.cpp`, `vagent_settings_test.cpp`.
- **Depends on.** None.
- **C++ and Vortex.** All C++: the upgrade orchestration and the settings loader are host
  boundaries.
- **Tests.** A populated profile refuses on both overloads with the context pointer, the program
  bases and the grant unchanged; a policy change refuses on both; the typed resume refusal; invalid
  settings refuse at load.
- **Gate.** ARCH, DIST, CODE.

#### E1. Move the epistemic code out of the two large files

*Foundations. Size: about 2,600 lines moved, about 150 changed.*

- **Content.** Move `Gen`'s declaration and its helper types to a private header
  `vagent/runtime/compiler_gen.hpp`. Move the epistemic emitters (`mintPolicyMaster`, `emitRtBDrop`,
  `emitProfileFor`, `emitObserveRefusal`, `emitSourceChange`, `emitObserve`, `emitSettle`, about
  lines 4308 to 5735) to `compiler_epistemic.cpp`. Move the epistemic `AgentExecutor` members
  (`bindSource`, `observe`, the ingress state, the outcome log, the charge coalescing) to
  `agent_executor_epistemic.cpp`. Change no behaviour. Proof: the emitted IR of every program in the
  test corpus is byte-equal before and after (a throwaway harness, not committed), the three suites
  are unchanged, and a moved-line check by `git diff --color-moved` shows no edit inside moved
  blocks.
- **Closes.** No rows; it enables the slices after it.
- **Files.** `compiler.cpp`, new `compiler_gen.hpp`, `compiler_epistemic.cpp`; `agent_executor.cpp`,
  new `agent_executor_epistemic.cpp`; the Makefile finds new files by `find` and needs no edit.
- **Depends on.** T0, so the large files are quiet; the lead rebases immediately before.
- **C++ and Vortex.** A mechanical move. No new C++ behaviour.
- **Tests.** Existing suites; the IR-equality harness.
- **Gate.** CODE, ARCH. The review is of the move, not the logic.

#### I3-2a. Health estimator and derived state

*I3-2. Size: about 1,100 lines.*

- **Content.** The event-indexed refusal fraction on the profile: `health_refused` and
  `health_events`, updated with `Exp`; the one routine `rtHealth` that derives FAULTED, SUSPECT,
  DEGRADED or NOMINAL and the reason code; the stamp on records; the three settings (half-life,
  suspect fraction, minimum events). Incorporate the round-2 ML findings: relayed readings count
  symmetrically with their refusals, coalescing order cannot err toward trust unboundedly, and the
  evidence floor must not let one refusal in a fresh profile read SUSPECT.
- **Closes.** 2.1-42, 5.1-4, 5.1-8, 8.2-R1c (4).
- **Files.** `compiler_epistemic.cpp`; `runtime_abi.hpp`; `epistemic_limits.hpp`;
  `system_docs.{hpp,cpp}`; `agent_executor_epistemic.cpp` (charge coalescing);
  `tests/xuzz/vagent_epistemic_health_test.cpp` (new), `vagent_epistemic_noise_test.cpp`,
  `system_docs_test.cpp`.
- **Depends on.** I3-2D accepted.
- **C++ and Vortex.** The estimator and the state routine are emitted IR. C++ is the IR emission,
  the settings and the existing charge queue.
- **Tests.** The estimator against a closed form (k refusals, m accepts, a coalesced batch); bounds
  after 10^6 updates; threshold and min-events edges; stale and time-mismatch refusals and relayed
  copies; a 1,000-reading flush; the noisy-sensor simulation at 0.1, 1 and 100 Hz; resume keeps R
  and N; a tombstone and a restored profile have a state; second source not refreshed (R1c).
- **Gate.** ML (design and implementation), SENS, CODE, NUM.

#### I3-2b. Settle after commit

*I3-2. Size: about 800 lines.*

- **Content.** Source changes (and, later, health actions) commit first; their belief events run in
  a separate settle turn; a failed settle runs a discard turn so no later turn inherits the events.
  Fixes the `-pred` handler wedge. The design records the delivery semantics of a discard (round 2,
  DIST M1): which events are dead-lettered and where the count is visible.
- **Closes.** No rows; it enables the slices after it.
- **Files.** `compiler_epistemic.cpp` (`settleEntry`, `discardPendingEntry`); `agent_executor.cpp`
  (scheduling); `program.cpp` (remap of both entries);
  `tests/xuzz/vagent_epistemic_ingress_test.cpp`, `vagent_upgrade_test.cpp`.
- **Depends on.** I3-2a.
- **C++ and Vortex.** Scheduling the extra turn is executor C++ (turns are a host concern). The
  entries are IR.
- **Tests.** An always-failing `-pred` handler: advance and retire commit, the outcome reads
  `SettleFailed`, the next ingestion commits; a correct handler sees each event once; the settle
  turn runs before a queued observation; an upgrade where either entry is absent fails a test rather
  than skipping the settle silently.
- **Gate.** ARCH, DIST, CODE, VM.

#### I3-2c1. Health records, the state version and the health turn

*I3-2. Size: about 1,400 lines.*

- **Content.** `rtSourceDrop` as one routine shared by advance, retire, isolate and reset; the
  `fault`, `reasons` (with an origin) and `actions` structures; `healthEntry` (isolate, degrade,
  clear, reset); an arena-side reset epoch and `retired_by`, so a reset can expire labels later; the
  **epistemic-state version cell** (X-12) that health, policy, calibration and label changes bump.
  Reasons carry an origin so I3-3d can clear only what a device added. A reset keeps the audit list.
- **Closes.** 5.1-7, 5.1-11, 5.1-14 (3).
- **Files.** `compiler_epistemic.cpp`; `runtime_abi.hpp`; `epistemic_limits.hpp`, `system_docs.*`
  (reason and action bounds); `tests/xuzz/vagent_epistemic_health_test.cpp`,
  `vagent_epistemic_resume_test.cpp`.
- **Depends on.** I3-2b.
- **C++ and Vortex.** IR only; no host API yet (the turn is driven by a test harness until 2c2).
- **Tests.** Isolate before the first reading sticks and drops supports; latch, reasons and actions
  survive an advance, a retire and revive, and a checkpoint; degrade unique by (kind, code, origin);
  clear removes only operator-added degraded reasons and never unlatches; reset clears latch,
  reasons, R, N and streak, keeps dedup and actions, and the source is eligible only after a new
  reading; reset with a failing handler commits; a reset of 16 records measured against turn fuel.
- **Gate.** ARCH, DIST, EPI, SENS, CODE.

#### I3-2c2. Health grants and the host API

*I3-2. Size: about 1,200 lines.*

- **Content.** `EpistemicHealthGrant`, a 128-bit token the host keeps, `healthAction`, the receipt
  and the bounded outcome log, queue quotas per grant, idempotent receipts, the ingress latch mirror
  with stated failure semantics, the rule that the issuer is never a bound source (rechecked when
  the action runs, not only at grant time). State the trust boundary: who may call `grantHealth`,
  and where the random source comes from (the design had cited one that does not exist).
- **Closes.** 4-18, 4-20, 5.1-12, 8.2-4c, 8.2-4g (5).
- **Files.** `agent_executor.hpp`, `agent_executor_epistemic.cpp`;
  `tests/xuzz/vagent_epistemic_grants_test.cpp` (new); `tests/fuzz/fuzz_epistemic_ingress.cpp`.
- **Depends on.** I3-2c1.
- **C++ and Vortex.** All host C++: authorization, clocks (the wall stamp), the transport queue.
  None of it is learned state; the arena stays authoritative for the latch (X-14).
- **Tests.** Each refused grant and request; a token revoked while queued; an issuer that is a bound
  source or a relay; an action on a source the grant does not name; quota exhaustion cannot keep a
  safety action out; fuzzer steps for each action under a valid, a revoked and a wrong-kind grant; a
  refused action changes nothing.
- **Gate.** SEC, DIST, ARCH, CODE.

#### T1. Translator spine

*Translator. Size: about 1,400 lines.*

- **Content.** Install a default `OwnedStateTranslator` for the P19 roots final by now: profiles,
  histories, the retired chain, health records, the policy master, the state version. It reuses the
  copier of `restoreState` (one reference map) through a shared routine, so resume and upgrade share
  one implementation. Typed refusals for an unknown shape version; a pending settle, a queued health
  action or a pending delivery blocks the upgrade; peak copy (old plus new graph) is priced; grants
  and the mirror are host state and are not carried (the operator re-grants).
- **Closes.** 1-12, 2.1-33, U-1, U-3, U-12, U-13, U-14, U-15, U-16, U-23, 8.2-R2f (11).
- **Files.** new `epistemic_translate.cpp`; `agent_executor.cpp`; `upgrade_schema.*`;
  `tests/xuzz/vagent_epistemic_translate_test.cpp` (new).
- **Depends on.** I3-2c2, T0, E1.
- **C++ and Vortex.** Runs inside the host upgrade callback, which must be atomic with rollback; the
  copy itself is the existing owned-graph copier.
- **Tests.** A populated actor upgrades with a changed handler and keeps every field, latch, reason
  and history; resume and upgrade produce the same state digest (property test over random ingestion
  sequences); a failing `on Upgrade` rolls back the translation; a changed policy refuses; the old
  actor, context and grant survive every refusal.
- **Gate.** ARCH, DIST, CODE, EPI, SEC.

#### I3-3a. Signed-observation ingress adapter

*I3-3. Size: about 1,000 lines.*

- **Content.** The first production caller of `bindSource` and `observe` (X-3): verify an Ed25519
  signature, or an X.509-delegated device key, over canonical `CheckedObservation` bytes; map an
  alias to a principal under a binding policy; mark relays. Reference implementation in tree if O4
  is (a).
- **Closes.** 2.1-45 (1).
- **Files.** new `epistemic_adapter.{hpp,cpp}`; `tests/xuzz/vagent_epistemic_adapter_test.cpp`
  (new); `tests/fuzz/fuzz_epistemic_ingress.cpp`.
- **Depends on.** I3-3D, O4.
- **C++ and Vortex.** All C++: cryptographic verification and key binding are the authorization
  boundary.
- **Tests.** A forged signature, a replayed envelope, a delegated key beyond its certificate, an
  alias bound to two principals, a relay claiming another origin, and a string that merely looks
  like a principal all refuse; the adapter feeds the existing ingestion tests end to end.
- **Gate.** SEC, DIST, ARCH, CODE.

#### I3-3b. Clock-mapping resolution and snapshots

*I3-3. Size: about 1,300 lines.*

- **Content.** An immutable mapping snapshot, addressable by version for a record's retained life;
  unknown mapping rejects; reboot re-establishes a mapping or expires observations; received-time
  basis recorded; the split between device lateness and path lateness, and the reclassification of
  `StaleObservation` and `ObservationTimeMismatch` as health refusals only where the split holds (ML
  round-2 minor).
- **Closes.** U-17, U-19, 3.2-6, 3.2-7, 3.2-8, 8.2-5d (6).
- **Files.** `compiler_epistemic.cpp`; `agent_executor_epistemic.cpp`; `runtime_abi.hpp`;
  `tests/xuzz/vagent_epistemic_clock_test.cpp` (new).
- **Depends on.** I3-3a, T1 (the translator carries the snapshot).
- **C++ and Vortex.** The mapping measurement is a host clock product (C++); the snapshot store and
  the check are IR.
- **Tests.** A query-time lookup of a replaced mapping is refused; a reboot gives no silent grace; a
  flush of stale readings from a healthy device moves nothing; the translator keeps snapshots.
- **Gate.** SENS, DIST, ARCH, CODE, ML.

#### I3-3c. Heartbeats, silence and freeze

*I3-3. Size: about 1,300 lines.*

- **Content.** A per-binding heartbeat expectation; a local health turn that fires with no sample
  arriving; a `silent` flag separate from the refusal fraction and from reasons that `rtHealth`
  reads; a stuck-output rule that needs excitation or an independent constraint (repetition alone
  never flags); a latched source is exempt from silence timers; deadline rebase after resume uses a
  paired mapping or marks stale, never a new grace period; pruning of inactive profiles begins here.
- **Closes.** U-18, 3.2-4, 5.1-20, 5.1-21, 5.1-22, 8.2-4f, 8.2-R3d (7).
- **Files.** `compiler_epistemic.cpp`; `agent_executor.cpp` (timer wake);
  `agent_executor_epistemic.cpp`; `tests/xuzz/vagent_epistemic_heartbeat_test.cpp` (new).
- **Depends on.** I3-3b.
- **C++ and Vortex.** The timer wake is a host scheduler concern (C++); the flags and rules are IR.
- **Tests.** Silence expires on receive time; a slow source is not flagged by a fast rule; a
  frozen-but-excited sensor is flagged and a constant-by-nature one is not; a resume never restarts
  a full grace period; silence leaves R and N unchanged.
- **Gate.** SENS, DIST, ARCH, EPI, ML.

#### I3-3d. Sensor health reports

*I3-3. Size: about 800 lines.*

- **Content.** A separate path from `healthEntry`: an authenticated report adds an origin-tagged
  `degraded` or `suspect` reason, bounded per source; its recovery percept clears only reasons that
  source added; a hard fault may isolate at once under a policy-declared condition; a sensor
  identity can never clear a latch.
- **Closes.** 4-19, 5.1-10, 5.1-16, 7-4 (4).
- **Files.** `compiler_epistemic.cpp`; `agent_executor_epistemic.cpp`;
  `tests/xuzz/vagent_epistemic_health_test.cpp`.
- **Depends on.** I3-3c, I3-3a.
- **C++ and Vortex.** IR plus the adapter's authenticated input.
- **Tests.** A report cannot reach `healthEntry`; a bound sensor and a relay cannot invoke a health
  action; recovery clears only its own reasons and records who removed what; an operator `clear` may
  still remove device reasons.
- **Gate.** SEC, SENS, EPI.

#### I4-1. Calibration records and policy v2

*I4. Size: about 1,300 lines.*

- **Content.** A calibration record per (predicate signature, domain, epoch) on the predicate's
  `d.epistemic` rank, found by full-key comparison of typed values (the hash is an accelerator
  only): alpha, beta, decay, epoch, label count, and the SGD and ledger fields present but
  absent-valued so the shape is frozen. Priors are applied at creation. The retired chain is
  re-homed under it (X-6). Reset calls a calibration reset in the same turn (5.1-13). Policy
  encoding `vagent-policy-2` adds `risk`, `c`, `epsilon_w`, `epsilon_floor` and their domains;
  dynamic and static validation; the invariant that no two fields share one value cell.
- **Closes.** 2.1-7, 2.1-12, 2.1-13, 2.1-14, 2.1-27, 2.1-31, 2.1-37, 2.1-38, 3.1-2, 8.1-9, 5.1-13,
  8.2-5a, 8.2-R2e (13).
- **Files.** `epistemic_policy.{hpp,cpp}`; `compiler_epistemic.cpp`; `runtime_abi.hpp`;
  `system_docs.*`; `epistemic_invariants.hpp` (test);
  `tests/xuzz/vagent_epistemic_calibration_test.cpp` (new).
- **Depends on.** I4-D, I3-2c2, T1.
- **C++ and Vortex.** IR and the policy front end only.
- **Tests.** Lookup by full key with forced hash collisions; the record starts at the declared
  prior; reset returns it to the prior; resume and upgrade carry it; invalid values refuse by
  number; every new key has a boundary test.
- **Gate.** ML, NUM, ARCH, CODE, EPI, DIST.

#### I4-2. Scoring graphs

*I4. Size: about 1,300 lines.*

- **Content.** Beta mean and variance, the freshness decay `w(a)`, the lower bound for
  `risk: "lcb"`, the clamp, a numerically stable sigmoid and logit, compensated or rescaled sums,
  and dynamic validation returning `InvalidEpistemicPolicy` before arithmetic. All emitted as IR in
  a new file; an independent Python reference in `tools/` generates the golden vectors.
- **Closes.** 1-4, 3.1-1, 3.1-5, 3.1-11, 3.1-20, 3.2-1, 3.2-2, 3.4-1, 3.4-4, 3.4-5, 8.1-11, 8.1-12,
  8.P2-1, 8.2-2f (14).
- **Files.** new `epistemic_scoring.cpp`; `tools/vagent-epistemic-vectors.py` (new, independent
  implementation); `tests/xuzz/vagent_epistemic_scoring_test.cpp` (new).
- **Depends on.** I4-1, O2.
- **C++ and Vortex.** None beyond the IR emitter. The numerics are IR over `Exp`, `Ln`, `Sqrt`.
- **Tests.** Golden vectors within a stated relative error; extremes (alpha near 10^12, a logit at
  the clamp, an underflowing exponential); a non-finite intermediate refuses; monotonicity and
  symmetry properties; age never changes alpha or beta.
- **Gate.** ML, NUM, VM.

#### I4-3. Tempered log-opinion pooling

*I4. Size: about 1,000 lines.*

- **Content.** The pool `s(C)`, `n_eff`, `W(C)`: one vote per source per candidate, identical
  originals counted once, the default single group with rho = 1, stable source and event-ID order,
  neutral pooling at prior 0.5.
- **Closes.** 3.1-8, 3.1-9, 3.1-10, 3.1-19, 5.3-8, 8.2-2a (6).
- **Files.** `epistemic_scoring.cpp`; `tests/xuzz/vagent_epistemic_scoring_test.cpp`;
  `tools/vagent-epistemic-vectors.py`.
- **Depends on.** I4-2.
- **C++ and Vortex.** None.
- **Tests.** Permutation invariance of the result; copy and relay sources; neutral pooling; the
  single-group fallback; order-stable summation against the reference.
- **Gate.** ML, NUM, EPI.

#### I4-4. Dependence groups and their admission

*I4. Size: about 1,300 lines.*

- **Content.** The group registry under the calibration record, memberships as clone occurrences,
  resolution by identity, predicate, domain and epoch, a disjoint non-empty audited partition, rho
  validated at load and on update, fuel charged per walk, `DependentMeasurements` when dependence is
  unknown, and a host admission API under a grant (the I3-2c2 pattern) with audit provenance.
- **Closes.** 2.1-19, 3.1-12, 3.1-13, 3.1-14, 3.1-16, 3.1-17, 3.1-18, 5.3-13, 8.2-2b, 8.2-2c (10).
- **Files.** `compiler_epistemic.cpp`; `epistemic_scoring.cpp`; `agent_executor_epistemic.cpp`;
  `tests/xuzz/vagent_epistemic_groups_test.cpp` (new).
- **Depends on.** I4-3, I3-2c2.
- **C++ and Vortex.** Admission and authorization only; groups and pooling are cells and IR.
- **Tests.** An overlapping or empty partition refuses; rho outside [0,1] or non-finite refuses at
  load and at update; unassigned sources fall back to one group; a forged audit provenance is
  refused; the walk cost is charged.
- **Gate.** ML, SEC, ARCH, EPI.

#### I4-5. Query syntax and static checks

*I4. Size: about 1,400 lines.*

- **Content.** `believes [truest|ranked|consensus] Term [from Source] [with uncertainty: ?v]` and
  `for believes [Modifier] Term do Expr`; the contextual modifier (a predicate named `truest` still
  parses); `UnsupportedRiskModel`, `EpistemicInReadOnly`, `ImpureRankedBody`; a variance type
  distinct from a score; override checks. The compiler refuses a parsed query with a typed "not yet
  lowered" error until I4-6.
- **Closes.** 4-1, 4-2, 4-3, 4-23, 4-27, 4-29, 4-30, 4-33, 4-34, 7-6, 8.2-3a, 8.2-3b, 8.2-3c, 8.2-3d
  (14).
- **Files.** `token.hpp`, `lexer.cpp`, `parser.cpp`, `ast.hpp`, `checker.{hpp,cpp}`;
  `tests/xuzz/vagent_epistemic_syntax_test.cpp` (new).
- **Depends on.** I4-4 (so the checker knows the policy fields).
- **C++ and Vortex.** The compiler front end.
- **Tests.** Each modifier and each ambiguity; a body with a send, a spawn or a belief change in a
  ranked guard refuses; a variance and a score are not interchangeable; `check-runtime` reports each
  new diagnostic.
- **Gate.** CODE, EPI, VM.

#### I4-6. `truest` end to end

*I4. Size: about 1,500 lines, the largest slice; begins with a spike.*

- **Content.** Emit `ScopedEvaluate` at the query site with a compiler-supplied callback graph and
  frame; the eligibility pass (source, health recomputed through `rtHealth` at query time, age,
  verdict, `allow_untested`); scoring through I4-2 and I4-3; `truest` selection with tie abstention;
  refusals mapped from `TypeMismatch` and `CallbackFault` to typed epistemic refusals,
  distinguishable from failure; a plain query of an arbitrated predicate becomes the arbitrated
  answer (Q1); `max_candidates` enforced; composite export of the result. The spike is a first
  commit that emits one trivial scope from `compiler.cpp` and runs it, so the lowering risk is known
  before the full slice is reviewed. If the slice exceeds 1,500 lines in review it splits at the
  eligibility pass.
- **Closes.** 1-2, 2.2-5, 3.2-3, 3.2-9, 3.4-6, 8.1-4, 4-35, 4-49, 4-51, 4-54, 5.1-1, 5.1-6, 5.1-17,
  5.1-19, 5.2-3, 5.2-17, 5.3-1, 5.3-7, 5.3-9, 5.3-10, 6.1-2, 6.1-12, 6.1-17, 6.1-20, 6.1-21, 6.1-26,
  8-3, 8.P8-2, 8.P8-3, 8.2-4e, 8.2-5g, 8.2-R3c (32).
- **Files.** new `epistemic_query.cpp`; `compiler_epistemic.cpp`; `agent_executor.cpp` (grant,
  refusal propagation); `vm/vortex_scoped_evaluation.cpp` only for a missing test;
  `tests/xuzz/vagent_epistemic_query_test.cpp` (new).
- **Depends on.** I4-5, I3-3b, I3-2c1, I4-4.
- **C++ and Vortex.** Emission and refusal propagation. The VM side already exists.
- **Tests.** A query never advances a counter, refreshes a timestamp or enqueues anything; repeated
  queries leave the arena size flat; `CutBelowFloor` has a named test; the refusal is not `Empty`;
  no eligible fact is not an unscored fallback; a query reads the committed verdict and live health;
  the cold-start `UNTESTED` path.
- **Gate.** CODE, VM, ARCH, EPI, ML.

#### I4-7. Query tokens, `ranked` and stale queries

*I4. Size: about 1,400 lines.*

- **Content.** The query token (signature, key and value tuple, observation IDs and versions,
  occurrences, context snapshot, state version) anchored as O3 decides; a generator whose owner
  holds stable IDs, scores and position outside the per-evaluation mark; each resume scopes its own
  scratch; `ranked` enumerates ties and fails only when exhausted; a change to belief or the state
  version gives `StaleEpistemicQuery`; the translator invalidates ranked continuations.
- **Closes.** 1-6, 2.1-9, 2.1-17, 2.1-41, U-9, 4-47, 4-53, 5.3-11, 6.1-28, 6.2-1, 6.2-2, 6.2-3,
  6.2-4, 6.2-5, 6.2-6, 8.2-3e, 8.2-5f (17).
- **Files.** `epistemic_query.cpp`; `compiler_epistemic.cpp`; `runtime_abi.hpp`;
  `epistemic_translate.cpp`; `tests/xuzz/vagent_epistemic_query_test.cpp`.
- **Depends on.** I4-6, O3.
- **C++ and Vortex.** IR; no host API.
- **Tests.** A ranked generator survives a mark release of its enumeration, not of the belief; a
  state bump between resumes is `StaleEpistemicQuery`; tokens survive resume and upgrade; exhaustion
  releases the continuation; a ranked body with an effect never reaches the VM.
- **Gate.** VM, CODE, ARCH, DIST, EPI.

#### I3-4a. Verification grants, surface and target resolution

*Verification loop. Size: about 1,400 lines.*

- **Content.** `EpistemicVerifyGrant` (verifier, actor, predicate and domain, evidence scope, epoch
  and expiry, action, maximum weight); a verification percept kind and the adapter check (missing,
  expired, mismatched, repeated or conflicting targets, out-of-scope evidence, excess weight);
  `confirm` and `refute` syntax and checker, legal only in a host-validated handler
  (`UnauthorizedVerification`); target resolution by observation ID and token match
  (`MissingVerificationTarget`, `ExpiredVerificationTarget`); the rule that a report source cannot
  certify itself without a distinct adjudicator; the verification record shape.
- **Closes.** 1-5, 2.1-18, 4-5, 4-9, 4-10, 4-11, 4-12, 4-13, 4-15, 4-16, 4-17, 4-21, 4-50, 6.3-1,
  6.3-2, 6.3-4, 8.2-4b (17).
- **Files.** `token.hpp`, `lexer.cpp`, `parser.cpp`, `ast.hpp`, `checker.cpp`; `agent_executor.hpp`,
  `agent_executor_epistemic.cpp`; `compiler_epistemic.cpp`;
  `tests/xuzz/vagent_epistemic_verify_test.cpp` (new).
- **Depends on.** V-D, I4-7, I3-2c2.
- **C++ and Vortex.** Grant and percept checks are authorization (C++); the syntax is the front end;
  the record is IR.
- **Tests.** Transport authentication alone confers no verification authority; self-issued and
  unauthorised verification refuse; a repeated or conflicting target refuses; weight outside (0, 1\]
  refuses; absence never justifies `refute` without an observation model and coverage.
- **Gate.** SEC, EPI, DIST, CODE, ML.

#### I3-4b. Intake journal and idempotency

*Verification loop. Size: about 1,400 lines.*

- **Content.** An authenticated intake turn that admits an owned receiver journal record on
  `d.epistemic` before acceptance is reported; a host-posted verification stays pending until intake
  commits (it no longer uses drop-on-settle); the issuer's pending record; dedup by issuer, event ID
  and payload; the cached outcome; `DuplicateVerificationPayload`; `VerificationTooOld`; replay
  floors that survive checkpoint, resume and the horizon; the audit of mesh receipt semantics
  (8.P5-2); ordinary dispatch semantics unchanged.
- **Closes.** 2.1-26, 2.1-28, 2.2-9, U-20, 3.3-5, 4-14, 6.3-23, 6.3-24, 6.3-25, 6.3-26, T-1, T-2,
  T-3, T-4, T-5, T-10, DF-5, 8.P5-2, 8.2-2d (19).
- **Files.** `agent_executor.cpp` (the pending-event path); `agent_executor_epistemic.cpp`;
  `compiler_epistemic.cpp`; `runtime_abi.hpp`; `tests/xuzz/vagent_epistemic_journal_test.cpp` (new);
  `design/` audit note.
- **Depends on.** I3-4a.
- **C++ and Vortex.** The pending-event and issuer-record handling is transport (C++); the journal
  is IR.
- **Tests.** An exact retry returns the cached outcome and relearns nothing; a failed intake
  publishes nothing; a different payload under the same ID is `DuplicateVerificationPayload`; a
  delivery below the floor mutates nothing; an ordinary failing user handler behaves as before; the
  journal survives resume.
- **Gate.** DIST, SEC, ARCH, CODE.

#### I3-4c. Processing turn and settle queue

*Verification loop. Size: about 1,400 lines.*

- **Content.** A separate processing turn after a successful non-speculative turn; queue entries on
  `$agent +d.epistemic` that own their payload cells and roll back with an enclosing mark; bounded
  retry with backoff; a terminal typed rejection recorded and reported; a `VerificationOutcome`
  message and the issuer removing its pending record only on a checked outcome; a lost response is
  re-emitted from the cache; a later plan failure cannot undo a published outcome. The outcome
  before I5-1 is typed `RecordedNotApplied`.
- **Closes.** 2.1-6, 2.2-6, 6.3-27, T-7, T-8, T-9, T-11, T-15, T-16, 8.P5-1, 8.2-4a, 8.2-4d (12).
- **Files.** `agent_executor.cpp`, `agent_executor_epistemic.cpp`; `compiler_epistemic.cpp`;
  `tests/xuzz/vagent_epistemic_journal_test.cpp`; `tests/fuzz/fuzz_epistemic_verification.cpp`
  (new).
- **Depends on.** I3-4b.
- **C++ and Vortex.** Turn scheduling and the outcome message are host C++; the queue and records
  are IR.
- **Tests.** Fail processing after intake, redeliver, commit once, lose the reply, redeliver, then
  fail a later plan: the journal, the cached outcome and the issuer state follow spec 6.3; a starved
  turn is never an invented label; extra turns are charged to the budget.
- **Gate.** DIST, ARCH, CODE, EPI, ML.

#### I5-1. Beta labels, the ledger and corrections

*I5. Size: about 1,500 lines.*

- **Content.** The label ledger on the calibration record (target identity and version, feature
  snapshot, scaler and model epoch, effective ID, predecessor, `y`, `q`, first-accept ordinal,
  evidence); immutable verification versions; a correction names its effective predecessor or is
  `VerificationConflict`; Beta state recomputed from the epoch prior plus the ledger sums, never
  incrementally; one effective adjudication per observation; competing values negative only for a
  declared exhaustive functional set; a correction of a retired observation fixes its label and
  never makes it current; transactional publication; ledger bounded and charged;
  `ExpiredVerificationTarget` beyond the horizon. Turns the I3-4c outcome into `Applied`.
- **Closes.** 2.1-29, 3.1-3, 8.1-6, 6.3-3, 6.3-5, 6.3-6, 6.3-7, 6.3-8, 6.3-9, 6.3-10, 6.3-11,
  6.3-12, 6.3-13, 6.3-17, 6.3-19, 6.3-21, 6.3-22, 6.3-28, T-6, 8.P5-3, 8.2-R4b (21).
- **Files.** `compiler_epistemic.cpp`; `epistemic_scoring.cpp`; `runtime_abi.hpp`;
  `tools/vagent-epistemic-vectors.py`; `tests/xuzz/vagent_epistemic_ledger_test.cpp` (new).
- **Depends on.** I3-4c, I4-2.
- **C++ and Vortex.** IR. No new host API beyond I3-4.
- **Tests.** The recompute against a closed form and the reference; corrections replace rather than
  add; two adjudicators cannot append two examples; gate acceptance alone never trains; the full
  loop: observe, query, verify, label, re-query changes the ranking; a label that cannot fit is
  `EpistemicBudget`.
- **Gate.** ML, EPI, NUM, DIST.

#### I4-8. Verdicts, innovation and slew gates, cold start

*I4. Size: about 1,400 lines.*

- **Content.** Ingestion computes a verdict (`ACCEPTED`, `REJECTED`, `UNTESTED`); the innovation
  gate with `r_tilde = r / max(w, eps)`; the slew gate with strict `dt > 0`; a host-supplied
  independent reference through a grant (O6), never the candidate or a fusion containing it; gate
  quantiles by dimension; a gate-rejection estimator with the same shape as the health fraction, the
  streak thresholds (three SUSPECT, five FAULTED) and inflation applied once; accepted measurements
  clear only the streak; cold start stays `UNTESTED` with no blame and no training.
- **Closes.** 3.1-4, 5.1-3, 5.1-9, 5.1-15, 5.1-18, 5.2-6, 5.2-10, 5.2-11, 5.2-12, 5.2-13, 5.2-14,
  5.2-15, 5.2-16, 5.2-18, 5.3-2, 5.3-3, 7-7, 8.2-2l (18).
- **Files.** `compiler_epistemic.cpp`; `epistemic_scoring.cpp`; `agent_executor_epistemic.cpp`
  (reference grant); `epistemic_policy.cpp`; `tests/xuzz/vagent_epistemic_gates_test.cpp` (new).
- **Depends on.** I5-1, I4-6, O6.
- **C++ and Vortex.** The reference grant is authorization; everything else is IR.
- **Tests.** Gate vectors against the reference; a rejection blames no sensor and trains nothing; a
  ramp drift below the gate and a common-mode drift are accepted but produce no calibrated claim; a
  candidate cannot corroborate itself; duplicates and out-of-order fixes never divide by zero.
- **Gate.** ML, SENS, NUM, EPI, SEC.

#### I4-9. Clustering, scalar fusion and `consensus`

*I4. Size: about 1,500 lines.*

- **Content.** Epsilon-adjacency single linkage with the `max_cluster_diameter` singleton rule;
  deterministic representatives; `t_star`; scalar isotropic fusion `r_f`, `x_f` over aligned,
  conditionally independent measurements; the reference gates but is not a measurement; the exported
  scalar coordinates and variance; `with uncertainty` binds the fused variance; no fused point when
  there is no eligible component, no reference, incompatible times or several components;
  `DependentMeasurements`.
- **Closes.** 1-7, 2.1-36, 3.2-10, 3.2-12, 4-48, 4-52, 5.2-4, 5.3-4, 5.3-5, 5.3-6, 5.3-12, 5.3-14,
  5.3-16, 5.3-17, 5.3-18, 5.3-19, 5.3-20, 6.1-27, 7-2, 7-10, 8.P2-2, 8.2-2h, 8.2-2i, 8.2-2j, 8.2-2k
  (25).
- **Files.** `epistemic_scoring.cpp`; `epistemic_query.cpp`; `tools/vagent-epistemic-vectors.py`;
  `tests/xuzz/vagent_epistemic_fusion_test.cpp` (new).
- **Depends on.** I4-8, I4-4, I4-7.
- **C++ and Vortex.** IR.
- **Tests.** Fusion against the reference; oversized-component determinism; a moving-target
  clock-window counterexample; zero or missing variance; unknown dependence refuses; fusion cost is
  charged to fuel and cells.
- **Gate.** ML, NUM, SENS, EPI, VM.

#### I4-10. Health directives and `assert ... with variance`

*I4. Size: about 1,000 lines.*

- **Content.** `reset`, `isolate`, `degrade` and `clear source` lower onto `healthEntry` and refuse
  `UnauthorizedHealthAction` outside a handler entered through an authenticated operator percept;
  the new `assert Term from Postfix with variance` form feeds arbitrated predicates through the
  percept envelope while base `assert` stays refused (X-15); a self-authored fact is scorable as
  testimony with an explicit time, frame and variance provider.
- **Closes.** 4-4, 4-6, 4-7, 4-8, 4-25, 4-46, 7-3 (7).
- **Files.** `parser.cpp`, `ast.hpp`, `checker.cpp`; `compiler_epistemic.cpp`;
  `tests/xuzz/vagent_epistemic_directives_test.cpp` (new).
- **Depends on.** I4-7, I3-2c2.
- **C++ and Vortex.** Front end and IR.
- **Tests.** A program cannot hold a token; directives behave exactly as the host API (mirror,
  epoch, receipt); an unprivileged plan cannot fabricate a source or time annotation.
- **Gate.** SEC, EPI, CODE, DIST.

#### I4-11. I4 close-out

*I4. Size: about 900 lines plus documents.*

- **Content.** Audit gates 2 and 3 against the matrix; `check-runtime` and `run` tests for every
  diagnostic; a query fuzzer; measured costs (emitted size, peak cells, VM cycles under sustained
  ingestion and repeated queries); the budget recommendation for O7; the user-guide worked examples
  with a complete policy; the I4 whole-iteration ML review over the learning loop.
- **Closes.** 2.2-1, 4-36, 8-2, 8.P1-6, 8.P4-1, 8.P4-2, 8.2-R5a (7).
- **Files.** `tests/xuzz/vagent_cli_test.cpp`; `tests/fuzz/fuzz_epistemic_query.cpp` (new);
  `docs/vagent-user-guide.md`; `i4-validation.md` (new).
- **Depends on.** I4-10.
- **C++ and Vortex.** None.
- **Tests.** The whole I4 matrix subset; fuzzer; the measurements.
- **Gate.** ML (whole iteration), ARCH, CODE, EPI, NUM.

#### I5-2. Refutation, defeaters and a revision route

*I5. Size: about 1,200 lines.*

- **Content.** Refutation removes matching assertional support by rank excision and keeps a bounded,
  inspectable defeater (event identity, evidence, acquisition window) that suppresses replayed and
  relayed copies during the horizon; a fresh authorised observation is new evidence; a corrected
  positive verdict clears the defeater and restores support only if the observation is still current
  and eligible; re-derivation is checked against the defeater. This is the revision route for
  arbitrated predicates (X-22).
- **Closes.** 2.1-30, DF-1, DF-2, DF-3, DF-4, DF-6, 8.2-R1d, 8.2-R1e, 8.2-R2b (9).
- **Files.** `compiler_epistemic.cpp`; `runtime_abi.hpp`;
  `tests/xuzz/vagent_epistemic_defeater_test.cpp` (new).
- **Depends on.** I5-1.
- **C++ and Vortex.** IR.
- **Tests.** Retracting or refuting one source preserves the other; a correction never revives a
  retired reading over a newer sequence; a replay inside the horizon is suppressed; the defeater is
  bounded and survives resume.
- **Gate.** EPI, ML, DIST, ARCH.

#### I5-3. Feature schema, `context` and the normalizer

*I5. Size: about 1,300 lines.*

- **Content.** Policy encoding `vagent-policy-3` with feature names and floors; `context` syntax and
  `UnknownContextFeature`; a fixed-schema feature vector and normalization snapshot stored per
  observation; fit during warm-up and frozen per model epoch; clipping and a positive floor; a
  schema change is a typed migration or a new epoch; `features` is no longer always empty.
- **Closes.** 2.1-16, 3.3-1, 3.3-2, 3.3-9, 8.1-8, 4-32, 4-38, 4-41, 4-44, 8.2-R1b (10).
- **Files.** `epistemic_policy.cpp`; `parser.cpp`, `checker.cpp`; `compiler_epistemic.cpp`;
  `epistemic_translate.cpp`; `tests/xuzz/vagent_epistemic_features_test.cpp` (new).
- **Depends on.** I5-2.
- **C++ and Vortex.** Front end and IR.
- **Tests.** Two sources with different features and labels keep them apart; a schema change refuses
  or opens a new epoch; the normalizer is frozen and its bits survive resume and upgrade; an unknown
  feature refuses statically.
- **Gate.** ML, NUM, CODE, EPI.

#### I5-4. Online SGD, warm-up and replay

*I5. Size: about 1,500 lines.*

- **Content.** The logistic model with a clamped sigmoid, `eta_n = eta0/(n+1)`, a weighted data
  gradient and independent L2; hyperparameter validation; Beta during warm-up and an untrained model
  never called calibrated; no Beta variance on logistic outputs; the checker rejects `lcb` with SGD;
  replay from the ledger in first-accept order over frozen normalized features within fuel and cell
  budgets, rebuilt in transactional scratch and published with the label and learned state together.
- **Closes.** 1-3, 2.1-15, 3.3-3, 3.3-6, 3.3-7, 3.3-8, 3.4-2, 3.4-3, 4-45, 6.3-14, 6.3-15, 6.3-16,
  8.P3-1, 8.P3-2, 8.2-2e (15).
- **Files.** `epistemic_scoring.cpp`; `compiler_epistemic.cpp`; `tools/vagent-epistemic-vectors.py`;
  `tests/xuzz/vagent_epistemic_sgd_test.cpp` (new).
- **Depends on.** I5-3.
- **C++ and Vortex.** IR. No C++ model evaluator.
- **Tests.** Independent reference vectors; replay from the ledger equals the incremental result
  within stated error; a correction replays deterministically; a failure leaves the old adjudication
  and model intact; replay cost against the budget.
- **Gate.** ML, NUM, VM.

#### I5-5. Drift epochs, horizons and validation

*I5. Size: about 1,200 lines.*

- **Content.** A privileged drift percept (under a grant) starts a new epoch that resets weights,
  bias, normalizer, Beta state and label count and expires pending labels (`ExpiredLabel`); the
  feedback horizon; prequential loss and calibration retained, with abstention when validation does
  not support deployment; calibration and validation data kept independent of selection; an
  authorised epoch reset clears the ledger; same-program resume preserves model bits.
- **Closes.** 2.1-40, 3.1-6, 3.3-4, 3.3-10, 3.3-12, 6.3-20 (6).
- **Files.** `compiler_epistemic.cpp`; `agent_executor_epistemic.cpp`;
  `tests/xuzz/vagent_epistemic_epoch_test.cpp` (new).
- **Depends on.** I5-4.
- **C++ and Vortex.** The drift grant is authorization; the rest is IR.
- **Tests.** A label across an epoch is `ExpiredLabel`; ordinary new-epoch training needs fresh
  eligible targets; a stale label cannot train a replacement; abstention when validation fails.
- **Gate.** ML, DIST, SEC, ARCH.

#### I6-1. Native model effects and keyed completions

*I6. Size: about 1,400 lines.*

- **Content.** An opt-in staged effect carrying the observation ID; capacity-bounded asynchronous
  evaluation through the existing native effect seam; a completion posted as a feature percept with
  model identity, version, schema, observation ID and epoch; a completion applies only on an exact
  full-key match; queries read completed cached features only; a raw softmax or detector score is
  not a calibrated probability; the full inference configuration is versioned with the calibrator.
- **Closes.** 1-8, 2.1-39, 3.4-8, 3.4-9, 3.4-10, 3.4-11, 3.4-12, 4-55, 6.3-18, T-14, 8.P6-1, 8.P6-2,
  8.2-2g (13).
- **Files.** `host_bindings.{hpp,cpp}`; `agent_executor.cpp`, `agent_executor_epistemic.cpp`;
  `compiler_epistemic.cpp`; `tests/xuzz/vagent_epistemic_native_test.cpp` (new).
- **Depends on.** I6-D, I5-5.
- **C++ and Vortex.** The model runtime and the effect queue are the OS and device boundary the
  rules allow in C++; the feature record and the key check are IR.
- **Tests.** A cancelled, stale or mismatched completion never overwrites features; a completion for
  a superseded observation expires; latency and memory bounds hold.
- **Gate.** ML, DIST, SEC, ARCH, CODE.

#### I6-2. Epoch fencing, drain on upgrade and stress

*I6. Size: about 900 lines.*

- **Content.** Model, schema and incarnation changes cancel pending completions; a live upgrade
  drains or cancels native work first and never transplants flight or request tables; late
  completions expire on key mismatch; a checkpoint refuses in-flight effects.
- **Closes.** 8.2-4h, 8.2-R3a (2).
- **Files.** `agent_executor.cpp`; `epistemic_translate.cpp`;
  `tests/xuzz/vagent_epistemic_native_test.cpp`.
- **Depends on.** I6-1.
- **C++ and Vortex.** Host cancellation and drain.
- **Tests.** Stale feature completions cannot train a replacement observation; repeated epoch
  changes under load stay bounded.
- **Gate.** DIST, ARCH, ML, SEC.

#### T2. Translator completion and the upgrade claim

*Translator. Size: about 1,000 lines.*

- **Content.** Extend and prove the translator for calibration, groups, tokens, journals, ledgers,
  defeaters, features and normalizer, epochs and completions; the typed migration or new epoch for a
  changed model, schema or incarnation; a populated intention rank refuses with a named error (O1);
  the full lifecycle tests of 8.2-R2; the property test that resume and upgrade agree, over every
  structure.
- **Closes.** 2.1-23, U-4, U-5, U-6, U-7, U-8, U-10, U-11, U-21, U-22, 3.1-15, T-12, 8.P1-3, 8.2-5b,
  8.2-5c, 8.2-R2c, 8.2-R2d, 8.2-R3b (18).
- **Files.** `epistemic_translate.cpp`; `upgrade_schema.*`;
  `tests/xuzz/vagent_epistemic_translate_test.cpp`.
- **Depends on.** I6-2, O1.
- **C++ and Vortex.** As T1.
- **Tests.** A populated actor with every structure at its bound upgrades, rolls back, and resumes;
  old observations become ineligible after an epoch change while retained IDs and floors still
  reject replay.
- **Gate.** ARCH, DIST, EPI, ML, CODE.

#### F1. Guards, documents and the final spec

*Final. Size: about 800 lines plus documents.*

- **Content.** A test that fails when a new `OpcodeKind` is missing from the VQL and VPL name tables
  or the artifact validator (X-21); a test that the bounded `check` frontend is unchanged; the
  `exp`/`ln`/`sqrt` benchmark; `tools/check-p19-traceability.py`; the user guide with the
  aiding-update rule, the native-estimator contract, the gate caveat and a warrant-index wording;
  spec citations refreshed and the v1.6 change history.
- **Closes.** 3.1-7, 3.2-11, 5.2-2, 7-9, 8.P7-2, 8.2-1e, 8.2-R5b, 9-6 (8).
- **Files.** `tests/xuzz/vortex_scoped_evaluation_test.cpp` or a new guard test;
  `tests/xuzz/vortex_benchmark_test.cpp`; `tools/check-p19-traceability.py` (new);
  `docs/vagent-user-guide.md`; the spec.
- **Depends on.** T2.
- **C++ and Vortex.** None.
- **Tests.** The guards; the traceability checker.
- **Gate.** ARCH, CODE, EPI, ML (claims in the documents).

#### F2. Final acceptance

*Final. Size: about 200 lines plus the record.*

- **Content.** Run every suite and fuzzer headless, `make check`, the user-guide journeys; re-run
  the Standing checks; the whole-system round by every persona; write `final-validation.md` against
  section 8 of this plan; set the README to Accepted.
- **Closes.** 1-10, 8.2-0a, 9-5 (3).
- **Files.** `final-validation.md` (new); `traceability.md`; `README.md`.
- **Depends on.** F1.
- **C++ and Vortex.** None.
- **Tests.** Everything.
- **Gate.** All personas; ML whole system.

## 5. Review gates

Personas: **ML** machine-learning expert; **NUM** numerics; **ARCH** architect; **DIST** distributed
systems; **CODE** codebase and compiler; **VM** Vortex and VM; **EPI** epistemology; **SENS**
sensor-fault and hardware; **SEC** security. The README rule holds: the ML expert reads the design
before the implementation and the source, tests, mathematics and lifecycle before acceptance, for
everything touching scoring, priors, decay, pooling, gates, fusion, features, labels, learning,
replay or native completions. ARCH and DIST read cross-cutting lifecycle, turn, transport and
upgrade work. No reviewer implements what they review.

| Slice          | Reviewers                                            | ML gate |
| -------------- | ---------------------------------------------------- | ------- |
| E0             | ARCH, EPI read the amendments against the code       | no      |
| T0             | ARCH, DIST, CODE                                     | no      |
| E1             | CODE, ARCH. The review is of the move, not the logic | no      |
| I3-2a          | ML (design and implementation), SENS, CODE, NUM      | yes     |
| I3-2b          | ARCH, DIST, CODE, VM                                 | no      |
| I3-2c1         | ARCH, DIST, EPI, SENS, CODE                          | no      |
| I3-2c2         | SEC, DIST, ARCH, CODE                                | no      |
| T1             | ARCH, DIST, CODE, EPI, SEC                           | no      |
| I3-3a          | SEC, DIST, ARCH, CODE                                | no      |
| I3-3b          | SENS, DIST, ARCH, CODE, ML                           | yes     |
| I3-3c          | SENS, DIST, ARCH, EPI, ML                            | yes     |
| I3-3d          | SEC, SENS, EPI                                       | no      |
| I4-1           | ML, NUM, ARCH, CODE, EPI, DIST                       | yes     |
| I4-2           | ML, NUM, VM                                          | yes     |
| I4-3           | ML, NUM, EPI                                         | yes     |
| I4-4           | ML, SEC, ARCH, EPI                                   | yes     |
| I4-5           | CODE, EPI, VM                                        | no      |
| I4-6           | CODE, VM, ARCH, EPI, ML                              | yes     |
| I4-7           | VM, CODE, ARCH, DIST, EPI                            | no      |
| I3-4a          | SEC, EPI, DIST, CODE, ML                             | yes     |
| I3-4b          | DIST, SEC, ARCH, CODE                                | no      |
| I3-4c          | DIST, ARCH, CODE, EPI, ML                            | yes     |
| I5-1           | ML, EPI, NUM, DIST                                   | yes     |
| I4-8           | ML, SENS, NUM, EPI, SEC                              | yes     |
| I4-9           | ML, NUM, SENS, EPI, VM                               | yes     |
| I4-10          | SEC, EPI, CODE, DIST                                 | no      |
| I4-11          | ML (whole iteration), ARCH, CODE, EPI, NUM           | yes     |
| I5-2           | EPI, ML, DIST, ARCH                                  | yes     |
| I5-3           | ML, NUM, CODE, EPI                                   | yes     |
| I5-4           | ML, NUM, VM                                          | yes     |
| I5-5           | ML, DIST, SEC, ARCH                                  | yes     |
| I6-1           | ML, DIST, SEC, ARCH, CODE                            | yes     |
| I6-2           | DIST, ARCH, ML, SEC                                  | yes     |
| T2             | ARCH, DIST, EPI, ML, CODE                            | yes     |
| F1             | ARCH, CODE, EPI, ML (claims in the documents)        | yes     |
| F2             | All personas; ML whole system                        | yes     |
| I3-2D (design) | ML, ARCH, DIST, EPI, SENS, SEC, CODE                 | yes     |
| I3-3D (design) | SENS, DIST, SEC, ARCH, ML                            | yes     |
| I4-D (design)  | ML, NUM, EPI, ARCH, CODE, VM, DIST                   | yes     |
| V-D (design)   | DIST, SEC, EPI, ML, ARCH                             | yes     |
| I5-D (design)  | ML, NUM, EPI, DIST                                   | yes     |
| I6-D (design)  | ML, DIST, SEC, ARCH                                  | yes     |

Procedure per gated unit: round 1 with all named personas in parallel; a failed round is fixed and
re-reviewed by the personas that failed it, plus any whose area the fix touched; a unit that fails
three rounds returns to design. The final F2 review is a whole-system round by every persona.

## 6. Risks and their controls

| Risk                                                                                                                                                                                                  | Control                                                                                                                                                                                                                                                                                                      |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `compiler.cpp` is 7,617 lines and its `Gen` class is one type of about 7,100 lines in an anonymous namespace; `agent_executor.cpp` is 5,673                                                           | E1 moves `Gen`'s declaration to a private header and the epistemic emitters and `AgentExecutor` members to new files, with zero behaviour change, proved by byte-equal emitted IR over the test corpus and unchanged suites. Afterwards the 400-line growth rule applies.                                    |
| E1's diff is large and other agents edit these files                                                                                                                                                  | E1 runs first and alone; the lead rebases before and after; its review is mechanical (moved-line check by tool)                                                                                                                                                                                              |
| Turn and fuel budgets: ingestion is about 4.4k cycles; a reset is O(K·H) with a settle per support; a query runs `Exp` and `Ln` per candidate                                                         | Budget parity rule; each slice records worst-case cycles for default bounds against the instruction grant; `max_candidates` is enforced in I4-6 so no query is unbounded; reset is measured before I3-2c1 lands, as its design requires                                                                      |
| Cell budget: the ceiling is 24,576 cells, a record costs about 80, and the default holds about 18 fully active principals; calibration, groups, tokens, journals, ledgers and features all draw on it | Every design prices its structures in cells and bounds them; O7 asks the owner whether to raise the ceiling or keep it and document; no slice prunes silently, a full budget is `EpistemicBudget`                                                                                                            |
| Checkpoint limit: `vagent.value_operations` is 65,536 and 24k cells checkpoint while 32k do not                                                                                                       | `AFullEpistemicBudgetStillCheckpoints` is re-measured by each slice with its structure at the bound; the translator's peak copy (old plus new graph) is priced in T1 and T2                                                                                                                                  |
| Drift the gates cannot see: common-mode drift, drift slower than the gate tolerance, and cold start with no reference                                                                                 | I4-8 never marks a reading `ACCEPTED` without a host-supplied reference; fusion at cold start returns no calibrated value; tests ramp one sensor below the gate and move all sensors together, and assert that no calibrated claim is made; I5-5 handles declared drift epochs; F1 documents the limit (7-9) |
| Code drifts from the spec and the records (34 foreign commits already touched P19 files)                                                                                                              | E0 reconciles now; each slice starts with `git log <last accepted>..HEAD` over the P19 files and records any foreign change; the matrix is updated in each acceptance commit; F1 adds `tools/check-p19-traceability.py`, which fails if a Done row cites a test or file that no longer exists                |
| Intermediate states: from I3-4c to I5-1 a verification is recorded and acknowledged but not applied; until I5-1 and I5-2 an arbitrated predicate has no revision route                                | No production caller issues a verify grant before F2; I3-4c's outcome is typed `RecordedNotApplied` and I5-1 reprocesses the journal in its own test; the two ship in one acceptance window                                                                                                                  |
| Floating-point results differ across hosts (libm) while spec 3.3 asks for deterministic replay and a checkpoint can move hosts                                                                        | NUM and ML define the contract in I4-D: results compared to an independent reference within stated relative error; tie tolerance far above libm error; replay deterministic on one host and within the stated error across hosts; documented                                                                 |
| Policy encoding changes alter every policy hash, which the upgrade schema compares                                                                                                                    | One bump per version (`vagent-policy-2` in I4-1, `-3` in I5-3), old readers deleted, tests regenerate expected hashes; a checkpoint with an old policy refuses by name                                                                                                                                       |
| Spec ambiguities that no code can decide (`t_star`, the trusted reference, profile variance)                                                                                                          | O6 asks for the rulings before I4-D completes                                                                                                                                                                                                                                                                |
| Review load: six designs and 36 slices                                                                                                                                                                | Designs first; at most three rounds per unit; parallel persona rounds; design review overlaps the previous slice's review                                                                                                                                                                                    |

## 7. Owner decisions

| #   | Decision                                                              | Options                                                                                                                                                                                                                                                                                                                                                       | Recommendation                                                                                                                                                                                                                                                         | Needed before |
| --- | --------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------- |
| O1  | What the live-upgrade claim covers                                    | (a) full vagent, after P17; (b) supported-agent upgrade with a typed refusal of active intentions; (c) hold P19 acceptance for P17                                                                                                                                                                                                                            | (b); amend 8.2 gate 0 wording                                                                                                                                                                                                                                          | T2            |
| O2  | Where scoring, pooling, clustering and SGD live                       | (a) emitted IR in new files under `vagent/runtime/` (b) a `std:` package in `vortex_stdlib`, plus a loader so actors can use it (c) emitted IR inside `compiler.cpp`                                                                                                                                                                                          | (a). Actors never load `vortex_stdlib` (X-17); (b) adds a loader and a second place for the numerics; (c) grows the file E1 shrinks. Amend the P19.2 and P19.3 file lists                                                                                              | I4-D          |
| O3  | Where query tokens hang                                               | (a) on an intention, as 2.1 says, which cannot exist until goal lowering; (b) on the agent's `d.epistemic` rank; (c) on the handler activation, dropped at turn end                                                                                                                                                                                           | (b), with a retention bound set in I4-D. Verification arrives in a later turn, so (c) cannot serve it. Amend 2.1                                                                                                                                                       | I4-D          |
| O4  | Scope of the signed-ingress adapter (I3-3a)                           | (a) build a reference adapter in tree (Ed25519 or X.509-delegated signature over canonical observation bytes, alias binding policy); (b) leave to hosts and test against fakes                                                                                                                                                                                | (a). Without it 2.1-45 and the authentication rows hold only against fakes. The mesh authenticates nodes through device delegation, but the actor `Envelope` (`wire.hpp`) carries no origin signature, so a sensor behind a gateway cannot be authenticated end to end | I3-3D         |
| O5  | The spec amendment bundle (v1.6)                                      | Approve E0 to record: replayed sequence is `StaleSequence`; health thresholds are settings and the refusal fraction is event-indexed, the consecutive streak stays for gate rejections; refusal charging; bindings not checkpointed; `clear`; P19.5 file names; extra scope opcode meanings; stale baseline and section 9 row; slot reclamation by checkpoint | Approve                                                                                                                                                                                                                                                                | E0            |
| O6  | Consensus inputs the spec leaves open                                 | `t_star`: the query's declared time, else the newest accepted observation; the trusted reference: a host-granted percept with an issuer that is no bound source; profile variance (5.2-6): keep or drop                                                                                                                                                       | `t_star` declared with a default of the query snapshot time; reference by grant; drop the profile-variance path for v1 and require variance on every continuous observation, as ingress does today                                                                     | I4-D          |
| O7  | Epistemic cell and checkpoint budget                                  | (a) keep 24,576 cells and `value_operations` 65,536 and document; (b) raise both after measurement in I4-11; (c) partition the ceiling per structure                                                                                                                                                                                                          | (a) until I4-11 measures; then (b) if the default holds fewer than the 128 sources `max_sources` allows. No slice prunes silently                                                                                                                                      | I4-D          |
| O8  | Who sees the operator's `clear` and `reset` outcomes and for how long | host log only, as designed, or a bounded arena audit that survives restarts                                                                                                                                                                                                                                                                                   | the arena audit list of I3-2 (bounded, never cleared by reset) plus the host log; revisit at F2                                                                                                                                                                        | I3-2D         |

## 8. Definition of done for P19

F2 may say P19 is implemented when all of the following hold, each with evidence in the final
validation record.

1. **Matrix.** Every row is Done, Deferred by spec, Informational, or Standing with its check re-run
   at F2. No row is Partial, Missing or Designed. `tools/check-p19-traceability.py` passes.
1. **Gates.** Each numbered gate and each regression bullet of spec 8.2 (rows `8.2-0` to `8.2-Z`) is
   Done, with the test that satisfies it named. Gate 0 reads as O1 decides.
1. **Suites.** `vagent_runtime_test`, `vagent_actor_test`, `xuzz_test`, and `make test` pass
   headless; the fuzz targets, including the new query, verification and adapter fuzzers, run a
   fixed iteration count without a finding; `make check` is clean on the touched files.
1. **ML.** Every slice marked ML has a PASS report with no open BLOCKING or MAJOR finding, and a
   whole-system ML review of I4, I5 and I6 together has passed. The independent reference vectors
   are reproducible from `tools/` with one command.
1. **Journeys.** The user-guide worked examples run headless as tests: a fleet read with `truest`,
   `ranked` and `consensus`; a fault, an operator isolate and reset; a verification with redelivery
   and a lost reply; a live upgrade of a populated actor; a checkpoint and resume at a full budget.
1. **Budgets.** The validation record gives the measured worst case for ingestion, a query at
   `max_candidates`, a reset, a replay and a checkpoint, each against its limit.
1. **Documents.** Spec v1.6 carries the O5 amendments and a truthful baseline; README says Accepted;
   the user guide carries a complete policy example and no unsupported certainty claim.
1. **Limits stated.** The final record lists what P19 does not claim: the Deferred rows (exact
   probabilities, matrix and correlated fusion, derived-rule arbitration, bandits, automatic
   change-point detection), unattended drift that no gate can see, and the upgrade boundary of O1.

## 9. Questions for the reviewers

- **Architect.** Is E1 worth its conflict cost, or should new code go in new files without moving
  the old? Is the token anchor (O3) the right seam? Is T1 before I3-3 the right place?
- **ML expert.** Is I5-1 before I4-8 the right place for Beta labels? Is the reference-vector
  approach (an independent implementation in `tools/`) enough for the I4 gate? Should the health
  fraction and the gate-rejection estimator share one shape (as I3-2 review round 2 suggests)? Is
  the cross-host tolerance contract in the risk table the right one?
- **Distributed-systems expert.** Is the verification loop's split (intake journal, processing turn,
  issuer pending record) at the right granularity? Does the arena-side reset epoch (I3-2D) remove
  the need for a host counter? Is the quiescence rule for upgrade (no pending settle, no queued
  health action, no pending verification) complete?
