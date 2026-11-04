# P19 remaining-work plan

Status: draft 3, for the round-3 review by an architect, an ML expert and a distributed-systems
expert. It answers the round-2 reviews ([plan-review/](plan-review/): all FAIL, 8 MAJOR: architect
M1-M3, distributed M1-M3, ML M1-M2, and 28 MINOR) and applies the lead's rulings in
[decisions.md](decisions.md): P2-1 to P2-5, the revised D2, O1 with amendment A5, and R3-11. Section
9 maps every round-2 finding to its change. Authority:
[P19 v1.5](../../vagent-dynamic-source-weighting-and-epistemic-arbitration.md) and the
[traceability matrix](traceability.md), which this plan consumes: every open row of the matrix is
closed by exactly one slice below. Process: the [README](README.md) review-gate rule, unchanged.

**State of the work.** T0 is done and committed (`4a912c69`; review by DIST and CODE, round 1 FAIL,
round 2 PASS). It holds the shared upgrade guards and the control-post wakeup; the typed refusals,
the settings loader and the TUI label that draft 2 listed under T0 were not part of it and are
re-assigned below. E1 is being implemented now in an isolated worktree as specified in draft 2. The
I3-2 design is at revision 3; its round-3 reviews are in, five of seven at the time of the rulings
(the ML and hardware reports were still owed), and the lead's rulings R3-1 to R3-11 are recorded in
decisions.md. No other implementation slice has started. Code baseline: `3b86b36d`, whose code is
that of `4a912c69`; the 34 commits after `8ec71fc6` made outside the gate process are taken as the
code of record, and the later commits on this branch change documents only except T0.

## 1. Summary

Of 468 requirements, 90 are Done, 354 are open (111 Partial, 231 Missing, 12 Designed), and the rest
are deferred by the spec, standing constraints or informational. What is built is the ingestion half
of P19: checked ingress, per-source observation history, profiles, the policy syntax and checker,
checkpoint and resume, the math opcodes and the scoped evaluation primitive. What is missing is
everything that reads that state: no arbitrated query, score, verdict, gate, label or fusion exists,
so no actor program reaches a scope. Nothing outside `AgentExecutor` tests calls `bindSource` or
`observe` (X-3). Two defects were live when the matrix was written, the second upgrade path skipping
the populated-epistemic-root refusal (X-1) and control posts not waking the reactor (X-27); T0
closed both.

The plan has 42 implementation slices of at most about 1,500 lines each and eight design slices. In
order:

1. **Foundations.** E0 records the rulings and refreshes the records (documents only). T0 (done)
   closed the upgrade defect for both entry points and the missed wakeup. E1 moves the code
   generator's declaration to a private header, so new epistemic code lives in new files by area
   (section 4.2) and the two large files stop growing. It moves no existing body.
1. **I3-2, source health** (4 slices, after a third design revision): the estimator, settle after
   commit, the host side of health actions with a stub turn, then the Vortex health turn. It writes
   no version cell (R3-11).
1. **T1, translator spine**, after its own design (T1-D): the typed refusal channel for resume and
   upgrade (P2-1), the owned-state shape version and its guard, and the default translator, so every
   later owned structure is born with its upgrade path and its parity test.
1. **I3-3, signed ingress, clock mapping, heartbeats and sensor reports** (4 slices).
1. **I4 and the first learning slice.** Two numeric and structural designs (I4-D1, I4-D2) settle the
   reference vectors, the cross-host contract, the record shapes, the caps and the open model
   questions. Then the policy and calibration records and the epistemic-state version (I4-1a,
   I4-1b), scoring as `std:` routines (I4-2), **the label ledger and its single apply routine
   (I5-1a)**, pooling, dependence groups, syntax, `truest`, ranked continuations and commit-point
   verification tokens (I4-3 to I4-7b). From I4-6 on a source's weight can differ from its prior,
   written only through the apply routine.
1. **A production path.** W1 wires the signed-observation adapter to `vagent run`, so a shipped
   binary can ingest a signed reading and answer a ranked query. W2 does the same for verification.
1. **Verification loop** (I3-4a to I3-4c, then I5-1b): grants, intake journal and replay floors, the
   issuer side, the processing turn that applies real labels, then competing values and corrections.
1. **I4, second half** (verdicts and gates, clustering and fusion, directives, close-out).
1. **I5, the rest** (defeaters, features, online SGD, drift epochs), **I6** (native model
   completions), **T2** (typed migrations and the full-bound lifecycle), **F1** and **F2**
   (documentation, guards, final acceptance).

No decision is open for the lead (section 7): the meaning of "deterministic" across hosts is ruled
(D1) and so is the fork and rollback handling of a resumed verification journal (D2). Everything
else that draft 1 sent to the owner is ruled in `decisions.md` or delegated to a named design.

## 2. Standing rules for every slice

These apply to each slice below and are not repeated in it.

- **Process.** A slice has a design (or a section of one), an implementation, and a validation
  record appended to the iteration's validation file. The reviewers named for the slice read the
  exact input and write reports to the project folder. A slice is accepted only with no BLOCKING and
  no MAJOR finding open; there is no provisional acceptance. The lead builds and commits. The matrix
  is updated in the commit that accepts the slice, with evidence for each row it closes. Each ML
  gate report lists the claims it passes and the claims it reserves to a later slice.
- **Headless and isolated.** Every run uses the Makefile's XDG redirects and offscreen drivers.
- **Where code goes.** Learned state and every algorithm run in the Vortex VM. The pure numerics
  (Beta, decay, pooling, gates, fusion, SGD) are `std:` module routines built in `vortex_stdlib`
  (the spec's P19.2 and P19.3 file list, lead ruling O2), each a pure routine with a fixed call ABI,
  built through a small emitter library and run from a unit test in a bare `WorkerContext`, with no
  agent. Pooling, the single-linkage walk and SGD are not scalar: I4-D2 states where the summation
  loop lives (emitted query IR calling a scalar routine per element, or a routine over an arena list
  head), how the bare-`WorkerContext` test feeds it, and that the sum order is a numeric decision of
  the reference. The bare-`WorkerContext` tests are `tests/xuzz/vortex_stdlib_epistemic_*` and run
  in `xuzz_test`; `tests/xuzz/vagent_*` run in `vagent_actor_test`. A new fuzz target needs its
  `fuzz-target` line and an entry in the `fuzz:` list of the `Makefile`, which the slice names in
  its files. Routines bound to the agent's record layout (ledger apply, health, eligibility) are
  emitted by the actor compiler. C++ is allowed only for what the repository's rule allows: the VM,
  clocks, transport, authorization, native effects, and the compiler front end and graph builders
  that emit IR. Each slice states its C++ and why.
- **Emitters are free functions.** From E1 on, a new emitter is a free function taking `Gen&` in its
  own file, so the private header stops changing.
- **No sentinels, no shims.** Optional values are `std::optional` or `std::expected`. A changed
  encoding bumps its version and deletes the old reader. A reader that cannot parse a state it is
  handed refuses by name and number.
- **One refusal channel.** Every typed refusal of resume and upgrade (policy-hash or policy-tag
  mismatch, unknown shape version, populated state with no translator, an active intention rank, a
  second live resume of one checkpoint) goes through the vocabulary T1 builds from T1-D (P2-1): a
  checked pre-flight that returns the typed reason and the offending numbers before
  `runtime_.resume` and before an upgrade mutates anything. A slice that adds a refusal after T1
  cites it and tests the typed value, not a log line. Before T1 no slice adds one.
- **Version rules.** Three numbers, kept apart (R3-11 moved the other two out of I3-2). The **policy
  tag** (`vagent-policy-N`) is bumped by the slice that changes the policy encoding;
  `vagent-policy-2` is live (`epistemic_policy.cpp:343`), so the next is `vagent-policy-3`, and a
  test asserts the tag string and that the previous tag is refused by name. The **shape version** is
  one integer at the head of the agent's `d.epistemic` rank, written from T1 and only for an agent
  whose program declares an arbitrated predicate (so an unrelated agent keeps upgrading). Resume,
  upgrade and the translator compare the saved value with the one the new program mints, by number,
  and never copy it; any slice that renames a field, changes a field's kind, moves a rank or changes
  a meaning bumps it. An additive field whose absence reads as a documented initial value does not.
  The **epistemic-state version** (X-12; the "state counter" of draft 2) is a separate cell, born in
  I4-1b with its first writer. Every committed change of a source's derived health state, every
  health action, supersession, policy change, calibration reset and label bumps it once, and a drift
  of `R` or `N` never does (ML minor 4); a continuation, a token and the `StaleEpistemicQuery` check
  read it. It is not carried across resume or upgrade, because continuations die there. It is not
  called an epoch, because `calibration_epoch`, `model_epoch` and the reset epoch already exist.
- **Host state against arena state.** Bindings, aliases, grants, the reference grant, the ingress
  outcome log, coalesced charges, queue counters and the in-flight reading snapshot are host memory:
  losing one can only cause a refusal or a delayed charge and must never change a cached outcome.
  Replay floors, deduplication, journals, cached outcomes, reset epochs, latches, issuer pending
  records and retry attempt counts are arena state. A slice that adds host state says what a resume
  loses and why that cannot admit a replay (the profile's last sequence and incarnation are in the
  arena), and tests it; it also says which host state an in-process upgrade keeps, since the pid
  survives it and only resume mints a new one. Per-source and per-issuer arena state is created only
  when the event that needs it commits: an authenticated caller with no grant leaves no cell.
- **Translator parity (from T1).** A slice that adds an owned structure adds its translator code in
  `epistemic_translate_<area>.cpp` and a populated-state test that runs through **both** state
  copiers (resume's `restoreState` and upgrade's, naming the upgrade road it drives until T1-D
  settles the collapse), asserts the shape version, and, where the structure carries learned state,
  asserts bit equality (doubles compare by bit pattern, so `-0.0` differs from `0.0`). Its file list
  names the translator file. Without it the slice is not accepted. At the end of I3, I4 and I5 a
  script (`tools/check-translator-coverage.py`, written at T1) lists every structure the iteration
  added and the test that covers it; the iteration's last slice fails review if a structure has
  none.
- **Numeric decisions.** A golden vector that feeds a decision carries its margin (relative distance
  to the decision boundary), and a test fails if the margin is below 1e-12, except for ties built
  from bit-identical inputs, which are exact on every host. No test pins a count that sits on a
  boundary (the health recovery count is "about 64", not 64).
- **Budget parity.** A slice that adds cells re-measures `AFullEpistemicBudgetStillCheckpoints` with
  its structure at its bound, and records the worst-case VM cycles for default bounds and the margin
  to the actor's instruction grant. A slice whose worst case exceeds half the default turn grant
  fails review.
- **File growth.** After E1, a slice adds at most 400 lines to any file that exists at E1 and is
  over 1,500 lines (`compiler.cpp`, `agent_executor.cpp`, `runtime_abi.hpp`, `parser.cpp`,
  `checker.cpp`, `vortex_stdlib.cpp`), and no file in the area table of section 4.2 grows past about
  3,000 lines.
- **Settings.** Every new limit is a `defaultSettingSpecs()` entry with a schema, a `settings::`
  name and an expected-list entry in `system_docs_test.cpp`. No naked constants in algorithms.
- **Format.** clang-format 19 on C++, mdformat and mdl on Markdown, in the same commit. Evidence for
  `make test` is the three gtest `[  PASSED  ]` lines; the swarm netns step needs the `veth` module
  (AGENTS.md) and fails after them without it.

## 3. Ordering questions

### A. Verification percepts and the journal (I3-4) against I4's query tokens

Spec 6.3 requires a verifier's ground values to match a query token. Draft 1 recommended option 2
(I3-4 after the token slice). All three reviewers agree, on two conditions that this draft adopts.

| Option                                                   | Gain                          | Cost                                                                                                    |
| -------------------------------------------------------- | ----------------------------- | ------------------------------------------------------------------------------------------------------- |
| 1. I3-4 first, matching observation IDs only             | transport and journal earlier | a weaker check ships and is tightened later; no query exists to produce the observations being verified |
| 2. I3-4 after the token slice                            | one check, tested end to end  | I4's first half cannot show the loop                                                                    |
| 3. Split: grants and journal first, the token match last | parallel to I4                | the same stub as option 1 for the part that matters                                                     |

**Option 2, narrowed.** Architect M4 shows that two things were called "token": a ranked
continuation (lifetime: the handler activation) and a verification token (lifetime: until the
verification horizon). Only the second gates I3-4a, so I4-7 splits: I4-7a is the continuation and
`ranked`, I4-7b is the commit-point verification token, and I3-4a depends on I4-7b alone. The second
condition (distributed): the message format of I3-3a already carries the verification kind, and V-D
and I4-D2 are drafted together so the token and the journal record are one interface.

### B. Where Beta label updates land

With only priors every source has the same weight, and a pooled score does not depend on the
supporter count. Draft 1 chose option 3 (labels at I5-1) with a test fixture that wrote alpha and
beta, and accepted I4's first half provisionally. The ML review (M1) shows both parts fail: the
fixture is the second write path draft 1 rejected, and the README rule has no provisional
acceptance.

| Option                                        | Gain                                                 | Cost                                                                          |
| --------------------------------------------- | ---------------------------------------------------- | ----------------------------------------------------------------------------- |
| 1. In I3-4 (labels applied on processing)     | the loop closes with the transport                   | mixes authorization review with learning review; I3-4c exceeds its size       |
| 2. In I4, with a test-only injector           | I4 shows reputation                                  | a second write path to learned state                                          |
| 3. I5-1 after I3-4c, fixtures write cells     | one production path                                  | five ML-gated slices are reviewed where reputation cannot move                |
| **3'. Split I5-1: apply routine early** (new) | one writer from the start; no provisional acceptance | I5-1a is built before the transport that will call it, so a harness drives it |

**Option 3'.** I5-1a lands right after I4-2: the ledger entry, the single routine `ledgerApply`
(append, or replace by named predecessor; recompute Beta from the epoch prior plus the current-epoch
ledger entries in first-accept order; bump the state version; refuse `EpistemicBudget` at the cap),
driven by a test harness exactly as I3-2c2 drives the health turn. It is the only writer of alpha
and beta after creation (I4-1b writes them once, at creation, and a reset reaches them through this
routine's recompute from I5-1a on); fixtures call it. I3-4c wires the processing turn to it and
I5-1b adds the cases that need the journal. Reputation separation is claimed from I4-6; the whole
loop (observe, query, verify, label, re-query) from I5-1b, and re-reviewed at I4-11. Claims that
hold with priors alone are eligibility, freshness decay toward 0.5 under an asymmetric prior, tie
abstention, refusals and enumeration; the user guide does not show "most reliable source wins"
before labels exist, and says that at an asymmetric prior with no labels `truest` selects by age.

### C. Calibration records and dependence groups

Calibration records stay in I4 and dependence groups stay before `truest` (I4-4 before I4-6): with
the default single group and rho = 1, `n_eff` is 1, so the pooled score does not depend on how many
sources support a candidate (ML computed 0.700000 for 1, 2, 5 and 20 sources of weight 0.7, and
0.985748 for five independent groups). Draft 1 "froze the whole record" at I4-1. Two reviews show
that is wrong: the record cannot be frozen before I5-D (weights and normalizer lengths follow the
feature count), and draft 1 keyed it without the source. I4-D2 freezes the **envelope**: two record
kinds with their keys and parents (a per-source calibration record, a per-domain policy record that
owns the group registry), the ledger entry's field list, and one closed policy key table covering
every key of 3.1 to 3.4 and 6.3, added once. SGD internals arrive as additive fields that read as
their initial value when absent (the shape-version rule), so the translator needs no change for
them. I4-1 splits into I4-1a (policy encoding and the domain record) and I4-1b (calibration).

### D. The translator against I5 and I6

Option 3 (spine early, parity rule, completion late) stands, with the conditions of the architect
and distributed reviews: T0 shares the upgrade guards across both entry points and leaves the two
state copiers alone (lead ruling); **T1-D**, a design slice, settles whether and how the copiers
collapse; the parity rule is in every slice's file list rather than in T2; T2 shrinks to typed
migrations, the full-bound lifecycle and the audit; and each iteration ends with the coverage
script.

### E. What "complete" means for live upgrade

Lead ruling O1(b), within the spec, plus **proposed amendment A5** (architect plan review M2): P19
state survives live upgrade and resume of a supported agent (no active intention, no native skill
state); an agent with an active intention refuses with a typed, tested error (introduced at T1 on
the refusal channel, not T2). Spec 8.2 gate 0 says to complete the independent full-vagent upgrade
prerequisite before claiming P19 upgrade support, so this claim departs from the gate's wording
until the owner ratifies A5 ("P19 upgrade support is claimed for agents without an active intention,
by quiescent upgrade only; full vagent upgrade support remains owed to P17"). F1 writes A5 into spec
v1.6 as pending. The final record does not claim full vagent upgrade support. The claim also carries
the quiescence precondition (section 4.5, T1) and F2 words it so (section 8).

## 4. Order, dependencies and slices

### 4.1 Order at a glance

```text
E0 ─ T0 (done) ─ E1 ─ I3-2a ─ I3-2b ─ I3-2c1 ─ I3-2c2 ─ T1 ─ I3-3a ─ I3-3b ─ I3-3c ─ I3-3d
   ─ I4-1a ─ I4-1b ─ I4-2 ─ I5-1a ─ I4-3 ─ I4-4 ─ I4-5 ─ I4-6 ─ I4-7a ─ I4-7b ─ W1
   ─ I3-4a ─ I3-4b1 ─ I3-4b2 ─ I3-4c ─ I5-1b ─ W2
   ─ I4-8 ─ I4-9 ─ I4-10 ─ I4-11 ─ I5-2 ─ I5-3 ─ I5-4 ─ I5-5 ─ I6-1 ─ I6-2 ─ T2 ─ F1 ─ F2

designs, each reviewed to PASS before its slice starts:
  I3-2D (before I3-2a)   T1-D (before T1)        I3-3D (before I3-3a)
  I4-D1 + I4-D2 (before I4-1a; I4-D1 also before I4-2)
  V-D (before I3-4a; drafted with I4-D2)         I5-D (before I5-2)
  I6-D (before I6-1)
```

I5-D is needed before I5-2. The two points I5-1a needs from it (the receive order equals the
first-accept order, and the ledger entry's replay fields are additive) are settled in I4-D2, as is
the shape of the abstention criterion (ML minor 7); I5-D fixes its numbers.

**What runs in parallel.** Implementation slices are strictly sequential: they share `compiler.cpp`,
`agent_executor.cpp`, `runtime_abi.hpp`, the translator and the test fixtures, and no two are in
flight at once. Documents are not bound by that, and a design is drafted and reviewed while the
previous implementation slice is in review, provided its inputs are accepted:

| Start while this is in review | Design that can be drafted                                  | Inputs it needs first                                          |
| ----------------------------- | ----------------------------------------------------------- | -------------------------------------------------------------- |
| now (E1 in progress)          | I3-2D (accept revision 3); I4-D1, the numerics half; S0     | none; I4-D1 reads only the spec and the accepted I1 record     |
| I3-2a to I3-2c2               | T1-D; I3-3D; I4-D2 (structures, ABI, tokens, caps)          | I3-2D accepted for the health states and the T0 guards         |
| I3-3a to I4-2                 | V-D (drafted with I4-D2, cross-reviewed); I5-D (first part) | I4-D1 and I4-D2 accepted; I3-3D accepted for the message union |
| I4-4 to I3-4c                 | I5-D (rest); I6-D                                           | I5-1a accepted for the ledger; I4-D1 for the SGD reference     |

E0 is documents and can run beside anything. E1 is a pure move and was never merged with T0 (a mixed
diff defeats the moved-line check).

Hard dependencies that fix the order:

- T0 before E1 (the large files are quiet); E1 before any slice that edits ingestion or health IR.
- T1 (the refusal channel, the shape version and its guard function) before I4-1a, whose tag
  mismatch uses the channel. I4-1b (the epistemic-state version and `rtBumpStateVersion`) before
  I5-1a, I4-7a (`StaleEpistemicQuery`) and I6-1.
- I3-2 (`rtHealth`, grants, `healthEntry`) before I4-6 (eligibility recomputes the state at query
  time) and before I4-10 (directives lower onto `healthEntry`). I3-2c1 before I3-2c2 (the stub turn
  before the real one).
- I3-3b (clock-mapping snapshots) before I4-6 (query-time age and alignment read the snapshot).
- I3-3a (signed ingress, message union) before I3-3d, before W1, and before I3-4a (a verification
  percept is authenticated input). Any claim that 2.1-45 holds outside tests waits for W1.
- I4-D1 and I4-D2 before I4-1a. I4-1a before I4-1b. I4-1b and I4-2 before I5-1a. I4-D2 before
  I4-1b's reset rule (P2-4) and before any cap (P2-5).
- I5-1a before I4-3 only by position (the same reviewers read both); I5-1a before I4-6 (reputation
  separation is tested through the apply routine) and before I3-4c (the processing turn calls it).
- I4-5 (syntax) before I4-6 (lowering). I4-6 before I4-7a. I4-7b (commit-point token) before I3-4a
  (the token match) and before I3-4b1 (journal pins).
- I3-4a before I3-4b1 before I3-4b2 before I3-4c. I3-3c (the timer wake) before I3-4c (backoff).
- I3-4c before I5-1b. I5-1b before W2.
- I4-8 (verdicts, reference) before I4-9 (consensus needs `ACCEPTED` and a reference).
- I5-3 (features) before I5-4 (SGD) before I5-5 (drift epochs). I5-2 before I5-3.
- I6-1 after I5-5 (completions are keyed by model epoch).
- T2 after I6-2 (it translates the last structures); F1 after T2; F2 last.

The I4-4 position is kept before I4-6 (ML M1, sound item 1). The reviewers may move I4-4 after I4-9
only if they accept that `truest` and `ranked` then ship a pooled score that ignores supporter
count.

### 4.2 Module boundary after E1

Draft 1 sent all growth to one file named for the feature. The boundary is now by area. Every area
gets its own emitter file, its own ABI header and its own tests, and no file in the table passes
about 3,000 lines. `compiler.cpp` (7,617 lines), `agent_executor.cpp` (5,694), `runtime_abi.hpp`
(887) and `vortex_stdlib.cpp` (5,676) take only registration, dispatch and `#include` lines, at most
400 lines per slice. This is the lead's minimal E1: no existing body moves.

| Area                      | Files (under `vagent/runtime/` unless stated)                                                                             | Slices that write them                     |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------ |
| Code generator seam       | `compiler_gen.hpp` (namespace `codegen`; no anonymous namespace in a header)                                              | E1 only                                    |
| Health                    | `epistemic_emit_health.cpp`, `epistemic_abi_health.hpp`, `agent_executor_health.cpp`                                      | I3-2a, I3-2b, I3-2c1, I3-2c2, I3-3c, I3-3d |
| Clock and liveness        | `epistemic_emit_clock.cpp`, `agent_executor_clock.cpp`                                                                    | I3-3b, I3-3c                               |
| Signed ingress            | `epistemic_adapter.{hpp,cpp}`, `epistemic_adapter_wire.cpp` (canonical bytes)                                             | I3-3a                                      |
| Policy and calibration    | `epistemic_policy.{hpp,cpp}`, `epistemic_emit_calibration.cpp`, `epistemic_abi_calibration.hpp`                           | I4-1a, I4-1b, I4-4, I5-3                   |
| Label ledger              | `epistemic_emit_ledger.cpp`, `epistemic_abi_ledger.hpp`                                                                   | I5-1a, I5-1b                               |
| Numerics (`vortex/`)      | `vortex_stdlib_epistemic_score.cpp`, `_pool.cpp`, `_gate.cpp`, `_fusion.cpp`, `_sgd.cpp`, declared in `vortex_stdlib.hpp` | I4-2, I4-3, I4-8, I4-9, I5-4               |
| Query                     | `epistemic_emit_query.cpp`, `epistemic_abi_query.hpp`                                                                     | I4-6, I4-7a, I4-7b                         |
| Gates and fusion emission | `epistemic_emit_gates.cpp`, `epistemic_emit_fusion.cpp`                                                                   | I4-8, I4-9                                 |
| Verification              | `epistemic_emit_verify.cpp`, `epistemic_abi_verify.hpp`, `agent_executor_verify.cpp`                                      | I3-4a, I3-4b1, I3-4b2, I3-4c               |
| Learning and defeaters    | `epistemic_emit_learning.cpp`, `epistemic_emit_defeaters.cpp`                                                             | I5-2 to I5-5                               |
| Native models             | `epistemic_emit_native.cpp`, `agent_executor_native.cpp`                                                                  | I6-1, I6-2                                 |
| Surface language          | `parser_epistemic.cpp`, `checker_epistemic.cpp` (dispatch lines only in `parser.cpp`, `checker.cpp`)                      | I4-5, I3-4a, I4-10, I5-3                   |
| Translator                | `epistemic_translate.cpp` (spine, T1), `epistemic_translate_<area>.cpp` per area                                          | T1 and every slice that adds a structure   |
| Refusals                  | `epistemic_refusal.{hpp,cpp}` (the typed vocabulary and the pre-flight; name per T1-D)                                    | T1; cited by I4-1a, I3-4c, T2              |
| Operator surface          | `apps/vagent/epistemic_cli.cpp`, dispatch in `apps/vagent/main.cpp` (1,851 lines)                                         | W1, W2                                     |

`runtime_abi.hpp` keeps what it holds. New constants go in the per-area ABI header, and
`runtime_abi.hpp` gains one include line per area.

### 4.3 Totals

| Group                                             | Slices                                                                             | Count  | Rows closed | Estimated lines of code and tests  | ML gate  |
| ------------------------------------------------- | ---------------------------------------------------------------------------------- | ------ | ----------- | ---------------------------------- | -------- |
| Foundations (E0, T0, E1)                          | E0, T0, E1                                                                         | 3      | 0           | 900 (T0 as built; E0, E1 excluded) | 0 of 3   |
| I3-2 (source health)                              | I3-2a, I3-2b, I3-2c1, I3-2c2                                                       | 4      | 12          | 4,900                              | 2 of 4   |
| Translator (T1, T2)                               | T1, T2                                                                             | 2      | 23          | 2,500                              | 1 of 2   |
| I3-3 (ingress, clock, heartbeat, reports)         | I3-3a, I3-3b, I3-3c, I3-3d                                                         | 4      | 17          | 5,000                              | 2 of 4   |
| I4 (policy, scoring, queries, gates, fusion)      | I4-1a, I4-1b, I4-2, I4-3, I4-4, I4-5, I4-6, I4-7a, I4-7b, I4-8, I4-9, I4-10, I4-11 | 13     | 163         | 16,100                             | 12 of 13 |
| Verification loop (grants, journal, issuer, turn) | I3-4a, I3-4b1, I3-4b2, I3-4c                                                       | 4      | 53          | 5,400                              | 3 of 4   |
| I5 (ledger, defeaters, features, SGD, drift)      | I5-1a, I5-1b, I5-2, I5-3, I5-4, I5-5                                               | 6      | 60          | 8,100                              | 6 of 6   |
| I6 (native models)                                | I6-1, I6-2                                                                         | 2      | 16          | 2,400                              | 2 of 2   |
| Production path (W1, W2)                          | W1, W2                                                                             | 2      | 1           | 1,600                              | 0 of 2   |
| Final (F1, F2)                                    | F1, F2                                                                             | 2      | 9           | 1,000                              | 2 of 2   |
| **Total**                                         |                                                                                    | **42** | **354**     | **47,800**                         |          |

Estimates are judgement, anchored on the accepted I3-1 slices (135 to 1,700 lines of code and tests,
median about 500) and I2U (2,246). A slice that grows past about 1,500 lines in review splits. Draft
1 estimated 40,000 lines in 36 slices; the extra 7,300 are the translator parity code and tests that
each slice now carries (about 200 lines in each of 15 slices), the split slices, the production path
and the reference data. "Rows closed" counts open matrix rows whose last slice is this one. The
matrix has 354 open rows (111 Partial, 231 Missing, 12 Designed) and this column sums to 354; draft
1's group totals summed to 357, an arithmetic slip, since its per-slice counts also summed to 354
(checked by script when this draft was written; F1's `tools/check-p19-traceability.py` keeps it
true). Done, Deferred, Standing and Informational rows need no slice.

### 4.4 Design slices

Each design is a document in this folder, reviewed to PASS before its implementation starts, as I3-1
was (five rounds) and I3-2 now is (two rounds failed, the second with 47 MAJOR findings). A design
that fails three rounds returns to its author with a narrower scope.

| Design | Covers                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | Must settle first                                                                                                                                                                                                                  | Reviewers                            |
| ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------ |
| I3-2D  | revision 3 of `i3-2-design.md` with the round-3 rulings R3-1 to R3-11: the 47 MAJOR findings of round 2 and decisions F1 to F33; the order contract for control posts against queued observations (R3-6, R3-7); what a resume loses of host state. It writes **no** shape-version or state-version cell (R3-11) and records that every health change, supersession and policy change must bump the epistemic-state version once I4-1b creates it, with the list of writers. It aligns its three gate-rejection sentences (`i3-2-design.md:195`, `:975`, `:1111`) with O10, so I3-2 does not pre-commit a streak (ML minor 2), and it emits the estimator routine parameterised by its two state cells, decay factor and evidence floor so I4-8 reuses it (ML minor 5)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | reason origin, reset epoch kept in the arena, health settings channel into the query turn, the precedence of a reset against readings of its own source (the in-flight snapshot decides; control-first applies to everything else) | ML, ARCH, DIST, EPI, SENS, SEC, CODE |
| T1-D   | the translator and the refusals: **the refusal channel** (P2-1: an executor-level `EpistemicRefusal` with `toString` and the offending numbers, returned by a checked pre-flight that `resume` runs before `runtime_.resume`, with the policy hashes, tag and shape version readable from the checkpoint; the files and callers it touches: `agent_executor.hpp`, `program.hpp`, `cluster_node.cpp`, `agent_tui.cpp`); **the shape version and the guard** (R3-11: minted only for an agent whose program declares an arbitrated predicate; one function `hasTranslatableEpistemicState` that ignores the header and is conditional on a translator for that shape version; header compared by number, never copied); the shared routine's signature, the deliberate resume and upgrade differences, the digest (doubles by bit pattern), whether the two copiers collapse and, if not, that `LocalHost::upgrade(pid, CompiledProgram)` forwards or installs the translator; the quiescence rule and the operator path to quiesce (pause admission for a pid, drain, upgrade, resume); admission closed during the swap; the mesh mailbox as a third queue (intake handling program-independent, or the mailbox joins the refusal list); which host state an in-process upgrade keeps and resume loses; D3 (a policy change rebinds, refuses or migrates source bindings); the coverage oracle (each per-area ABI header exports its owned dimensions and record kinds, each translator test registers the ones it covers, the script diffs both lists and greps `ensureDimension` names against the first)                                                                                                               | the two copiers (`restoreState`; `makeStateReconciler` and the translator seam), the rule for a post racing the swap, the refusal list and where each check runs                                                                   | ARCH, DIST, CODE, EPI, SEC           |
| I3-3D  | the signed-observation message (tagged union, canonical bytes, audience, domain separation, relay marking, certificate validity, reuse of `DeviceDelegation`), **the signed header common to every kind** (issuer, incarnation, sequence, low-watermark, audience, expiry; which kind uses which field), clock-mapping snapshots, heartbeats, sensor reports; the carrier of W1's alias-to-principal table (a system xanadoc dimension, or a file path named by a setting, since a setting is a flat typed tuple); what carries `relayedBy` in the CLI (an in-process loopback peer, or "none, relay unsupported in W1")                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  | O4 (ruled); the classification of time refusals as device or path faults; what a gateway can drop, delay, duplicate, reorder                                                                                                       | SENS, DIST, SEC, ARCH, ML            |
| I4-D1  | the numeric half: reference program and committed vectors, their file format (inputs as hex-float bit patterns, outputs as 50-digit decimal strings, the allowed relative error per routine from its operation count, no `math` or `numpy` function for `exp`, `log` or `sqrt`, checked by a grep test); the cross-host contract (D1); `lcb` at an uninformative prior (O9); `score_tolerance` space; the gate-rejection rule (O10) with **the accepted false-latch rate per device-year at a stated event rate, proved by a bound** (the event-indexed fraction at H = 32 has a weighted Chernoff bound per event of 8.7e-9 at threshold 0.25 and p = 0.05; simulation is a smoke test of the implementation against it, since zero latches in n events only bounds the rate by about 3/n); per-source `decay` meaning; the profile-variance question (O6). The label and ledger caps and cell prices are **not** here (P2-5)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | the cross-host meaning of "deterministic" (ruled, D1); O9; O10; the false-latch rate, proposed here and confirmed by the lead                                                                                                      | ML, NUM, EPI, SENS                   |
| I4-D2  | the structural half: two record kinds and keys; the ledger entry fields; **the label and ledger cap** (P2-5): the cap per (source, domain, epoch), its default, the cell price of one entry computed from its field list and confirmed by building one in a throwaway fixture (no actor IR needed), the minimum labels per source the default must hold (with 24,576 cells, 128 sources and an estimated 40 to 60 cells per entry the ceiling holds a few hundred labels in total; the design states the number it accepts and what the operator does at the cap), and the interaction with the checkpoint limit of 65,536 value operations; **the reset rule** (P2-4): a reset opens a new calibration epoch in O(1), the recompute reads only current-epoch entries, older entries leave the cap (excised in bounded steps or counted separately), a pending label for the old epoch gets `ExpiredLabel`; the closed policy key table, with the shape of the abstention criterion (prequential loss against the Beta baseline, a minimum sample, one margin; I5-D fixes the numbers); the `std:` mechanism, its bootstrap cost (O2) and where list-valued loops live; the query frame ABI; the continuation and the commit-point token (O3), whether a verifier-requested token has a message (else the clause is dropped), the retention unit (turns or state-version steps, never wall time), the rule that eviction never precedes expiry and that minting at the cap refuses with a typed error, the pending-record life setting and the check `retention >= pending life + retransmit bound` at settings load; eligibility; refusal vocabulary (cites T1's); budgets for every structure (O7); the S0 spike result | O2 mechanism, O3 trigger, the C++ justification, the reset rule and the cap (so I4-1b and I5-1a can be briefed)                                                                                                                    | ARCH, CODE, VM, DIST, EPI, ML        |
| V-D    | the verification loop: grants, **the replay floor as an issuer-declared low-watermark** (P2-2: each signed verification carries the lowest sequence the issuer will still retransmit; the receiver caches an outcome for every sequence at or above it and refuses below with `VerificationTooOld`; it raises the stored value only to a signed one and never infers it; the window is bounded by a setting and a cell price per entry; when full the refusal is the retryable `VerificationWindowFull` and nothing is evicted; the issuer incarnation is monotone, an issuer restored from a checkpoint takes a fresh one (A3 for issuers), and a reused (incarnation, sequence) with another payload is `DuplicateVerificationPayload`; floor and journal state exist only after intake commits), the idempotency key, the intake decision order, host-state classes, **stable agent identity**, the issuer side, the processing write set, label semantics for competing values, the mesh receipt audit (8.P5-2), **two fault models** (P2-3: an in-turn abort by fault injection at each statement boundary of the write set, which rolls back with the enclosing mark; a restart from the last checkpoint after intake, after processing and after the response was sent) with the expected state of the actor and the issuer and the issuer's next retransmit per crash point and per D2 option, and for upgrade the rows "refused as not quiescent" and "state carried"; a one-page message and crash-point table                                                                                                                                                                                                  | the floor's order, retry and backoff, the fault models; D2 is ruled (section 7.2)                                                                                                                                                  | DIST, SEC, EPI, ML, ARCH             |
| I5-D   | defeaters, features and normalizer, SGD and replay, drift epochs; receive order equals first-accept order and what a revision does to it; the abstention criterion (held-out or prequential loss against the Beta baseline, minimum sample); reference vectors for SGD                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | determinism across hosts, replay horizon, abstention rule                                                                                                                                                                          | ML, NUM, EPI, DIST                   |
| I6-D   | native model effects, keyed completions, cancellation                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | model-epoch keys, capacity and latency bounds, sandbox                                                                                                                                                                             | ML, DIST, SEC, ARCH                  |

**S0, the spike.** Before I4-D2 goes to review, one throwaway change emits a single trivial
`ScopedEvaluate` from the actor compiler and runs it headless. It is kept as a patch and a log under
the design's evidence folder, never merged, so the query frame ABI is judged against something that
runs (architect minor).

### 4.5 Implementation slices

#### E0. Record the rulings and refresh the records

*Foundations. Size: about 150 lines of Markdown, no code.*

- **Content.** [decisions.md](decisions.md) is the record of the lead's rulings and of the proposed
  spec amendments (section 3 there); the spec itself is not edited until F1, which folds the
  amendments into v1.6, marked pending until the owner ratifies them. E0 updates the README
  iteration table to the order in this plan, records the 34 foreign commits and the code baseline,
  lists `decisions.md` and `plan-review/` in the README, and refreshes stale citations in the matrix
  notes (the 6.3 receipt lines now at `agent_executor.cpp:1715` and `:1755`; the intention refusal
  at `:5081` in the version before T0 and `:5123` after it; the X-1, X-20 and X-27 rows marked
  closed by T0). Each design that raises a further amendment appends a row to section 3 of
  `decisions.md`.
- **Closes.** No rows; it enables the slices after it.
- **Files.** `README.md` of the project folder; `traceability.md` notes.
- **Depends on.** None.
- **C++ and Vortex.** None.
- **Tests.** `make format-check` and `make lint` on the changed Markdown.
- **Gate.** ARCH, EPI read it against the code.

#### T0. Share the upgrade guards across both entry points, and the control-post wakeup

*Foundations. **Done**: committed as `4a912c69`. Size as built: about 900 lines of code and tests.*

- **Content, as built.** One private guard, `AgentExecutor::upgradeRefusal(pid, old, route)`, is
  called by both `upgrade` overloads before anything mutates. It refuses `NotQuiescent` for a turn
  in flight, an un-drained host event, any post-box item for the pid (a queued source change
  included), a non-zero choice depth or an outstanding mark, and, on the schema route only, for
  flights, requests, outstanding deliveries and a staged outbox or pending entry. It refuses
  `ExecutionFailed` for a populated `d.trust` or `d.epistemic` root unless the route is the schema
  route and a translator is registered; the reconciler route refuses it whether or not one is.
  `PostBox::pushControl` wakes the reactor as `push` does. `putBack` stays silent, because a signal
  there spun the reactor, and posts kept for an actor that stopped before its first turn are
  abandoned with their accounting. The review (DIST, CODE) failed round 1 and passed round 2.
- **What moved out.** Draft 2 listed four more items under T0 that the commit does not hold. A typed
  refusal for a policy-hash mismatch on resume (X-9) and for a populated intention rank (still
  `ExecutionFailed` at `agent_executor.cpp:5123`) moved to **T1**, because neither has a vehicle:
  `ActorError` is a closed enum without a payload, and the resume check runs on the new actor's
  first turn, after `resume` has already returned a pid (architect M1, P2-1).
  `AgentSettings::fromModel` and the "Bayesian certainty" label (X-7, X-2) moved to **I3-2a**, which
  owns the settings loader; decision F30 keeps the whole-block fallback with a WARN naming the
  field, so X-7 reduces to giving `checkedEpistemicLimits` its production caller there.
- **Closes.** No rows; it enabled the slices after it. X-1, X-20 (the guard half) and X-27 are
  closed in the drift register.
- **Files.** `agent_executor.{hpp,cpp}`; `tests/xuzz/vagent_epistemic_ingress_test.cpp`.
- **C++ and Vortex.** All C++: upgrade orchestration and the post queue are host boundaries.
- **Tests.** Seven new tests: every enqueue path makes the wake descriptor readable; a put-back does
  not; a stopped actor's unplaced posts are abandoned; a populated profile refuses on every upgrade
  entry point with the context pointer, program bases and latch unchanged; a queued source change is
  `NotQuiescent`; an empty epistemic root still upgrades both ways; the reconciler refuses a
  populated profile even with a translator registered.
- **Left for T1.** The guard's root check (`linked(trust) || linked(epistemic)`) is replaced by
  `hasTranslatableEpistemicState` (R3-11), and by-value `LocalHost::upgrade` and
  `LocalHost::runtime()` are still reachable without the guard (T0 codebase review; T1-D decides).
  Source bindings keep the policy they were bound under after an upgrade (D3, T1-D).
- **Gate.** ARCH, DIST, CODE. Passed.

#### E1. Move the code generator's declaration to a private header

*Foundations. Size: about 1,200 lines moved, about 80 changed.*

- **Status.** In progress in an isolated worktree, as specified here. The architect's round-2
  details (minor 1) are recorded, not applied to this run: "no out-of-class body moves" is the
  accurate wording (the class body holds inline members such as `mkOp`, `store` and `mov` at
  `compiler.cpp:568-770`, which move with it); non-template free functions such as `tyOf` and
  `collectCallees` need `inline` in a header (the brief says so); a `struct` with a per-area state
  slot would save a header edit per area but changes the moved code, so the first emitter slice that
  needs a member made public or a state slot edits the header by a few lines, and that edit is
  allowed under the file-growth rule.
- **Content.** Move the helper types, constants and `Bail` (`compiler.cpp:92-413`) and the
  declaration of `class Gen` (`:414-1278`) to the private header `vagent/runtime/compiler_gen.hpp`,
  in the named namespace `xanadu::vagent::runtime::codegen`, since a header cannot export from an
  anonymous namespace. Grep for symbol collisions first. The members that emitters use become
  public; nothing else changes. The out-of-class member definitions stay in `compiler.cpp`, which
  now defines them in `codegen`. No emitter body moves: the lead's ruling is that new epistemic code
  lives in new files, not that old code relocates. Proof: the emitted IR of every program in the
  test corpus is byte-equal before and after (a throwaway harness, not committed); the three suites
  are unchanged; `git diff --color-moved` shows no edit inside moved blocks.
- **Closes.** No rows; it enables the slices after it.
- **Files.** `compiler.cpp`, new `compiler_gen.hpp`. The Makefile finds sources by `find` and needs
  no edit.
- **Depends on.** T0 (done), so the large files are quiet; the lead rebases immediately before and
  after.
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
  evidence floor must not let one refusal in a fresh profile read SUSPECT. The update routine is
  emitted parameterised by its two state cells, the decay factor and the evidence floor, not
  hard-wired to `health_refused_weight` and `health_weight`, so I4-8 reuses it for gate rejections
  without rewriting accepted code (ML minor 5). The settings loader keeps the whole-block fallback
  with a WARN naming the field (F30) and gives `checkedEpistemicLimits` its production caller (X-7);
  the TUI and user-guide "Bayesian certainty" figure is relabelled acquisition uncertainty (X-2,
  moved from T0). No streak threshold is implemented (O10 leaves the gate rule to I4-D1).
- **Closes.** 2.1-42, 5.1-4, 5.1-8, 8.2-R1c (4).
- **Files.** `epistemic_emit_health.cpp` and `epistemic_abi_health.hpp` (new); `compiler.cpp`
  (charge call sites, at most 400 lines); `epistemic_limits.hpp`; `system_docs.{hpp,cpp}`;
  `agent_executor.cpp` (`fromModel`, at most 60 lines); `vagent/tui/agent_tui.cpp`;
  `docs/vagent-user-guide.md`; `agent_executor_health.cpp` (charge coalescing);
  `tests/xuzz/vagent_epistemic_health_test.cpp` (new), `vagent_epistemic_noise_test.cpp`,
  `vagent_settings_test.cpp`, `system_docs_test.cpp`.
- **Depends on.** I3-2D accepted.
- **C++ and Vortex.** The estimator and the state routine are emitted IR. C++ is the IR emission,
  the settings and the existing charge queue.
- **Tests.** The estimator against a closed form (k refusals, m accepts, a coalesced batch); bounds
  after 10^6 updates; threshold and minimum-event edges; stale and time-mismatch refusals and
  relayed copies; an invalid setting falls back as a block and logs the field; a 1,000-reading
  flush; the noisy-sensor simulation at 0.1, 1 and 100 Hz; resume keeps R and N; a tombstone and a
  restored profile have a state; a second source is not refreshed (R1c). The recovery from
  saturation is tested as "about 64 events" over a band, never at the boundary: with `H = 32` and a
  threshold of 2^-2 the recovery event is 64 with the host's `exp`, 64 with one ulp less and 65 with
  one ulp more (ML plan review M5).
- **Gate.** ML (design and implementation), SENS, CODE, NUM.

#### I3-2b. Settle after commit

*I3-2. Size: about 800 lines.*

- **Content.** Source changes (and, later, health actions) commit first; their belief events run in
  one settle turn per pending event (decision F9); a failing event is discarded alone, and the
  outcome carries the settled and discarded counts and the bounded names of discarded predicates.
  Fixes the `-pred` handler wedge. The design records the delivery semantics of a discard (round 2,
  DIST M1): which events are dead-lettered and where the count is visible.
- **Closes.** No rows; it enables the slices after it.
- **Files.** `epistemic_emit_health.cpp` (`settleEntry`, `discardPendingEntry`);
  `agent_executor.cpp` (scheduling, at most 400 lines); `program.cpp` (remap of both entries);
  `tests/xuzz/vagent_epistemic_ingress_test.cpp`, `vagent_upgrade_test.cpp`.
- **Depends on.** I3-2a.
- **C++ and Vortex.** Scheduling the extra turn is executor C++ (turns are a host concern). The
  entries are IR.
- **Tests.** An always-failing `-pred` handler: advance and retire commit, the outcome reads
  `SettleFailed`, the next ingestion commits; a correct handler sees each event once; the settle
  turn runs before a queued observation; an upgrade where either entry is absent fails a test rather
  than skipping the settle silently.
- **Gate.** ARCH, DIST, CODE, VM.

#### I3-2c1. Health grants, the host API, the post order and a stub turn

*I3-2. Size: about 1,400 lines. The numbering is that of `i3-2-design.md` revision 3 (2c1 is the
host side, 2c2 the Vortex turn); draft 2 had the two the other way round.*

- **Content.** `EpistemicHealthGrant`, a 128-bit token the host keeps, `healthAction`, the receipt
  and the bounded outcome log, queue quotas per grant, idempotent receipts (F17), the rule that the
  issuer is never a bound source on any actor of the executor (F14), rechecked when the action runs
  and not only at grant time. State the trust boundary: who may call `grantHealth`, and where the
  random source comes from (the design had cited one that does not exist). `healthEntry` is a stub
  that answers `HealthUnavailable`, so the authorization surface is reviewed before any state can
  change. **The post order** is the design's (R3-6, R3-7): control items have strict priority, the
  control cap yields only to other actors and counts every turn of a change before it starts; the
  reset floor comes from a bounded per-(principal, predicate) multiset of readings in flight that
  the reset call snapshots, so `wouldChange(Reset)` is true while any reading is in flight and
  "eligible only after a new reading" cannot be satisfied by a reading that predates the repair. The
  precedence is stated once (distributed minor 2): for a reset against readings of its own source
  the snapshot decides, and "control items first" applies to everything else, `putBack` included.
  The slice also states what a resume loses (bindings, aliases, floors in `Ingress`, the outcome
  log, coalesced charges), which of these an in-process upgrade keeps (the pid survives it, so
  `Ingress` state keyed by pid does), and tests both, including that a replay after resume is still
  refused from the profile's last sequence and incarnation in the arena (matrix 1-10). Nothing here
  writes a version cell (R3-11).
- **Closes.** 4-18, 8.2-4c (2).
- **Files.** `agent_executor.hpp`, `agent_executor_health.cpp`, `health_grants.{hpp,cpp}` (new, as
  in the design); `epistemic_emit_health.cpp` (the stub);
  `tests/xuzz/vagent_epistemic_grants_test.cpp` (new); `tests/fuzz/fuzz_epistemic_ingress.cpp`.
- **Depends on.** I3-2b.
- **C++ and Vortex.** All host C++: authorization, clocks (the wall stamp), the transport queue.
  None of it is learned state; the arena stays authoritative for the latch (X-14).
- **Tests.** Each refused grant and request; a token revoked while queued; an issuer that is a bound
  source or a relay; an action on a source the grant does not name; quota exhaustion cannot keep a
  safety action out; fuzzer steps for each action under a valid, a revoked and a wrong-kind grant; a
  refused action changes nothing; the stub answers `HealthUnavailable` and changes nothing; a reset
  between two queued readings (the snapshot decides), an isolate behind a held item, `putBack` keeps
  control items first; a resume then a replay; an in-process upgrade keeps the grants and ordering
  state the design says it keeps.
- **Gate.** SEC, DIST, ARCH, CODE.

#### I3-2c2. Health records and the Vortex health turn

*I3-2. Size: about 1,500 lines; splits at the reset routine if it runs over.*

- **Content.** `rtSourceDrop` as one routine shared by advance, retire, isolate and reset; the
  `fault`, `reasons` (with an origin) and `actions` structures; `healthEntry` replacing the 2c1 stub
  (isolate, degrade, clear, reset); an arena-side reset epoch and `retired_by`, so a reset can
  expire labels later. Reasons carry an origin so I3-3d can clear only what a device added. A reset
  keeps the audit list. The arena half of the order contract: the reset epoch and `retired_by` are
  durable; the in-flight snapshot that decides which queued reading predates a reset is host state
  and lives in 2c1. **No shape-version cell and no state-version cell is written here** (R3-11,
  which moved the first to T1 and the second to I4-1b). The slice instead lists, in the design and
  in the code comment at each site, every writer that must bump the epistemic-state version once it
  exists (isolate, degrade, clear, reset, an advance or retire that withdraws supports, a change of
  derived state), so I4-1b finds them all; the rule is a bump on a change of derived state and on
  any action, never on `R` or `N` drift (ML minor 4).
- **Closes.** 5.1-7, 5.1-11, 5.1-12, 5.1-14, 4-20, 8.2-4g (6).
- **Files.** `epistemic_emit_health.cpp`, `epistemic_abi_health.hpp`; `epistemic_limits.hpp`,
  `system_docs.*` (reason and action bounds); `tests/xuzz/vagent_epistemic_health_test.cpp`,
  `vagent_epistemic_resume_test.cpp`.
- **Depends on.** I3-2c1.
- **C++ and Vortex.** IR only; the host side was built and reviewed in 2c1.
- **Tests.** Isolate before the first reading sticks and drops supports; latch, reasons and actions
  survive an advance, a retire and revive, and a checkpoint; degrade unique by (kind, code, origin);
  clear removes only operator-added degraded reasons and never unlatches; reset clears latch,
  reasons, R, N and streak, keeps dedup and actions, and the source is eligible only after a new
  reading; reset with a failing handler commits; a reset of 16 records measured against turn fuel (a
  16-record reset within half the grant); a sensor cannot clear its own latch without reset
  authority (4-20).
- **Gate.** ARCH, DIST, EPI, SENS, CODE, ML (named items: the reset epoch and `retired_by` that
  later expire labels).

#### T1. Translator spine, the refusal channel and the shape version

*Translator. Size: about 1,500 lines; splits at the refusal channel if it runs over. Its design,
T1-D, is a design slice (section 4.4).*

- **Content.** Three parts, in this order.
  1. **The refusal channel** (P2-1, architect M1). The vocabulary and the checked pre-flight that
     T1-D designs, run by `resume` before `runtime_.resume` and by both upgrade overloads before
     they mutate anything. It carries the typed reason and the offending numbers: a policy-hash
     mismatch on resume (X-9: today `restoreState` throws inside the new actor's first turn,
     `prepare` logs at DEBUG, and the caller holds a pid for an actor that dies), an unknown shape
     version, a populated state with no translator for its shape, and a populated intention rank
     (the O1(b) boundary, today `ExecutionFailed` at `agent_executor.cpp:5123`). Callers updated:
     `LocalHost`, `cluster_node`, the TUI.
  1. **The shape version and its guard** (R3-11, architect M3). One integer at the head of the
     agent's `d.epistemic` rank, minted at agent creation only when the program declares an
     arbitrated predicate. One function, `hasTranslatableEpistemicState`, replaces the root check in
     `upgradeRefusal`; it ignores the header cell and is conditional on a translator being
     registered for the shape version, so later slices change the function and not each call site.
     `restoreState` and the translator compare the saved header with the freshly minted one by
     number and never copy it.
  1. **The default `OwnedStateTranslator`** over the P19 roots final by then: profiles and what
     hangs under them (histories, the per-profile retired chain, whatever shape I4-1b later gives
     it), health records and the policy master. The routine is the one T1-D defines: its signature,
     the deliberate differences (resume clears `last_receive` and `last_contact` because the clock
     instance changed and refuses on a policy-hash change; upgrade keeps the clock and refuses on a
     schema diff) and a digest defined as equal modulo them, with doubles compared by bit pattern,
     so the property test "resume and upgrade produce the same digest" is stated correctly. T1-D
     decides whether `LocalHost::upgrade(pid, CompiledProgram)` forwards to one road or the
     translator is installed on both, so no public entry point accepts a state another refuses.
     **The quiescence rule** (distributed M5): upgrade requires no turn in flight, no un-drained
     host event (a pending settle or discard included), no staged unsent outbox, no unreplied send
     and no flight. It does not require an empty journal or queue: journal entries, queue entries
     with attempt counts, cached outcomes and floors are arena state, carried by the translator in
     the slice that creates them. A post that lands between the check and the swap is either
     program-independent by construction (T1-D lists every queued item kind, the mesh mailbox
     included) or admission for the pid is closed during the swap. T1-D names the operator path to
     quiesce, since an actor under steady ingestion is rarely quiescent. Peak copy (old plus new
     graph) is priced. Grants, the mirror and the in-flight snapshot are host state: an in-process
     upgrade keeps what T1-D says it keeps (the pid survives) and a resume loses them, and the user
     guide says which, so an operator is not told to re-grant after an upgrade that did not need it.
     `tools/check-translator-coverage.py` is written here, against the oracle T1-D defines.
- **Closes.** 1-12, 2.1-33, U-1, U-3, U-12, U-13, U-14, U-15, U-16, U-23, 8.2-R2f (11).
- **Files.** new `epistemic_translate.cpp`; new `epistemic_refusal.{hpp,cpp}` (name per T1-D);
  `agent_executor.{hpp,cpp}` (pre-flight, `restoreState`, `upgradeRefusal`, at most 400 lines);
  `program.{hpp,cpp}`; `cluster_node.cpp`; `vagent/tui/agent_tui.cpp`; `compiler.cpp` (mint the
  header, at most 100 lines); `upgrade_schema.*`; `tools/check-translator-coverage.py` (new);
  `tests/xuzz/vagent_epistemic_translate_test.cpp` (new), `vagent_epistemic_resume_test.cpp`,
  `vagent_upgrade_test.cpp`.
- **Depends on.** I3-2c2, T0, E1, T1-D.
- **C++ and Vortex.** Runs inside the host upgrade callback, which must be atomic with rollback; the
  copy itself is the existing owned-graph copier. The pre-flight is host admission.
- **Tests.** Each refusal asserts the typed value and the numbers, not a log line, and leaves no
  live pid from `resume`; a resume of a changed policy is refused before a pid exists (X-9). A
  populated actor upgrades with a changed handler and keeps every field, latch, reason and history;
  resume and upgrade produce the same state digest modulo the listed differences (property test over
  random ingestion sequences); a failing `on Upgrade` rolls back the translation; a changed policy
  refuses; the old actor, context and grant survive every refusal; a post racing the swap; an
  unknown shape version refuses by number; a populated intention rank refuses on every road. **A
  non-arbitrating agent upgrades unchanged after T1, and an arbitrating agent with no profile yet
  upgrades** (the header does not make an agent populated); the saved header is compared and not
  copied. A verification message in the mesh mailbox during an upgrade is handled as T1-D rules.
- **Gate.** ARCH, DIST, CODE, EPI, SEC.

#### I3-3a. Signed-observation ingress adapter

*I3-3. Size: about 1,300 lines.*

- **Content.** The first production caller of `bindSource` and `observe` (X-3). It reuses the
  existing `DeviceDelegation` and `StandardCryptoEngine` X.509 path; there is no second verifier.
  The signature travels inside the application payload, so `Envelope`, `ActorMessage` and the mesh
  wire are unchanged and the choice stays a lead decision. I3-3D fixes these properties, which are
  what makes authentication end to end: **audience binding** (the signed bytes name the receiving
  agent's stable identity and the predicate with its policy hash); **domain separation** (a
  versioned context string distinct from provenance, delegation and any other Ed25519 use);
  **canonical bytes** (fixed encodings for doubles by bit pattern with NaN and `-0` rejected or
  normalized by rule, for the tuple, strings and optionals); **relay marking from the transport
  only** (`relayedBy` is taken from the authenticated transport peer, never from a field a gateway
  can omit or a device can forge); **certificate validity** (at receipt, and for a delegated key at
  acquisition or by a stated rule, with revocation through the ledger); and the message is a
  **tagged union** from the start (observation, verification, later drift and reference) with the
  **signed header common to every kind** that I3-3D fixes (issuer, incarnation, sequence,
  low-watermark, audience, expiry), so I3-4a changes no accepted wire format. The design also states
  which of drop, delay, duplicate and reorder the incarnation and sequence floors, the time-mismatch
  refusal and the heartbeat catch.
- **Closes.** No rows; 2.1-45 is worked here and closes at W1.
- **Files.** new `epistemic_adapter.{hpp,cpp}`, `epistemic_adapter_wire.cpp`;
  `tests/xuzz/vagent_epistemic_adapter_test.cpp` (new); `tests/fuzz/fuzz_epistemic_ingress.cpp` (an
  adapter step in the existing target; a new target would add its `Makefile` lines).
- **Depends on.** I3-3D, T1.
- **C++ and Vortex.** All C++: cryptographic verification and key binding are the authorization
  boundary.
- **Tests.** A forged signature, a replayed envelope, a delegated key beyond its certificate, an
  alias bound to two principals, a relay claiming another origin, a string that merely looks like a
  principal, an observation replayed to a different actor or predicate, a signature from another
  domain, and non-canonical but equal inputs all refuse; the adapter feeds the existing ingestion
  tests end to end.
- **Gate.** SEC, DIST, ARCH, CODE.

#### I3-3b. Clock-mapping resolution and snapshots

*I3-3. Size: about 1,500 lines.*

- **Content.** An immutable mapping snapshot, addressable by version for a record's retained life;
  unknown mapping rejects; reboot re-establishes a mapping or expires observations; received-time
  basis recorded; the split between device lateness and path lateness, and the reclassification of
  `StaleObservation` and `ObservationTimeMismatch` as health refusals only where the split holds (ML
  round-2 minor). The snapshot is an owned structure, so the translator carries it in this slice.
- **Closes.** U-17, U-19, 3.2-6, 3.2-7, 3.2-8, 8.2-5d (6).
- **Files.** `epistemic_emit_clock.cpp`, `agent_executor_clock.cpp`; `epistemic_translate_clock.cpp`
  (new); `tests/xuzz/vagent_epistemic_clock_test.cpp` (new).
- **Depends on.** I3-3a, T1.
- **C++ and Vortex.** The mapping measurement is a host clock product (C++); the snapshot store and
  the check are IR.
- **Tests.** A query-time lookup of a replaced mapping is refused; a reboot gives no silent grace; a
  flush of stale readings from a healthy device moves nothing; a populated-state resume and upgrade
  test through both copiers keeps the snapshots.
- **Gate.** SENS, DIST, ARCH, CODE, ML.

#### I3-3c. Heartbeats, silence and freeze

*I3-3. Size: about 1,400 lines.*

- **Content.** A per-binding heartbeat expectation; a local health turn that fires with no sample
  arriving; a `silent` flag separate from the refusal fraction and from reasons that `rtHealth`
  reads; a stuck-output rule that needs excitation or an independent constraint (repetition alone
  never flags); a latched source is exempt from silence timers; deadline rebase after resume uses a
  paired mapping or marks stale, never a new grace period; pruning of inactive profiles begins here.
- **Closes.** U-18, 3.2-4, 5.1-20, 5.1-21, 5.1-22, 8.2-4f, 8.2-R3d (7).
- **Files.** `epistemic_emit_health.cpp`, `epistemic_emit_clock.cpp`; `agent_executor.cpp` (timer
  wake, at most 400 lines; deadline rebase in `restoreState`, at most 60 lines);
  `agent_executor_clock.cpp`; `epistemic_translate_clock.cpp`;
  `tests/xuzz/vagent_epistemic_heartbeat_test.cpp` (new).
- **Depends on.** I3-3b.
- **C++ and Vortex.** The timer wake is a host scheduler concern (C++); the flags and rules are IR.
- **Tests.** Silence expires on receive time; a slow source is not flagged by a fast rule; a
  frozen-but-excited sensor is flagged and a constant-by-nature one is not; a resume never restarts
  a full grace period; silence leaves R and N unchanged; a populated-state resume and upgrade test
  through both copiers rebases the deadlines on resume and keeps them on upgrade.
- **Gate.** SENS, DIST, ARCH, EPI, ML.

#### I3-3d. Sensor health reports

*I3-3. Size: about 800 lines.*

- **Content.** A separate path from `healthEntry`: an authenticated report adds an origin-tagged
  `degraded` or `suspect` reason, bounded per source; its recovery percept clears only reasons that
  source added; a hard fault may isolate at once under a policy-declared condition; a sensor
  identity can never clear a latch. The iteration closes here: the translator coverage script runs
  over everything I3 added.
- **Closes.** 4-19, 5.1-10, 5.1-16, 7-4 (4).
- **Files.** `epistemic_emit_health.cpp`, `agent_executor_health.cpp`;
  `tests/xuzz/vagent_epistemic_health_test.cpp`.
- **Depends on.** I3-3c, I3-3a.
- **C++ and Vortex.** IR plus the adapter's authenticated input.
- **Tests.** A report cannot reach `healthEntry`; a bound sensor and a relay cannot invoke a health
  action; recovery clears only its own reasons and records who removed what; an operator `clear` may
  still remove device reasons; origin-tagged reasons survive a populated-state resume and upgrade
  through both copiers; the coverage script lists no uncovered I3 structure.
- **Gate.** SEC, SENS, EPI.

#### I4-1a. Policy encoding 3 and the domain policy record

*I4. Size: about 900 lines.*

- **Content.** Policy encoding `vagent-policy-3`, written once with **every** key of spec 3.1 to 3.4
  and 6.3 in the closed table: `risk`, `c`, `epsilon_w`, `epsilon_floor` and their domains; the SGD
  hyperparameters (`eta0`, `lambda_reg`, clip, floor); warm-up length, feedback horizon and the
  abstention parameters; feature names and floors; the declaration of a functional, exhaustive
  alternative set (what lets `confirm` label competing values negative, I5-1b); the meaning of an
  absent `decay` (I4-D1 settles; the reading is lambda = 0). The checker keeps refusing a model
  other than `beta_reputation` until I5-4. The encoding tag is recorded with the policy master, and
  a checkpoint or program with another tag refuses on T1's channel with a typed error naming both
  tags. The checker refuses an SGD-only key (`eta0`, `lambda_reg`, clip, floor) under any model but
  `online_sgd`, with a typed error, so a program cannot set a key that is accepted and ignored (ML
  minor 11). The abstention parameters follow the criterion shape I4-D2 fixes (prequential loss
  against the Beta baseline, a minimum sample, one margin); I5-D fixes their numbers. Dynamic and
  static validation. The **domain policy record** (the first of the two kinds I4-D2 fixes): parent
  the predicate descriptor, owner of the dependence-group registry. The invariants that no two
  fields share one value cell (2.1-12) and that metadata is never JSON or a bit-packed integer
  (2.1-13) join `epistemic_invariants.hpp`.
- **Closes.** 2.1-12, 2.1-13, 8.1-9 (3).
- **Files.** `epistemic_policy.{hpp,cpp}`; `epistemic_emit_calibration.cpp` (new);
  `epistemic_abi_calibration.hpp` (new); `system_docs.*`; `epistemic_invariants.hpp` (test);
  `tests/xuzz/vagent_epistemic_policy_test.cpp`.
- **Depends on.** I4-D1, I4-D2, T1 (the refusal channel).
- **C++ and Vortex.** The policy front end and IR emission only.
- **Tests.** The tag string is `vagent-policy-3` and `vagent-policy-2` is refused by name, as a
  typed value; every new key has a boundary test, including the refusal of an SGD-only key under
  `beta_reputation`; invalid values refuse by number; expected hashes regenerate; `online_sgd` is
  refused statically; the two invariants.
- **Gate.** ML, NUM, ARCH, CODE.

#### I4-1b. Per-source calibration records and the epistemic-state version

*I4. Size: about 1,500 lines; splits at the state version if it runs over.*

- **Content.** The second record kind: a calibration record per source and domain, found by the full
  key of spec 2.1 (source identity and incarnation, predicate signature, domain or schema ID,
  calibration epoch, model epoch, feature schema), by full-key comparison of typed values (the hash
  is an accelerator only), parented on the source profile. Draft 1 omitted the source from the key,
  which would have shared one reputation among all sources (ML M2). Fields known at this point:
  alpha, beta, decay, epochs, label count and a view of the policy master (2.1-31). List heads for
  weights, bias, normalizer and the ledger are present and absent-valued; their element shape is
  I5-D's, added under the additive rule, and vectors are separate cells whose length is a policy
  value. Priors are applied at creation, which is the one time this slice writes alpha and beta. The
  retired chain is re-homed under the record (X-6). **A health reset calls a calibration reset in
  the same turn (5.1-13) by the rule I4-D2 states (P2-4): it opens a new calibration epoch for each
  of the principal's calibration records in O(1), not by clearing a ledger that does not exist
  yet.** Cell price and the label cap are the values I4-D2 set (P2-5). The shape version is bumped
  and the translator carries the record in this slice. **The epistemic-state version is born here**
  (R3-11, with its first writer): one cell at the head of the agent's `d.epistemic` rank beside the
  shape version, minted under the same rule (only for an agent whose program declares an arbitrated
  predicate), one routine `rtBumpStateVersion`, and calls to it at every writer I3-2c2 listed
  (health actions, a change of derived state, supersession, an advance or retire that withdraws
  supports, a calibration reset). The bump rule is the one in section 2.
  `hasTranslatableEpistemicState` is extended to ignore this cell too. The version is not carried
  across resume or upgrade, and a writer-enumeration test fails when a health writer does not bump.
  Closing 5.1-13 waits for I5-1a, which adds the ledger the reset must also forget.
- **Closes.** 2.1-7, 2.1-14, 2.1-27, 2.1-31, 2.1-37, 2.1-38, 3.1-2, 8.2-5a, 8.2-R2e (9).
- **Files.** `epistemic_emit_calibration.cpp`, `epistemic_abi_calibration.hpp`;
  `epistemic_emit_health.cpp` (the reset turn and the bump calls);
  `epistemic_translate_calibration.cpp` (new); `epistemic_invariants.hpp` (test);
  `tests/xuzz/vagent_epistemic_calibration_test.cpp` (new), `vagent_epistemic_health_test.cpp`.
- **Depends on.** I4-1a, I4-D2.
- **C++ and Vortex.** IR only.
- **Tests.** Lookup by full key with forced hash collisions; the record starts at the declared
  prior; a health reset returns it to the prior and opens a new epoch in constant work; every health
  writer bumps the state version exactly once and an `R` or `N` drift never does; the version is
  absent for a non-arbitrating agent, which still upgrades; a populated-state resume and upgrade
  test through both copiers carries the record and the re-homed chain; the previous shape version
  refuses by number on T1's channel; budget parity at the cap, and a 16-record reset with
  calibration records within half the grant.
- **Gate.** ML, NUM, ARCH, CODE, EPI, DIST.

#### I4-2. Scoring routines

*I4. Size: about 1,300 lines.*

- **Content.** Beta mean and variance, the freshness decay `w(a)`, the lower bound for `risk: "lcb"`
  as I4-D1 rules for an uninformative prior (O9: floor at 0.5 before decay, or keep the text and
  document the inversion), the clamp, a numerically stable sigmoid and logit, compensated or
  rescaled sums, and dynamic validation returning `InvalidEpistemicPolicy` before arithmetic. Each
  is a pure scalar `std:` routine with a fixed ABI in `vortex_stdlib_epistemic_score.cpp`,
  registered from `vortex_stdlib.cpp`, and loaded into an actor's arena by the mechanism I4-D2
  settles. The golden vectors are the committed data file and reference program of I4-D1, written
  before this IR exists and not by its author.
- **Closes.** 1-4, 3.1-1, 3.1-5, 3.1-11, 3.1-20, 3.2-1, 3.2-2, 3.4-1, 3.4-4, 3.4-5, 8.P2-1, 8.1-11,
  8.1-12, 8.2-2f (14).
- **Files.** `vortex/vortex_stdlib.{hpp,cpp}` (declaration and registration, at most 60 lines); new
  `vortex/vortex_stdlib_epistemic_score.cpp`; `tests/xuzz/vortex_stdlib_epistemic_test.cpp` (new,
  bare `WorkerContext`); `tests/xuzz/vagent_epistemic_scoring_test.cpp` (new, the actor path).
- **Depends on.** I4-1b, I4-D1, I4-D2.
- **C++ and Vortex.** Graph construction is C++ and its justification is written in I4-D2; the
  algorithm runs in the VM over `Exp`, `Ln`, `Sqrt`. No C++ evaluator.
- **Tests.** Vectors within the stated relative error, each with its margin; closed forms (prior 1/2
  with w = 0.8: one positive report 0.8, an independent opposite report back to 0.5; decay at one
  half-life is exactly halfway to 0.5); metamorphic properties (`logit(w)` odd about 0.5, a neutral
  source changes nothing, scaling all `q` by zero is a no-op); extremes (alpha near 10^12, a logit
  at the clamp, an underflowing exponential); a non-finite intermediate refuses; age never changes
  alpha or beta; a case where naive summation of 128 mixed-sign logits loses a stated number of bits
  and the routine does not; `lcb` at a symmetric prior behaves as I4-D1 ruled. The report states
  that this gate passes the formulas and the extremes and reserves reputation separation to I4-6.
- **Gate.** ML, NUM, VM.

#### I5-1a. Label ledger and the single apply routine

*I5. Size: about 1,500 lines; splits at the reset interaction if it runs over.*

- **Content.** The ledger entry (target identity and version, feature snapshot, scaler and model
  epoch, **calibration epoch**, effective ID, predecessor, `y`, `q`, first-accept ordinal, evidence;
  field list and cap from I4-D2) and the immutable verification-version records; the one IR routine
  `ledgerApply` that appends an entry, or replaces one by naming its effective predecessor (else
  `VerificationConflict`), allows at most one effective adjudication per observation, recomputes
  alpha and beta from the epoch prior plus the **current-epoch** ledger sums in first-accept order
  (never incrementally), bumps the state version through `rtBumpStateVersion`, and refuses
  `EpistemicBudget` at the ledger cap. It is the only writer of alpha and beta after creation (ML
  M1, architect minor 5). **A reset reaches the ledger by the rule of P2-4:** the reset's
  calibration step calls this routine's recompute for the new epoch, which yields the prior; entries
  of earlier epochs are history that no longer scores and no longer counts against the cap (excised
  in bounded steps or counted separately, as I4-D2 states); a pending label that names the old epoch
  gets `ExpiredLabel`. It is driven by a test harness turn, exactly as I3-2c2 drives the health
  turn; the target is an observation ID; the token match and authorization stay with I3-4a. The
  ledger heads are owned structure, so the translator carries them here.
- **Closes.** 3.1-3, 5.1-13, 6.3-3, 6.3-8, 6.3-10, 6.3-11, 6.3-12, 6.3-13, 6.3-17, 6.3-19, 8.1-6
  (11).
- **Files.** `epistemic_emit_ledger.cpp`, `epistemic_abi_ledger.hpp` (new);
  `epistemic_emit_health.cpp` (the reset step calls the recompute);
  `epistemic_translate_calibration.cpp`; `tests/xuzz/vagent_epistemic_ledger_test.cpp` (new).
- **Depends on.** I4-1b, I4-2.
- **C++ and Vortex.** IR; no host API.
- **Tests.** The recompute against a closed form and the reference vectors; a correction replaces
  rather than adds; two adjudicators cannot append two examples for one report; weight outside (0,
  1\] refuses; a label that cannot fit is `EpistemicBudget` with nothing written; **reset, then
  apply a label: the result equals the prior plus the new label only**; a reset of 16 records with
  full ledgers fits the grant (budget parity, work independent of ledger length); a label that names
  an observation from before the reset is refused; entries of an earlier epoch do not count against
  the cap; a populated resume and upgrade test through both copiers is bit-equal for alpha, beta and
  the ledger. The ML report lists what it passes (the update rule and the reset) and reserves the
  loop through the transport to I5-1b.
- **Gate.** ML, EPI, NUM, DIST.

#### I4-3. Tempered log-opinion pooling

*I4. Size: about 1,000 lines.*

- **Content.** The pool `s(C)`, `n_eff`, `W(C)`: one vote per source per candidate, identical
  originals counted once, the default single group with rho = 1, stable source and event-ID order,
  neutral pooling at prior 0.5.
- **Closes.** 3.1-8, 3.1-9, 3.1-10, 3.1-19, 5.3-8, 8.2-2a (6).
- **Files.** new `vortex/vortex_stdlib_epistemic_pool.cpp`; `vortex_stdlib.{hpp,cpp}`
  (registration); `tests/xuzz/vortex_stdlib_epistemic_test.cpp`,
  `vagent_epistemic_scoring_test.cpp`.
- **Depends on.** I4-2.
- **C++ and Vortex.** As I4-2.
- **Tests.** Permutation invariance of the result; copy and relay sources; neutral pooling; the
  single-group fallback; order-stable summation against the reference; the property that in one
  group with rho = 1 a weaker corroborating source lowers the score (0.9 and 0.55 give 0.768, below
  0.9), so nobody reads the pool as accumulating evidence inside a group; saturation of the tie
  tolerance as I4-D1 ruled.
- **Gate.** ML, NUM, EPI.

#### I4-4. Dependence groups and their admission

*I4. Size: about 1,500 lines.*

- **Content.** The group registry under the domain policy record, memberships as clone occurrences,
  resolution by identity, predicate, domain and epoch, a disjoint non-empty audited partition, rho
  validated at load and on update, fuel charged per walk, `DependentMeasurements` when dependence is
  unknown, and a host admission API under a grant (the I3-2c1 pattern) whose audit provenance is
  copied into the arena at admission, so no later decision rests on a host-only fact. Groups are
  owned structure, so the translator carries them here.
- **Closes.** 2.1-19, 3.1-12, 3.1-13, 3.1-14, 3.1-15, 3.1-16, 3.1-17, 3.1-18, 5.3-13, 8.2-2b, 8.2-2c
  (11).
- **Files.** `epistemic_emit_calibration.cpp`; `agent_executor_health.cpp` (grant plumbing, at most
  200 lines); `epistemic_translate_calibration.cpp`; `tests/xuzz/vagent_epistemic_groups_test.cpp`
  (new).
- **Depends on.** I4-3, I3-2c1.
- **C++ and Vortex.** Admission and authorization only; groups and pooling are cells and IR.
- **Tests.** An overlapping or empty partition refuses; rho outside [0,1] or non-finite refuses at
  load and at update; unassigned sources fall back to one group; a forged audit provenance is
  refused; the walk cost is charged; a populated-state resume and upgrade test through both copiers.
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
- **Files.** `token.hpp`, `lexer.cpp`; `parser_epistemic.cpp`, `checker_epistemic.cpp` (new), with
  dispatch lines in `parser.cpp`, `checker.cpp`; `ast.hpp`;
  `tests/xuzz/vagent_epistemic_syntax_test.cpp` (new).
- **Depends on.** I4-4 (so the checker knows the policy fields).
- **C++ and Vortex.** The compiler front end.
- **Tests.** Each modifier and each ambiguity; a body with a send, a spawn or a belief change in a
  ranked guard refuses; a variance and a score are not interchangeable; `check-runtime` reports each
  new diagnostic.
- **Gate.** CODE, EPI, VM, ML (named items: `lcb` with `online_sgd`, and the variance type).

#### I4-6. `truest` end to end

*I4. Size: about 1,500 lines, the largest slice.*

- **Content.** Emit `ScopedEvaluate` at the query site with a compiler-supplied callback graph and
  the frame I4-D2 fixed from the S0 spike; the eligibility pass (source, health recomputed through
  `rtHealth` at query time, age, verdict, `allow_untested`); scoring through I4-2 and I4-3; `truest`
  selection with tie abstention; refusals mapped from `TypeMismatch` and `CallbackFault` to typed
  epistemic refusals, distinguishable from failure; a plain query of an arbitrated predicate becomes
  the arbitrated answer (Q1); `max_candidates` enforced; composite export of the result. If the
  slice exceeds 1,500 lines in review it splits at the eligibility pass.
- **Closes.** 1-2, 2.2-5, 3.2-3, 3.2-9, 3.4-6, 4-35, 4-49, 4-51, 4-54, 5.1-1, 5.1-6, 5.1-17, 5.1-19,
  5.2-3, 5.2-17, 5.3-1, 5.3-7, 5.3-9, 5.3-10, 6.1-2, 6.1-12, 6.1-17, 6.1-20, 6.1-21, 6.1-26, 8-3,
  8.P8-2, 8.P8-3, 8.1-4, 8.2-4e, 8.2-5g, 8.2-R3c (32).
- **Files.** `epistemic_emit_query.cpp`, `epistemic_abi_query.hpp` (new); `compiler.cpp` (call
  sites, at most 400 lines); `agent_executor.cpp` (grant, refusal propagation, at most 100 lines);
  `vortex/vortex_scoped_evaluation.cpp` only for a missing test;
  `tests/xuzz/vagent_epistemic_query_test.cpp` (new).
- **Depends on.** I4-5, I3-3b, I3-2c2, I4-4, I5-1a.
- **C++ and Vortex.** Emission and refusal propagation. The VM side already exists.
- **Tests.** A query never advances a counter, refreshes a timestamp or enqueues anything; repeated
  queries leave the arena size flat; `CutBelowFloor` has a named test; the refusal is not `Empty`;
  no eligible fact is not an unscored fallback; a query reads the committed verdict and live health;
  the cold-start `UNTESTED` path; **reputation separation**: two sources with different ledgers,
  written through `ledgerApply`, rank differently, which is the first claim of the ML gate that no
  earlier slice could make; a priors-only run at an asymmetric prior selects by age and the test
  says so.
- **Gate.** CODE, VM, ARCH, EPI, ML.

#### I4-7a. `ranked`, continuations and stale queries

*I4. Size: about 1,200 lines.*

- **Content.** A generator whose owner holds stable IDs, scores and position in the handler
  activation's scope, outside the per-evaluation mark; each resume scopes its own scratch; `ranked`
  enumerates ties (the tie group is anchored at the greatest remaining score and is not transitive)
  and fails only when exhausted; a change to belief or the epistemic-state version (born in I4-1b)
  gives `StaleEpistemicQuery`; exhaustion releases the continuation; an upgrade invalidates
  continuations. A query writes no durable state, so the arena-flat test of I4-6 holds here too
  (architect M4).
- **Closes.** 1-6, 2.1-41, U-9, 4-47, 4-53, 5.3-11, 6.1-28, 6.2-1, 6.2-2, 6.2-3, 6.2-4, 6.2-5,
  6.2-6, 8.2-3e (14).
- **Files.** `epistemic_emit_query.cpp`, `epistemic_abi_query.hpp`; `epistemic_translate_query.cpp`
  (new, invalidation only); `tests/xuzz/vagent_epistemic_query_test.cpp`.
- **Depends on.** I4-6.
- **C++ and Vortex.** IR; no host API.
- **Tests.** A ranked generator survives a mark release of its enumeration, not of the belief; a
  state version bump between resumes is `StaleEpistemicQuery`; exhaustion releases the continuation;
  a ranked body with an effect never reaches the VM; an upgrade invalidates a live continuation; the
  arena size is flat across repeated queries.
- **Gate.** VM, CODE, ARCH, EPI, ML (named items: the non-transitive tie group and the stale rule).

#### I4-7b. Commit-point verification tokens

*I4. Size: about 1,000 lines.*

- **Content.** The verification token (signature, key and value tuple, observation IDs and versions,
  occurrences, context snapshot, state version) is **minted at the commit point where the plan acts
  on a selection** (I4-D2 defines "acts on"), and not by a read. A verifier-requested token has no
  message in the I3-3a union, so the clause stays out unless I4-D2 defines one (architect minor 9).
  It lives on the agent's `d.epistemic` rank (lead ruling O3), bounded, charged, and retained at
  least as long as an issuer's pending record can live, so a legitimate in-flight verification never
  becomes `ExpiredVerificationTarget`. Retention is counted in turns or state-version steps, never
  wall time, so replay is deterministic; eviction never precedes expiry; at the cap minting refuses
  with a typed error and evicts nothing; I4-D2 defines the pending-record life setting and settings
  load checks `retention >= pending life + retransmit bound` (distributed minor 5). It carries the
  state version and the observation versions. Tokens are owned structure, so the translator carries
  them and verifies the mapped query versions here. This is a proposed amendment to 2.1 ("an
  intention heads query tokens") until intentions exist.
- **Closes.** 2.1-9, 2.1-17, U-7, 8.2-5f (4).
- **Files.** `epistemic_emit_query.cpp`; `epistemic_abi_query.hpp`; `epistemic_translate_query.cpp`;
  `tests/xuzz/vagent_epistemic_query_test.cpp`.
- **Depends on.** I4-7a, I4-D2.
- **C++ and Vortex.** IR; no host API.
- **Tests.** A read mints no token; an acted-on selection mints exactly one; retention and the bound
  hold; the settings check rejects a retention shorter than the pending-record life plus the
  retransmit bound (the pending record itself arrives at I3-4b2, so this slice tests the setting
  only); minting at the cap refuses and evicts nothing; tokens survive resume and upgrade through
  both copiers with their mapped versions; a token whose observation was retired is
  `ExpiredVerificationTarget` once outside retention.
- **Gate.** VM, CODE, ARCH, DIST, EPI.

#### W1. Production path: the adapter in `vagent run`

*Production path. Size: about 900 lines.*

- **Content.** `vagent run` accepts the signed-observation adapter of I3-3a: a binding policy whose
  carrier I3-3D decides (an alias-to-principal table cannot be one setting, because a setting is a
  flat typed tuple; the carrier is a system xanadoc dimension or a file path named by a setting; no
  key material in either, the keys come from the identity ledger and device delegation), a
  command-line option to enable it, the transport that carries `relayedBy` as I3-3D states it (an
  in-process loopback peer, or "none, relay unsupported in W1", in which case the relay-marking test
  runs in I3-3a only and W1 says so), and one headless end-to-end journey from a signed reading to a
  `truest` and a `ranked` answer through the CLI. After it, 2.1-45 holds outside `AgentExecutor`
  tests. If the lead rules instead that CLI integration is outside this plan, section 8 must say
  that P19 is delivered as a runtime and library capability, with that wording in the final record
  (architect M8, alternative (b)).
- **Closes.** 2.1-45 (1).
- **Files.** new `apps/vagent/epistemic_cli.cpp`; dispatch in `apps/vagent/main.cpp` (at most 100
  lines); `system_docs.*`; `docs/vagent-user-guide.md`; `tests/xuzz/vagent_cli_test.cpp`.
- **Depends on.** I3-3a, I3-3D (the carrier), I4-7a.
- **C++ and Vortex.** Host C++: argument parsing, key lookup and transport are the boundary.
- **Tests.** The journey; a forged or replayed signed reading is refused through the CLI; the
  settings entry has a schema and an expected-list entry.
- **Gate.** SEC, DIST, CODE, ARCH, ML (named items: the guide text only, where "most reliable source
  wins" before labels or a score read as a probability would first appear).

#### I3-4a. Verification grants, surface and target resolution

*Verification loop. Size: about 1,400 lines.*

- **Content.** `EpistemicVerifyGrant` (verifier, actor, predicate and domain, evidence scope, epoch
  and expiry, action, maximum weight); a verification percept as a member of the I3-3a tagged union
  and the adapter check (missing, expired, mismatched, repeated or conflicting targets, out-of-scope
  evidence, excess weight); `confirm` and `refute` syntax and checker, legal only in a
  host-validated handler (`UnauthorizedVerification`); target resolution by observation ID and token
  match against the commit-point token of I4-7b (`MissingVerificationTarget`,
  `ExpiredVerificationTarget`); the rule that a report source cannot certify itself without a
  distinct adjudicator (decision F14 applies host-wide); the verification record shape. Refusals
  that depend on host state (no grant, unknown issuer, quota) are transport-level typed errors that
  journal nothing and leave the issuer pending (distributed M2, class a).
- **Closes.** 1-5, 2.1-18, 4-5, 4-9, 4-10, 4-11, 4-12, 4-13, 4-15, 4-16, 4-17, 4-21, 4-50, 6.3-1,
  6.3-2, 6.3-4, 8.2-4b (17).
- **Files.** `token.hpp`, `lexer.cpp`; `parser_epistemic.cpp`, `checker_epistemic.cpp`;
  `agent_executor.hpp`, `agent_executor_verify.cpp` (new); `epistemic_emit_verify.cpp`,
  `epistemic_abi_verify.hpp` (new); `epistemic_adapter.cpp`;
  `tests/xuzz/vagent_epistemic_verify_test.cpp` (new).
- **Depends on.** V-D, I4-7b, I3-3a, I3-2c1.
- **C++ and Vortex.** Grant and percept checks are authorization (C++); the syntax is the front end;
  the record is IR.
- **Tests.** Transport authentication alone confers no verification authority; self-issued and
  unauthorised verification refuse; a repeated or conflicting target refuses; weight outside (0, 1\]
  refuses; absence never justifies `refute` without an observation model and coverage; a refusal
  caused by a missing grant leaves no journal record.
- **Gate.** SEC, EPI, DIST, CODE, ML.

#### I3-4b1. Intake journal, replay floor and idempotency

*Verification loop. Size: about 1,500 lines.*

- **Content.** The fixed **decision order at intake**, tested in this order: authenticate the
  issuer; check the replay floor; look up the journal (a cached outcome, or
  `DuplicateVerificationPayload` for a different payload under the same ID); authorize; resolve the
  target and its expiry. Cached lookup precedes epoch or target expiry, so an identical retry after
  an epoch change returns the cached outcome (6.3-23) and not `ExpiredVerificationTarget`. **The
  floor and the key** (P2-2, distributed M1). The event ID is (issuer, incarnation, sequence),
  carried in the signed header that I3-3D fixes for every message kind. The floor is an
  **issuer-declared low-watermark**: each signed verification carries the lowest sequence the issuer
  will still retransmit, and everything below it is terminal at the issuer. The receiver keeps a
  cached outcome for every sequence at or above the watermark and refuses below it with
  `VerificationTooOld`; it raises the stored watermark only to a signed value and never infers one.
  A highest-seen floor would turn ordinary loss into a permanent failure (a simulation with 5% loss
  and one event per time unit ends 5.3% of events in `VerificationTooOld`; the watermark makes it
  zero). The window from the watermark to the highest accepted sequence is bounded by a setting and
  a cell price per entry; when it is full the refusal is the retryable class (a)
  `VerificationWindowFull`, and nothing is evicted, because eviction makes a retry look too old. The
  issuer's incarnation is monotone and refused below the stored one, as for observations; reuse of
  an old (incarnation, sequence) with a different payload fails safe as
  `DuplicateVerificationPayload`. The floor is arena state per authenticated issuer, because the
  mesh floors (`Actor::floors`) and the agent-level floors (`State::floors`) are host memory that no
  checkpoint carries, and it is created **only when intake commits**: an authenticated issuer with
  no grant leaves no cell. An authenticated intake turn admits an owned receiver journal record on
  `d.epistemic` before acceptance is reported, **copying into it every input the processing decision
  needs** (grant identity and scope, maximum weight, epoch, expiry, evidence scope), so processing
  reads only arena state and a resume on another host with no grants cannot change a cached outcome
  (class b). The journal, pending records and outcome checks are keyed by a **stable agent
  identity** stored in the arena (name plus lineage ID), never by pid, because `resume` mints a new
  pid (`mesh_actor.cpp:330`). A host-posted verification stays pending until intake commits (it no
  longer uses drop-on-settle). Dedup by issuer, event ID and payload; the audit of mesh receipt
  semantics (8.P5-2), which V-D writes and this slice pins with a test that ordinary dispatch is
  unchanged. Intake of a verification message from the mesh mailbox is program-independent, as T1-D
  rules, so an upgrade with one queued does not hand it to a program that cannot read it. The
  journal and floors are owned structure, so the translator carries them here.
- **Closes.** 2.1-26, 2.1-28, 2.2-9, U-20, U-22, 3.3-5, 4-14, 6.3-23, 6.3-24, 6.3-25, 6.3-26, T-1,
  T-2, T-4, T-5, T-10, DF-5, 8.P5-2, 8.2-2d (19).
- **Files.** `agent_executor.cpp` (the pending-event path, at most 400 lines);
  `agent_executor_verify.cpp`; `epistemic_emit_verify.cpp`, `epistemic_abi_verify.hpp`;
  `epistemic_translate_verify.cpp` (new); `tests/xuzz/vagent_epistemic_journal_test.cpp` (new); the
  V-D audit note under `design/`.
- **Depends on.** I3-4a.
- **C++ and Vortex.** The pending-event and issuer-record handling is transport (C++); the journal
  is IR.
- **Tests.** An exact retry returns the cached outcome and relearns nothing; a failed intake
  publishes nothing; a different payload under the same ID is `DuplicateVerificationPayload`; a
  delivery below the watermark mutates nothing; **a retry of an uncommitted event after a later
  sequence committed is accepted** (the row that fails under a highest-seen floor); a retry at,
  inside and below the watermark; a full window answers `VerificationWindowFull` and evicts nothing;
  an incarnation regression is refused; a watermark raised by an unsigned or replayed header is
  refused; an ungranted authenticated issuer leaves no cell; a retry after resume, and after an
  upgrade that opens a new epoch; resume with no grants, redeliver, expect a retryable refusal and
  no cached outcome, then re-grant, redeliver, expect one application; an ordinary failing user
  handler behaves as before; a verification in the mesh mailbox across an upgrade; a populated-state
  resume and upgrade test through both copiers.
- **Gate.** DIST, SEC, ARCH, CODE, ML (named items: the watermark window against the feedback
  horizon of 3.3).

#### I3-4b2. The issuer side

*Verification loop. Size: about 1,000 lines.*

- **Content.** Draft 1 gave the issuer no owner (distributed answer 1). Both issuers are named: the
  host (the operator and the adapter) and an actor. The issuer keeps a bounded on-manifold pending
  record keyed by issuer, event ID and payload identity; it states who retransmits (a host timer or
  the actor), the backoff and its bound, and the record's maximum life, which is at least the
  retention of the commit-point token it targets (the setting and its unit are I4-D2's, so the
  inequality is checked in one place). The issuer declares the low-watermark of its signed header:
  the lowest sequence it will still retransmit, raised only when it gives up on an event. It removes
  its pending record only on a checked `VerificationOutcome` that names the exact actor by stable
  identity and the issuer and event ID. `VerificationTooOld` is a terminal outcome of unknown commit
  state, now rare because the watermark is the issuer's own: the issuer surfaces it to an operator
  and does not retry. `VerificationWindowFull` is retried with the same backoff. **An issuer
  restored from a checkpoint takes a fresh incarnation** (A3 applied to issuers, decision D2), so a
  regressed counter cannot reuse an old (incarnation, sequence).
- **Closes.** T-3, T-9 (2).
- **Files.** `agent_executor_verify.cpp`; `epistemic_emit_verify.cpp`;
  `epistemic_translate_verify.cpp`; `tests/xuzz/vagent_epistemic_journal_test.cpp`.
- **Depends on.** I3-4b1.
- **C++ and Vortex.** Retransmit and expiry timers are host scheduling (C++); the pending record is
  IR.
- **Tests.** A lost reply causes a retransmit with backoff and a bound; the pending record survives
  resume and upgrade; an outcome that names another actor or event does not clear it;
  `VerificationTooOld` surfaces and does not retry; `VerificationWindowFull` retries; a pending
  record outlives the token it targets; the watermark rises only past events the issuer gave up on;
  an issuer resumed from a checkpoint sends a fresh incarnation.
- **Gate.** DIST, SEC, ARCH, CODE.

#### I3-4c. Processing turn, settle queue and crash points

*Verification loop. Size: about 1,500 lines.*

- **Content.** A separate processing turn after a successful non-speculative turn; queue entries on
  `$agent +d.epistemic` that own their payload cells and roll back with an enclosing mark; bounded
  retry with backoff. The **write set is real** (distributed M7): the immutable verification-version
  record and the effective-label ledger entry with its predecessor link, written through
  `ledgerApply` with the Beta recompute inside the same transaction as the cached outcome and the
  staged response. The outcome set is defined once here (`Applied` and the typed rejections); there
  is no `RecordedNotApplied`, because a cached terminal outcome is immutable (6.3-23). A terminal
  typed rejection is recorded and reported; a `VerificationOutcome` message is staged in the
  journal's cached copy, not the outbox; a lost response is re-emitted from the cache; a later plan
  failure cannot undo a published outcome. **The processing trigger** after resume or upgrade is a
  scan of the arena queue at the end of `upgrade` and at the first turn after `resume`, which
  re-arms backoff from the stored attempt count, so "bounded retry" stays bounded across restarts.
  `needsFirstTurn_` is not that hook for upgrade: the upgrade tail pushes the pid, but
  `pumpUntilIdle` skips a prepared, initialized actor (`agent_executor.cpp:3228`) and an upgraded
  actor keeps `initialized`. A migration that opens a new epoch turns queued entries into
  `ExpiredLabel` with their cached outcome, not a silent drop. **Decision D2 is implemented here.**
  (1) A second live resume of one checkpoint in one executor is refused with a typed error on T1's
  channel: the checkpoint content hash already has a live actor. (2) Cross-host fork prevention is
  an operator precondition, stated in the user guide and in the F1 and F2 limits, and is not claimed
  as implemented; a lease without fencing would not be safe across a partition. (3) A rollback to an
  older checkpoint loses verifications acknowledged since (option (c) of section 7.2): the journal,
  the cached outcome and the label disappear together, so the state is self-consistent, and the test
  states exactly what is lost. (4) A rolled-back issuer resends from a regressed counter, which
  I3-4b2's fresh incarnation covers. The queue entries are owned structure, so the translator
  carries them here.
- **Closes.** 2.1-6, 2.2-6, 6.3-27, 6.3-28, T-6, T-7, T-8, T-11, T-12, T-15, T-16, 8.P5-1, 8.2-4a,
  8.2-4d, 8.2-R4b (15).
- **Files.** `agent_executor.cpp` (turn scheduling and the trigger, at most 400 lines);
  `agent_executor_verify.cpp`; `epistemic_emit_verify.cpp`; `epistemic_translate_verify.cpp`;
  `Makefile` (the new fuzz target and its `fuzz:` entry);
  `tests/xuzz/vagent_epistemic_journal_test.cpp`; `tests/fuzz/fuzz_epistemic_verification.cpp`
  (new).
- **Depends on.** I3-4b2, I5-1a, I3-3c (the timer wake is the backoff scheduler), T1 (the channel).
- **C++ and Vortex.** Turn scheduling, the trigger and the outcome message are host C++; the queue
  and records are IR.
- **Tests.** Fail processing after intake, redeliver, commit once, lose the reply, redeliver, then
  fail a later plan: the journal, the cached outcome and the issuer state follow spec 6.3; atomic
  publication; `EpistemicBudget` on a full ledger; rollback of record and outcome together; a
  starved turn is never an invented label; extra turns are charged to the budget; an idle upgraded
  actor with a queued entry is processed; a double resume of one checkpoint is refused with the
  typed value. **The crash-point matrix runs under the two fault models of V-D** (P2-3). *In-turn
  abort*: the processing turn is aborted by fault injection at each statement boundary of its write
  set (record, ledger entry, cached outcome, staged response) and the enclosing mark rolls
  everything back together; expected: journal still pending, no outcome, no label. *Restart from the
  last checkpoint*: after intake, after processing and after the response was sent; the expected
  journal state of the actor, pending record of the issuer and the result of the issuer's next
  retransmit are tabulated per row for the D2 behaviour. For upgrade the rows are "refused as not
  quiescent" for the points with a staged response or an unreplied send and "state carried" for
  committed points, each listed. A post racing the swap.
- **Gate.** DIST, ARCH, CODE, EPI, ML (the label write set and the Beta recompute).

#### I5-1b. Competing values, corrections and the whole loop

*I5. Size: about 1,200 lines.*

- **Content.** What needs the journal and the transport: `confirm` labels only the referenced
  agreeing observations, and competing values are labelled negative only for a declared exhaustive
  functional set (the key of I4-1a); `refute` labels only referenced contradicted observations
  negative and never rewards an untested alternative; neither directive reaches back to what a
  mutable source currently asserts; each correction is a new immutable event with a unique ID from
  its authenticated issuer and names its effective predecessor; a correction of a retired
  observation fixes its label and never makes it current; `ExpiredVerificationTarget` beyond the
  retained epoch or replay horizon, with no compensating label; ordinary new-epoch training needs
  fresh eligible targets; the cost statement is measured. The whole loop is tested: observe, query,
  verify, label, re-query changes the ranking.
- **Closes.** 2.1-29, 6.3-5, 6.3-6, 6.3-7, 6.3-9, 6.3-21, 6.3-22, 8.P5-3 (8).
- **Files.** `epistemic_emit_ledger.cpp`; `epistemic_emit_verify.cpp`;
  `tests/xuzz/vagent_epistemic_ledger_test.cpp`, `vagent_epistemic_journal_test.cpp`.
- **Depends on.** I3-4c.
- **C++ and Vortex.** IR; no new host API beyond I3-4.
- **Tests.** The full loop; a correction of a retired observation never revives it over a newer
  sequence; competing values without a declared set are not labelled negative; gate acceptance alone
  never trains. The report passes the whole-loop claim.
- **Gate.** ML, EPI, NUM, DIST.

#### W2. Production path: verification through `vagent run`

*Production path. Size: about 700 lines.*

- **Content.** The verification percept and grant reach `vagent run`: a verify grant option, a
  headless journey in which a signed verification arrives, is journalled, redelivered and applied
  once, a reply is lost and re-emitted, and the ranking changes. "A reply is lost" uses the
  fault-injection hook that V-D names for the in-turn abort and the lost response (a test transport
  that drops a message by predicate), not a sleep.
- **Closes.** No rows; it carries the verification journey of section 8 item 5.
- **Files.** `apps/vagent/epistemic_cli.cpp`; `docs/vagent-user-guide.md`;
  `tests/xuzz/vagent_cli_test.cpp`.
- **Depends on.** I5-1b, W1.
- **C++ and Vortex.** Host C++, as W1.
- **Tests.** The journey; an unauthorized verification refuses through the CLI.
- **Gate.** SEC, DIST, EPI, ML (named items: the guide text only).

#### I4-8. Verdicts, innovation and slew gates, cold start

*I4. Size: about 1,500 lines.*

- **Content.** Ingestion computes a verdict (`ACCEPTED`, `REJECTED`, `UNTESTED`); the innovation
  gate with `r_tilde = r / max(w, eps)`; the slew gate with strict `dt > 0`; a host-supplied
  independent reference through a grant (O6), never the candidate or a fusion containing it; gate
  quantiles by dimension. **The gate-rejection rule is the one I4-D1 writes under O10.** The default
  that design starts from (ML M4) reuses I3-2a's update routine, parameterised by its state cells,
  decay factor and evidence floor, and the evidence-floor rule of the health fraction, but keeps
  separate state (`gate_rejected`, `gate_events`, where `N` counts tested events only, never
  `UNTESTED` ones), separate settings and separate nominal rates, and uses the fraction for SUSPECT
  and for FAULTED, with FAULTED also needing a minimum number of events. At a 100 Hz healthy sensor
  behind a 95% gate, three consecutive rejections occur about every 84 seconds and five about every
  9.4 hours, so a bare streak would latch healthy devices. A streak is kept only if I4-D1 gives it a
  time or event window, and inflation is applied once. Accepted measurements clear only the streak.
  Cold start stays `UNTESTED` with no blame and no training.
- **Closes.** 3.1-4, 5.1-3, 5.1-9, 5.1-15, 5.1-18, 5.2-6, 5.2-10, 5.2-11, 5.2-12, 5.2-13, 5.2-14,
  5.2-15, 5.2-16, 5.2-18, 5.3-2, 5.3-3, 7-7, 8.2-2l (18).
- **Files.** `epistemic_emit_gates.cpp` (new); new `vortex/vortex_stdlib_epistemic_gate.cpp`;
  `agent_executor_health.cpp` (reference grant); `epistemic_policy.cpp`;
  `tests/xuzz/vagent_epistemic_gates_test.cpp` (new).
- **Depends on.** I5-1b, I4-6, O6.
- **C++ and Vortex.** The reference grant is authorization; everything else is IR or `std:`
  routines. The new profile fields are additive, so the translator needs a test and no new code.
- **Tests.** Gate vectors against the reference with margins; a rejection blames no sensor and
  trains nothing; the false-latch rate is met by the bound I4-D1 proves at the stated event rate and
  device-year figure (ten million simulated healthy events can only bound a rate by about 3/n, which
  is one latch per 9.3 hours at 100 Hz, so a simulation at p = 0.05 and 0.01 is a smoke test of the
  implementation against the bound and not the evidence); a common-mode drift where rejections
  arrive in runs; a ramp drift below the gate and a common-mode drift are accepted but produce no
  calibrated claim; a candidate cannot corroborate itself; duplicates and out-of-order fixes never
  divide by zero; a populated-state resume and upgrade test through both copiers.
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
- **Files.** new `vortex/vortex_stdlib_epistemic_fusion.cpp`; `epistemic_emit_fusion.cpp` (new);
  `epistemic_emit_query.cpp`; `tests/xuzz/vagent_epistemic_fusion_test.cpp` (new).
- **Depends on.** I4-8, I4-4, I4-7a.
- **C++ and Vortex.** IR and `std:` routines.
- **Tests.** Fusion against the reference with margins; oversized-component determinism; a
  moving-target clock-window counterexample; zero or missing variance; unknown dependence refuses;
  fusion cost is charged to fuel and cells.
- **Gate.** ML, NUM, SENS, EPI, VM.

#### I4-10. Health directives and `assert ... with variance`

*I4. Size: about 1,000 lines.*

- **Content.** `reset`, `isolate`, `degrade` and `clear source` lower onto `healthEntry` and refuse
  `UnauthorizedHealthAction` outside a handler entered through an authenticated operator percept;
  the new `assert Term from Postfix with variance` form feeds arbitrated predicates through the
  percept envelope while base `assert` stays refused (X-15); a self-authored fact is scorable as
  testimony with an explicit time, frame and variance provider.
- **Closes.** 4-4, 4-6, 4-7, 4-8, 4-25, 4-46 (6).
- **Files.** `parser_epistemic.cpp`, `checker_epistemic.cpp`, `ast.hpp`;
  `epistemic_emit_health.cpp`; `tests/xuzz/vagent_epistemic_directives_test.cpp` (new).
- **Depends on.** I4-7a, I3-2c2.
- **C++ and Vortex.** Front end and IR.
- **Tests.** A program cannot hold a token; directives behave exactly as the host API (mirror,
  epoch, receipt); an unprivileged plan cannot fabricate a source or time annotation.
- **Gate.** SEC, EPI, CODE, DIST, ML (named items: which weight a self-authored fact earns, since
  scoring it as testimony is the self-confirming loop of 3.1).

#### I4-11. I4 close-out

*I4. Size: about 900 lines plus documents.*

- **Content.** Audit gates 2 and 3 against the matrix; `check-runtime` and `run` tests for every
  diagnostic; a query fuzzer; measured costs (emitted size, peak cells, VM cycles under sustained
  ingestion and repeated queries); the re-measurement of the cell ceiling and the ledger cap (set in
  I4-D2, P2-5); the user-guide worked examples with a complete policy; the translator coverage
  script over I4; and the I4 whole-iteration ML review over the learning loop that I5-1b already
  closed.
- **Closes.** 2.2-1, 4-36, 8-2, 8.P1-6, 8.P4-1, 8.P4-2, 8.2-R5a (7).
- **Files.** `tests/xuzz/vagent_cli_test.cpp`; `tests/fuzz/fuzz_epistemic_query.cpp` (new);
  `Makefile` (its `fuzz-target` line and `fuzz:` entry); `docs/vagent-user-guide.md`;
  `i4-validation.md` (new).
- **Depends on.** I4-10.
- **C++ and Vortex.** None.
- **Tests.** The whole I4 matrix subset; fuzzer; the measurements.
- **Gate.** ML (whole iteration), ARCH, CODE, EPI, NUM.

#### I5-2. Refutation, defeaters and a revision route

*I5. Size: about 1,300 lines.*

- **Content.** Refutation removes matching assertional support by rank excision and keeps a bounded,
  inspectable defeater (event identity, evidence, acquisition window) that suppresses replayed and
  relayed copies during the horizon; a fresh authorised observation is new evidence; a corrected
  positive verdict clears the defeater and restores support only if the observation is still current
  and eligible; re-derivation is checked against the defeater. This is the revision route for
  arbitrated predicates (X-22). Defeaters are owned structure, so the translator carries them here.
- **Closes.** 2.1-30, DF-1, DF-2, DF-3, DF-4, DF-6, 8.2-R1d, 8.2-R1e, 8.2-R2b (9).
- **Files.** `epistemic_emit_defeaters.cpp` (new); `epistemic_abi_ledger.hpp`;
  `epistemic_translate_verify.cpp`; `tests/xuzz/vagent_epistemic_defeater_test.cpp` (new).
- **Depends on.** I5-1b, I5-D.
- **C++ and Vortex.** IR.
- **Tests.** Retracting or refuting one source preserves the other; a correction never revives a
  retired reading over a newer sequence; a replay inside the horizon is suppressed; the defeater is
  bounded and survives resume and upgrade through both copiers.
- **Gate.** EPI, ML, DIST, ARCH.

#### I5-3. Feature schema, `context` and the normalizer

*I5. Size: about 1,300 lines.*

- **Content.** `context` syntax and `UnknownContextFeature`; a fixed-schema feature vector and
  normalization snapshot stored per observation; fit during warm-up and frozen per model epoch;
  clipping and a positive floor; a schema change is a typed migration or a new epoch; `features` is
  no longer always empty. The policy keys for feature names and floors are already in
  `vagent-policy-3`; if I5-D finds a key that table lacks, this slice bumps the tag to
  `vagent-policy-4` and refuses `-3` by name.
- **Closes.** 2.1-16, 3.3-1, 3.3-2, 3.3-9, 4-32, 4-38, 4-41, 4-44, 8.1-8, 8.2-R1b (10).
- **Files.** `epistemic_policy.cpp`; `parser_epistemic.cpp`, `checker_epistemic.cpp`;
  `epistemic_emit_learning.cpp` (new); `epistemic_translate_calibration.cpp`;
  `tests/xuzz/vagent_epistemic_features_test.cpp` (new).
- **Depends on.** I5-2.
- **C++ and Vortex.** Front end and IR.
- **Tests.** Two sources with different features and labels keep them apart; a schema change refuses
  or opens a new epoch; the normalizer is frozen and its bits survive resume and upgrade through
  both copiers; an unknown feature refuses statically.
- **Gate.** ML, NUM, CODE, EPI.

#### I5-4. Online SGD, warm-up and replay

*I5. Size: about 1,500 lines.*

- **Content.** The logistic model with a clamped sigmoid, `eta_n = eta0/(n+1)`, a weighted data
  gradient and independent L2; hyperparameter validation; Beta during warm-up and an untrained model
  never called calibrated; no Beta variance on logistic outputs; the checker rejects `lcb` with SGD
  and now admits `online_sgd`; replay from the ledger in first-accept order over frozen normalized
  features within fuel and cell budgets, rebuilt in transactional scratch and published with the
  label and learned state together. Replay from the ledger with no correction is **bit-equal** to
  the incremental result on one host, not "within stated error", since a tolerance would pass a real
  ordering bug (ML M5); across hosts the stored bits are authoritative and are never recomputed on
  resume or upgrade. Row 8.P6-3 ("only `beta_reputation` is admissible") stops being true here and
  its Standing check is re-run.
- **Closes.** 1-3, 2.1-15, 3.3-3, 3.3-6, 3.3-7, 3.3-8, 3.4-2, 3.4-3, 4-45, 6.3-14, 6.3-15, 6.3-16,
  8.P3-1, 8.P3-2, 8.2-2e (15).
- **Files.** new `vortex/vortex_stdlib_epistemic_sgd.cpp`; `epistemic_emit_learning.cpp`;
  `epistemic_translate_calibration.cpp`; `tests/xuzz/vagent_epistemic_sgd_test.cpp` (new).
- **Depends on.** I5-3.
- **C++ and Vortex.** IR and `std:` routines. No C++ model evaluator.
- **Tests.** The I5-D reference vectors (a hand-computed one-step case, a separable stream whose
  loss decreases); bit-equal replay on one host; a correction replays deterministically; a failure
  leaves the old adjudication and model intact; replay cost against the budget; resume and upgrade
  keep the weights bit for bit.
- **Gate.** ML, NUM, VM.

#### I5-5. Drift epochs, horizons and validation

*I5. Size: about 1,300 lines.*

- **Content.** A privileged drift percept (under a grant, a member of the I3-3a union) starts a new
  epoch that resets weights, bias, normalizer, Beta state and label count and expires pending labels
  (`ExpiredLabel`); the feedback horizon; prequential loss and calibration retained, with abstention
  by the criterion I5-D defined; calibration and validation data kept independent of selection; an
  authorised epoch reset excises the earlier epochs' entries in bounded steps (a health reset
  already uncounts them in O(1), I4-D2); same-program resume preserves model bits. The iteration
  closes here: the translator coverage script runs over everything I5 added.
- **Closes.** 2.1-40, U-8, 3.1-6, 3.3-4, 3.3-10, 3.3-12, 6.3-20 (7).
- **Files.** `epistemic_emit_learning.cpp`; `agent_executor_verify.cpp`;
  `epistemic_translate_calibration.cpp`; `tests/xuzz/vagent_epistemic_epoch_test.cpp` (new).
- **Depends on.** I5-4.
- **C++ and Vortex.** The drift grant is authorization; the rest is IR.
- **Tests.** A label across an epoch is `ExpiredLabel`; ordinary new-epoch training needs fresh
  eligible targets; a stale label cannot train a replacement; abstention when validation fails; the
  query versions mapped across a same-program resume.
- **Gate.** ML, DIST, SEC, ARCH.

#### I6-1. Native model effects and keyed completions

*I6. Size: about 1,500 lines.*

- **Content.** An opt-in staged effect carrying the observation ID; capacity-bounded asynchronous
  evaluation through the existing native effect seam; a completion posted as a feature percept with
  model identity, version, schema, observation ID and epoch; a completion applies only on an exact
  full-key match; queries read completed cached features only; a raw softmax or detector score is
  not a calibrated probability; the full inference configuration is versioned with the calibrator.
- **Closes.** 1-8, 2.1-39, 3.4-8, 3.4-9, 3.4-10, 3.4-11, 3.4-12, 4-55, 6.3-18, T-14, 8.P6-1, 8.P6-2,
  8.2-2g (13).
- **Files.** `host_bindings.{hpp,cpp}`; `agent_executor_native.cpp` (new);
  `epistemic_emit_native.cpp` (new); `epistemic_translate_learning.cpp` (new);
  `tests/xuzz/vagent_epistemic_native_test.cpp` (new).
- **Depends on.** I6-D, I5-5.
- **C++ and Vortex.** The model runtime and the effect queue are the OS and device boundary the
  rules allow in C++; the feature record and the key check are IR.
- **Tests.** A cancelled, stale or mismatched completion never overwrites features; a completion for
  a superseded observation expires; latency and memory bounds hold; a populated-state resume and
  upgrade test through both copiers.
- **Gate.** ML, DIST, SEC, ARCH, CODE.

#### I6-2. Epoch fencing, drain on upgrade and stress

*I6. Size: about 900 lines.*

- **Content.** Model, schema and incarnation changes cancel pending completions; a live upgrade
  drains or cancels native work first and never transplants flight or request tables; late
  completions expire on key mismatch; a checkpoint refuses in-flight effects.
- **Closes.** U-21, 8.2-4h, 8.2-R3a (3).
- **Files.** `agent_executor_native.cpp`; `epistemic_translate_learning.cpp`;
  `tests/xuzz/vagent_epistemic_native_test.cpp`.
- **Depends on.** I6-1.
- **C++ and Vortex.** Host cancellation and drain.
- **Tests.** Stale feature completions cannot train a replacement observation; repeated epoch
  changes under load stay bounded.
- **Gate.** DIST, ARCH, ML, SEC.

#### T2. Typed migrations and the upgrade claim

*Translator. Size: about 900 lines.*

- **Content.** Draft 1's T2 translated ten structures at once. Each structure now has its translator
  code and parity test in the slice that added it, so T2 is what remains: the typed migration or new
  epoch for a changed model, schema or incarnation; the three-pass order proved over every
  structure; the full-bound lifecycle test of 8.2-R2; the property that resume and upgrade agree,
  over every structure, modulo the differences T1-D listed; and the coverage script run once over
  all of P19. The typed refusal for a populated intention rank (O1(b)) has existed since T1; this
  slice only tests it against the full state. The claim reads: P19 state (profiles, calibration,
  journals, ledgers, tokens, epochs) survives live upgrade and resume of a supported agent; an agent
  with an active intention refuses with a named error.
- **Closes.** 2.1-23, U-4, U-5, U-6, U-10, U-11, 8.P1-3, 8.2-5b, 8.2-5c, 8.2-R2c, 8.2-R2d, 8.2-R3b
  (12).
- **Files.** `epistemic_translate.cpp`; `upgrade_schema.*`;
  `tests/xuzz/vagent_epistemic_translate_test.cpp`.
- **Depends on.** I6-2, O1.
- **C++ and Vortex.** As T1.
- **Tests.** A populated actor with every structure at its bound upgrades, rolls back, and resumes;
  old observations become ineligible after an epoch change while retained IDs and floors still
  reject replay; learned state is bit-equal.
- **Gate.** ARCH, DIST, EPI, ML, CODE.

#### F1. Guards, documents and the final spec

*Final. Size: about 800 lines plus documents.*

- **Content.** A test that fails when a new `OpcodeKind` is missing from the VQL and VPL name tables
  or the artifact validator (X-21); a test that the bounded `check` frontend is unchanged; the
  `exp`/`ln`/`sqrt` benchmark; `tools/check-p19-traceability.py` (it checks the counts and that each
  Done row cites a test or file that exists); the user guide with the aiding-update rule, the
  native-estimator contract, the gate caveat, a warrant-index wording, the cross-host sentence of
  decision D1, the rollback limit and the cross-host fork precondition of decision D2 (a checkpoint
  is resumed on one host at a time, and the operator guarantees it); spec citations refreshed and
  **spec v1.6** written with the proposed amendments of `decisions.md` marked pending ratification.
- **Closes.** 3.1-7, 3.2-11, 5.2-2, 8.P7-2, 8.2-1e, 8.2-R5b (6).
- **Files.** `tests/xuzz/vortex_scoped_evaluation_test.cpp` or a new guard test;
  `tests/xuzz/vortex_benchmark_test.cpp`; `tools/check-p19-traceability.py` (new);
  `docs/vagent-user-guide.md`; the spec.
- **Depends on.** T2.
- **C++ and Vortex.** None.
- **Tests.** The guards; the traceability checker.
- **Gate.** ARCH, CODE, EPI, ML (claims in the documents).

#### F2. Final acceptance

*Final. Size: about 200 lines plus the record.*

- **Content.** Run every suite and fuzzer headless, `make check` over the whole tree, the user-guide
  journeys; re-run the Standing checks; the whole-system round by every persona; write
  `final-validation.md` against section 8 of this plan; set the README to Accepted.
- **Closes.** 1-10, 8.2-0a, 9-5 (3).
- **Files.** `final-validation.md` (new); `traceability.md`; `README.md`.
- **Depends on.** F1.
- **C++ and Vortex.** None.
- **Tests.** Everything.
- **Gate.** All personas; ML whole system.

## 5. Review gates

**Superseded for scheduling** by [decisions.md section 0.1](decisions.md) (2026-11-04): reviews run
once per milestone with at most three personas and no confirmation rounds. The table below still
names the concerns each slice raises; the milestone reviewers cover them.

Personas: **ML** machine-learning expert; **NUM** numerics; **ARCH** architect; **DIST** distributed
systems; **CODE** codebase and compiler; **VM** Vortex and VM; **EPI** epistemology; **SENS**
sensor-fault and hardware; **SEC** security. The README rule holds: the ML expert reads the design
before the implementation and the source, tests, mathematics and lifecycle before acceptance, for
everything touching scoring, priors, decay, pooling, gates, fusion, features, labels, learning,
replay or native completions. ARCH and DIST read cross-cutting lifecycle, turn, transport and
upgrade work. No reviewer implements what they review. "ML (named items)" is a short section of the
other reviewers' round, not a full review: the ML plan review (minor 1) found items in five slices
that touch learning without being learning slices (I3-2c2, I4-5, I4-7a, I4-10, I3-4b1), and the
user-guide text of W1 and W2 gets the same short read (ML round-2 minor 9).

| Slice          | Reviewers                                                                 | ML gate     |
| -------------- | ------------------------------------------------------------------------- | ----------- |
| E0             | ARCH, EPI read the records against the code                               | no          |
| T0 (done)      | ARCH, DIST, CODE. Round 1 FAIL, round 2 PASS                              | no          |
| E1             | CODE, ARCH. The review is of the move, not the logic                      | no          |
| I3-2a          | ML (design and implementation), SENS, CODE, NUM                           | yes         |
| I3-2b          | ARCH, DIST, CODE, VM                                                      | no          |
| I3-2c1         | SEC, DIST, ARCH, CODE                                                     | no          |
| I3-2c2         | ARCH, DIST, EPI, SENS, CODE, ML (named items)                             | named items |
| T1             | ARCH, DIST, CODE, EPI, SEC                                                | no          |
| I3-3a          | SEC, DIST, ARCH, CODE                                                     | no          |
| I3-3b          | SENS, DIST, ARCH, CODE, ML                                                | yes         |
| I3-3c          | SENS, DIST, ARCH, EPI, ML                                                 | yes         |
| I3-3d          | SEC, SENS, EPI                                                            | no          |
| I4-1a          | ML, NUM, ARCH, CODE                                                       | yes         |
| I4-1b          | ML, NUM, ARCH, CODE, EPI, DIST                                            | yes         |
| I4-2           | ML, NUM, VM                                                               | yes         |
| I5-1a          | ML, EPI, NUM, DIST                                                        | yes         |
| I4-3           | ML, NUM, EPI                                                              | yes         |
| I4-4           | ML, SEC, ARCH, EPI                                                        | yes         |
| I4-5           | CODE, EPI, VM, ML (named items)                                           | named items |
| I4-6           | CODE, VM, ARCH, EPI, ML                                                   | yes         |
| I4-7a          | VM, CODE, ARCH, EPI, ML (named items)                                     | named items |
| I4-7b          | VM, CODE, ARCH, DIST, EPI                                                 | no          |
| W1             | SEC, DIST, CODE, ARCH, ML (named items: the guide text only)              | named items |
| I3-4a          | SEC, EPI, DIST, CODE, ML                                                  | yes         |
| I3-4b1         | DIST, SEC, ARCH, CODE, ML (named items) (one round with I3-4b2 and I3-4c) | named items |
| I3-4b2         | DIST, SEC, ARCH, CODE                                                     | no          |
| I3-4c          | DIST, ARCH, CODE, EPI, ML                                                 | yes         |
| I5-1b          | ML, EPI, NUM, DIST                                                        | yes         |
| W2             | SEC, DIST, EPI, ML (named items: the guide text only)                     | named items |
| I4-8           | ML, SENS, NUM, EPI, SEC                                                   | yes         |
| I4-9           | ML, NUM, SENS, EPI, VM                                                    | yes         |
| I4-10          | SEC, EPI, CODE, DIST, ML (named items)                                    | named items |
| I4-11          | ML (whole iteration), ARCH, CODE, EPI, NUM                                | yes         |
| I5-2           | EPI, ML, DIST, ARCH                                                       | yes         |
| I5-3           | ML, NUM, CODE, EPI                                                        | yes         |
| I5-4           | ML, NUM, VM                                                               | yes         |
| I5-5           | ML, DIST, SEC, ARCH                                                       | yes         |
| I6-1           | ML, DIST, SEC, ARCH, CODE                                                 | yes         |
| I6-2           | DIST, ARCH, ML, SEC                                                       | yes         |
| T2             | ARCH, DIST, EPI, ML, CODE                                                 | yes         |
| F1             | ARCH, CODE, EPI, ML (claims in the documents)                             | yes         |
| F2             | All personas; ML whole system                                             | yes         |
| I3-2D (design) | ML, ARCH, DIST, EPI, SENS, SEC, CODE                                      | yes         |
| T1-D (design)  | ARCH, DIST, CODE, EPI, SEC                                                | no          |
| I3-3D (design) | SENS, DIST, SEC, ARCH, ML                                                 | yes         |
| I4-D1 (design) | ML, NUM, EPI, SENS (reviews the reference program as an artefact)         | yes         |
| I4-D2 (design) | ARCH, CODE, VM, DIST, EPI, ML                                             | yes         |
| V-D (design)   | DIST, SEC, EPI, ML, ARCH                                                  | yes         |
| I5-D (design)  | ML, NUM, EPI, DIST                                                        | yes         |
| I6-D (design)  | ML, DIST, SEC, ARCH                                                       | yes         |

Procedure per gated unit: round 1 with all named personas in parallel; a failed round is fixed and
re-reviewed by the personas that failed it, plus any whose area the fix touched; a unit that fails
three rounds returns to design. I3-4b1, I3-4b2 and I3-4c share reviewers and one executor path and
may be reviewed in one round once V-D is accepted (distributed, process economy). The final F2
review is a whole-system round by every persona.

## 6. Risks and their controls

| Risk                                                                                                                                                                                                  | Control                                                                                                                                                                                                                                                                                                                                                                                           |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `compiler.cpp` is 7,617 lines and its `Gen` class is one type of about 7,100 lines in an anonymous namespace; `agent_executor.cpp` is 5,694; `runtime_abi.hpp` is edited by about 15 slices           | E1 moves the declaration to a private header; new code lives in per-area files (section 4.2) as free emitter functions; the 400-line growth rule covers every large file that exists at E1 and the 3,000-line cap covers every area file                                                                                                                                                          |
| E1's diff is large and other agents edit these files                                                                                                                                                  | E1 runs alone, after T0 (done); the lead rebases before and after; its review is mechanical (moved-line check by tool, byte-equal emitted IR)                                                                                                                                                                                                                                                     |
| Turn and fuel budgets: ingestion is about 4.4k cycles; a reset is O(K·H) with a settle per support; a query runs `Exp` and `Ln` per candidate                                                         | Budget parity rule; each slice records worst-case cycles for default bounds against the instruction grant; `max_candidates` is enforced in I4-6; reset is measured before I3-2c1 lands                                                                                                                                                                                                            |
| Cell budget: the ceiling is 24,576 cells, a record costs about 80, and the default holds about 18 fully active principals; calibration, groups, tokens, journals, ledgers and features all draw on it | I4-D2 prices every structure (the entry from its field list, confirmed by a throwaway fixture) and sets the per-(source, domain, epoch) label cap and the ledger cap, because Beta is recomputed from the current-epoch ledger and a source at its cap cannot learn again until a reset opens a new epoch (ML M3, P2-4, P2-5); the ceiling is re-measured at each slice; no slice prunes silently |
| Checkpoint limit: `vagent.value_operations` is 65,536 and 24k cells checkpoint while 32k do not                                                                                                       | `AFullEpistemicBudgetStillCheckpoints` is re-measured by each slice with its structure at the bound; the translator's peak copy (old plus new graph) is priced in T1                                                                                                                                                                                                                              |
| Drift the gates cannot see: common-mode drift, drift slower than the gate tolerance, and cold start with no reference                                                                                 | I4-8 never marks a reading `ACCEPTED` without a host-supplied reference; fusion at cold start returns no calibrated value; tests ramp one sensor below the gate and move all sensors together; I5-5 handles declared drift epochs; F1 documents the limit (7-9)                                                                                                                                   |
| A healthy sensor is latched FAULTED by a run of gate rejections (five in a row at p = 0.05 and 100 Hz: about every 9.4 hours)                                                                         | I4-D1 writes one rule under O10: an event-indexed fraction with a minimum event count for both SUSPECT and FAULTED, or a streak with a window; the accepted false-latch rate per device-year is proved by a bound, with simulation as a smoke test only                                                                                                                                           |
| `lcb` at an uninformative prior makes a fresh source vote against its own claim (three fresh independent sources with c = 1 pool to 0.0189)                                                           | O9: I4-D1 settles with the ML reviewer; I4-2 carries the test; a change to the spec's formula is a proposed amendment                                                                                                                                                                                                                                                                             |
| Floating-point results differ across hosts (libm) while spec 3.3 asks for deterministic replay and a checkpoint can move hosts                                                                        | The contract of decision 1 (section 7): stored bits are authoritative and never recomputed on resume or upgrade; same-host replay is bit-equal; reference vectors come from an independent high-precision program; every decision vector carries its margin; no test pins a boundary count; the user guide says so                                                                                |
| Stale-state hazards: a checkpoint is a point-in-time copy, so a verification acknowledged after it is lost on rollback, and two resumes of one checkpoint fork the journal                            | Decision D2 (section 7.2): I3-4c refuses a second live resume of one checkpoint in one executor and tests a rollback; the cross-host fork precondition and the rollback loss are stated in the user guide and in F2                                                                                                                                                                               |
| A health reset removes learning and the next label brings the old learning back, because Beta is recomputed from the whole ledger                                                                     | I4-D2 states the reset rule (P2-4): a reset opens a new calibration epoch in O(1), the recompute reads only current-epoch entries, a pending label for the old epoch is `ExpiredLabel`; I5-1a tests reset-then-label equals prior plus the new label, and row 5.1-13 closes there                                                                                                                 |
| Resume and upgrade refusals have no typed vehicle (`ActorError` is a closed enum; the resume check runs after `resume` returns a pid)                                                                 | P2-1: T1-D designs one checked pre-flight and T1 implements it; every later refusal cites it and tests the typed value                                                                                                                                                                                                                                                                            |
| A retry of a lost verification looks "below the floor" once a later event was accepted                                                                                                                | P2-2: the floor is an issuer-declared low-watermark with a bounded window and a retryable `VerificationWindowFull`; I3-4b1 tests the retry-after-later-commit row                                                                                                                                                                                                                                 |
| Intermediate states: until I5-2 an arbitrated predicate has no revision route                                                                                                                         | No production caller issues a verify grant before F2; the processing turn applies real labels from I3-4c on, so there is no `RecordedNotApplied` state; the revision route ships at I5-2                                                                                                                                                                                                          |
| Policy encoding changes alter every policy hash, which the upgrade schema compares                                                                                                                    | One bump per change (`vagent-policy-3` at I4-1a; a further bump only if I5-D finds a missing key), old readers deleted, tests assert the tag and refuse the previous one by name                                                                                                                                                                                                                  |
| An owned record changes shape between slices and an old checkpoint resumes into routines that read other field names                                                                                  | The shape version of section 2 (T1), checked by number at resume and upgrade on the typed refusal channel; a slice that changes a shape bumps it. Before T1 no slice claims an upgrade path for populated state                                                                                                                                                                                   |
| Spec ambiguities that no code can decide (`t_star`, the trusted reference, profile variance)                                                                                                          | O6: I4-D1 settles `t_star` and the reference grant; dropping the profile-variance path is a proposed amendment, since it changes a spec sentence                                                                                                                                                                                                                                                  |
| Code drifts from the spec and the records (34 foreign commits already touched P19 files)                                                                                                              | E0 records the baseline; each slice starts with `git log <last accepted>..HEAD` over the P19 files and records any foreign change; the matrix is updated in each acceptance commit; F1's checker fails if a Done row cites a test or file that no longer exists                                                                                                                                   |
| Review load: eight designs and 42 slices                                                                                                                                                              | Designs first and in parallel where section 4.1 allows; at most three rounds per unit; parallel persona rounds; limited ML reads for five slices; one round for I3-4b1 to I3-4c                                                                                                                                                                                                                   |

## 7. Decisions

### 7.1 Ruled by the lead (decisions.md section 2)

| #     | Decision                                                 | Ruling, and where this plan applies it                                                                                                                                                                                                                                                                                                                                                                |
| ----- | -------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| O1    | What the live-upgrade claim covers                       | (b) Translate P19 state for supported agents and refuse an active intention with a typed, tested error. This claims P19 upgrade support before the full P17 pipeline, which spec 8.2 gate 0 reserves, so it is a lead ruling within the spec **plus proposed amendment A5** (architect plan review M2). Applied in T1 (the refusal), T2, F1 and section 8.                                            |
| O2    | Where scoring, pooling, clustering and SGD live          | The spec's file list: graph construction in `vortex_stdlib`, since every actor's `WorkerContext` already bootstraps `std:math`. I4-D2 settles the mechanism and its bootstrap cost. Applied in I4-2, I4-3, I4-8, I4-9, I5-4.                                                                                                                                                                          |
| O3    | Where query tokens hang                                  | I4-D2 settles. Applied as: ranked continuations in the activation scope (I4-7a); verification tokens at the commit point on the agent's `d.epistemic` with a retention bound counted in turns or state-version steps (I4-7b). A proposed amendment to 2.1.                                                                                                                                            |
| O4    | Scope of the signed-ingress adapter                      | I3-3D settles. Applied as an in-tree reference adapter with audience binding and domain separation (I3-3a), reaching a shipped binary at W1.                                                                                                                                                                                                                                                          |
| O5    | The spec amendment bundle                                | Amendments accumulate in `decisions.md` section 3; F1 writes spec v1.6 with them pending. E0 is the record.                                                                                                                                                                                                                                                                                           |
| O6    | Consensus inputs (`t_star`, reference, profile variance) | I4-D1 settles with the ML, numerics and epistemology reviewers.                                                                                                                                                                                                                                                                                                                                       |
| O7    | Cell and checkpoint budget                               | **I4-D2** prices every structure and sets the label and ledger caps (P2-5); re-measured at every slice. (Draft 2 said I4-D1, which reads neither the entry's fields nor the record layout.)                                                                                                                                                                                                           |
| O8    | Visibility of `clear` and `reset` outcomes               | Per decision F28 (a bounded arena audit list with class eviction, plus the host log).                                                                                                                                                                                                                                                                                                                 |
| O9    | `lcb` at an uninformative prior                          | I4-D1 settles with the ML reviewer. A change to the spec formula is a proposed amendment. I4-D1 also settles whether `score_tolerance` is absolute in W-space (it saturates at a logit sum of about 20.7) or in logit space.                                                                                                                                                                          |
| O10   | Gate rejections: streak or fraction                      | I4-D1 settles with the ML and SENS reviewers, including the accepted false-latch rate. Amendment A2 in `decisions.md` says gate rejections "are I4-D's (O10)"; this plan implements no streak threshold in I3-2 and treats the rule as open until I4-D1 rules. A change to the spec default is a proposed amendment.                                                                                  |
| E1    | The size of the code-generator move                      | Minimal: a private header for `Gen`; no bulk move. Section 4.2 states the boundary.                                                                                                                                                                                                                                                                                                                   |
| T0    | How far the upgrade fix goes                             | Share the guards across both entry points. Done (`4a912c69`). Collapsing the two state copiers is T1-D's.                                                                                                                                                                                                                                                                                             |
| D1    | Meaning of "deterministic" across hosts                  | Replay is bit-equal on one host; across hosts results agree within a stated tolerance, and decisions near a threshold carry a declared margin. Applied in section 2 (numeric decisions), I4-D1, I5-4 and F1.                                                                                                                                                                                          |
| D2    | Fork and rollback of a verification journal              | I3-4c refuses a second live resume of one checkpoint in one executor with a typed error. Cross-host fork prevention is an operator precondition, stated in the user guide and the final limits, not implemented. An issuer restored from a checkpoint takes a fresh incarnation (A3 for issuers). Rollback of the receiver is stated as a limit (option (c) below). Applied in I3-4b2, I3-4c, F1, F2. |
| D3    | Source bindings after an upgrade                         | They keep the policy they were bound under; T1-D decides whether a policy change rebinds, refuses or migrates them.                                                                                                                                                                                                                                                                                   |
| P2-1  | Typed refusals                                           | T1-D designs one channel (a checked pre-flight returning the typed reason and offending numbers before `runtime_.resume`), T1 implements it and fixes X-9; later slices cite it. Applied in section 2, T0 (what moved out), T1-D, T1, I4-1a, I3-4c.                                                                                                                                                   |
| P2-2  | Verification replay floor                                | An issuer-declared low-watermark, a bounded window with a retryable `VerificationWindowFull`, a monotone issuer incarnation, floor state created only when intake commits, and the common signed header fixed by I3-3D. Applied in I3-3D, V-D, I3-3a, I3-4b1, I3-4b2.                                                                                                                                 |
| P2-3  | Crash-point matrix                                       | V-D defines the two fault models and the expected state per crash point and per D2 option; upgrade rows are refused-not-quiescent or state-carried. Applied in V-D and I3-4c.                                                                                                                                                                                                                         |
| P2-4  | Reset and the label ledger                               | A reset opens a new calibration epoch in O(1); the recompute reads only current-epoch entries; older entries leave the cap; a pending label for the old epoch gets `ExpiredLabel`. I4-D2 states it, I4-1b opens the epoch, I5-1a tests it and closes row 5.1-13.                                                                                                                                      |
| P2-5  | The ledger cap                                           | One owner, I4-D2: the cap per (source, domain, epoch), its default, the entry's cell price computed from its fields and confirmed by a throwaway fixture, the minimum labels per source, and the checkpoint interaction.                                                                                                                                                                              |
| R3-11 | Version cells                                            | No shape-version or state-version cell in I3-2. The shape version and the guard `hasTranslatableEpistemicState` are T1's; the epistemic-state version is born in I4-1b, with its first writer (section 7.3, assumption 5).                                                                                                                                                                            |

### 7.2 Decisions 1 and 2, as ruled

No decision is open for the lead. They are kept here because the slices cite their options.

**Decision 1 (D1). What "deterministic" means across hosts.** Spec 3.3 and 6.3 say replay is
deterministic and never say whether the checkpoint can move hosts. The VM calls the host `std::exp`
and `std::log` (`vortex_vm.cpp:939-941`), and decisions that sit on a threshold flip with a one-ulp
difference (the recovery count of I3-2a is 64, 64 or 65 for the same history). Ruled: **same-host
bit-equal, cross-host within a stated tolerance.** Stored bits are authoritative and never
recomputed on resume or upgrade; replay on one host is bit-equal; golden vectors carry margins; the
user guide states that a decision within about 1e-12 of its boundary may differ between hosts. Two
options were rejected: a bit-exact `Exp` and `Ln` owned by the VM (it changes accepted I1 work), and
a single-host-only checkpoint.

**Decision 2 (D2). Durability of an acknowledged verification.** A checkpoint is a point-in-time
copy. If host A commits verification V after the checkpoint, sends the outcome and the issuer drops
its pending record, a failover to the older checkpoint on host B has no journal entry for V and the
issuer never retransmits: the label is lost under a committed-looking acknowledgement. Resuming one
checkpoint twice yields two live actors with separate journals, and an issuer restored from a
checkpoint resends from a regressed counter. The journal, the pending records and the outcome checks
are keyed by a stable agent identity in the arena, not by pid. Ruled:

1. A second live resume of one checkpoint in one executor is refused with a typed error (I3-4c).
1. Cross-host fork prevention is an operator precondition, stated in the user guide and the final
   limits. A lease without fencing is unsafe across a partition (host A may stay alive while host B
   resumes), and nothing in the tree implements one, so none is claimed.
1. An issuer restored from a checkpoint takes a fresh incarnation (I3-4b2).
1. The receiver's rollback is option (c), a stated limit, with the options priced as follows:

| Option                                                    | Cost                                                                                                                                                                                                                                                                                                                                   |
| --------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| (a) Release the outcome only after a checkpoint covers it | A host-driven checkpoint on the verification path (only a host can call `checkpoint(pid)`), a cached-copy release rule, and the staged response kept in the journal and not the outbox, because `checkpoint` refuses while an outbox entry or an unanswered send exists (`agent_executor.cpp:4937-4956`). Heavy at a full cell budget. |
| (b) A lineage generation on the outcome                   | A receiver-to-issuer report channel, a bounded issuer-side retention rule and another message; the issuer ignores outcomes from a lower generation. Chosen only if cross-host failover must be claimed.                                                                                                                                |
| (c) State the limit                                       | A rollback to an older checkpoint loses verifications acknowledged since, together with their journal entry, cached outcome and label, so the state stays self-consistent. F2 records it; I3-4c tests exactly what is lost.                                                                                                            |

### 7.3 Assumptions this draft took, for the reviewers to contest

- The verification event ID is (issuer, incarnation, sequence) carried in the signed header, with an
  issuer-declared low-watermark. The spec asks only for a globally unique ID from an authenticated
  issuer.
- Isolate and degrade may overtake queued readings; a reset is decided by the in-flight snapshot
  (R3-6) against readings of its own source. I3-2D may reverse this with a test of both outcomes.
- The adapter's signature stays in the application payload, so no mesh wire change is needed and the
  adapter choice remains a lead decision.
- W1 and W2 are in the plan. If the lead rules that CLI integration is outside P19, both are deleted
  and section 8 item 5 reads as stated there.
- **The epistemic-state version is born in I4-1b, not in I4-7a.** R3-11 says "with its first
  consumer". The first reader is I4-7a (`StaleEpistemicQuery`), but the first writer after I3-2 is
  I4-1b (calibration reset), and I5-1a's apply routine bumps it before I4-7a exists. Creating the
  cell at the first writer avoids retrofitting a bump into accepted ledger code. The retrofit of the
  I3-2 health writers is unavoidable either way, and a writer-enumeration test guards it.
- **The I3-2c numbering follows `i3-2-design.md` revision 3.** Draft 2 had 2c1 as the records and
  health turn and 2c2 as the grants; the design and decisions.md (a stub turn in 2c1, the real one
  in 2c2) have them the other way round. The matrix targets moved with the content (eight rows).

## 8. Definition of done for P19

F2 may say P19 is implemented when all of the following hold, each with evidence in the final
validation record.

1. **Matrix.** Every row is Done, Deferred by spec, Informational, or Standing with its check re-run
   at F2. No row is Partial, Missing or Designed. `tools/check-p19-traceability.py` passes.
1. **Gates.** Each numbered gate and each regression bullet of spec 8.2 (rows `8.2-0` to `8.2-Z`) is
   Done, with the test that satisfies it named. Gate 0 is met by lead ruling O1(b) plus proposed
   amendment A5, pending the owner's ratification: P19 upgrade support is claimed for agents without
   an active intention, by quiescent upgrade only, and the final record does not claim full vagent
   upgrade support, which 8.2 reserves for after the P17 prerequisite.
1. **Suites.** `vagent_runtime_test`, `vagent_actor_test`, `xuzz_test` and `make test` pass headless
   (the three gtest `[  PASSED  ]` lines are the evidence where the swarm netns step lacks the
   `veth` module); the fuzz targets, including the new query, verification and adapter fuzzers, run
   a fixed iteration count without a finding; `make check` is clean on the whole tree, or on the
   full diff from `3ae8d384` with the baseline findings listed.
1. **ML.** Every slice marked ML has a PASS report with no open BLOCKING or MAJOR finding, naming
   the claims it passes and the claims it reserves, and a whole-system ML review of I4, I5 and I6
   together has passed. The reference vectors are reproducible from `tools/` with one command, and
   the reference program was reviewed as a design artefact before any IR existed.
1. **A shipped path.** A shipped binary (`vagent run`) ingests a signed reading, answers `truest`
   and `ranked` queries, and applies a verification (W1, W2), each as a headless test. If the lead
   rules W1 and W2 out, the final record says instead that P19 is delivered as a runtime and library
   capability and that CLI integration is outside this plan.
1. **Journeys.** The user-guide worked examples run headless as tests: a fleet read with `truest`,
   `ranked` and `consensus`; a fault, an operator isolate and reset; a verification with redelivery
   and a lost reply; a live upgrade of a populated actor; a checkpoint and resume at a full budget.
1. **Budgets.** The validation record gives the measured worst case for ingestion, a query at
   `max_candidates`, a reset, a replay and a checkpoint, each against its limit.
1. **Documents.** Spec v1.6 carries the proposed amendments marked pending ratification and a
   truthful baseline; README says Accepted; the user guide carries a complete policy example, the
   cross-host sentence and no unsupported certainty claim.
1. **Limits stated.** The final record lists what P19 does not claim: the Deferred rows (exact
   probabilities, matrix and correlated fusion, derived-rule arbitration, bandits, automatic
   change-point detection), unattended drift that no gate can see, the rollback limit and the
   cross-host fork precondition of decision D2, and the upgrade boundary. The F2 claim reads: **P19
   is implemented for supported agents (no active intention, no native skill state), quiescent
   upgrade only, deferred rows as listed.**

## 9. Round-2 findings and their resolution

The round-1 findings (22 MAJOR across three reviews) and their changes are mapped in draft 2, commit
`a318eb3c`. The three round-2 reviews ([plan-review/](plan-review/), `review-*-r2.md`) are all FAIL
with no BLOCKING finding: architect 3 MAJOR and 10 MINOR, distributed 3 MAJOR and 7 MINOR, ML 2
MAJOR and 11 MINOR. "Recorded" means the finding is accepted as stated and the reason for not
changing the plan further is given. Lead rulings are cited by their `decisions.md` identifiers.

### 9.1 Architect (3 MAJOR, 10 MINOR)

| Finding | Resolution                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| ------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1      | P2-1. T1-D designs one refusal channel (a checked pre-flight returning the typed reason and offending numbers before `runtime_.resume`, with the files and callers it touches); T1 implements it, fixes X-9 and types the intention-rank refusal; every later refusal cites it (section 2, T0 "what moved out", T1-D row, T1, I4-1a, I3-4c). I3-2 adds no refusal.                                                                                         |
| M2      | O1 is a lead ruling within the spec plus proposed amendment A5 (already in `decisions.md` section 3). Sections 3 E, 7.1 O1 and 8 items 2 and 9 are reworded; F1 writes A5 into spec v1.6 as pending.                                                                                                                                                                                                                                                       |
| M3      | R3-11. No header cell in I3-2. T1 mints the shape version only for an agent that declares an arbitrated predicate, adds the one guard `hasTranslatableEpistemicState` that ignores the header and depends on a registered translator, compares the saved header by number and never copies it, and tests that a non-arbitrating and an arbitrating-without-profile agent still upgrade. The state counter is not carried (section 2); it is born in I4-1b. |
| m1      | Recorded for E1, which is in flight as specified: "no out-of-class body moves", `inline` for non-template free functions, and a later header edit of a few lines per area instead of a `struct` with a state slot, which would change moved code (E1 status note).                                                                                                                                                                                         |
| m2      | E0 refreshes the citation (`:5081` before T0, `:5123` after).                                                                                                                                                                                                                                                                                                                                                                                              |
| m3      | T1-D defines the coverage oracle (per-area ABI headers export owned dimensions and record kinds, tests register what they cover, the script diffs and greps `ensureDimension` names); T1 writes the script.                                                                                                                                                                                                                                                |
| m4      | T1-D row and T1: no public entry point accepts a state another refuses (`LocalHost::upgrade(pid, CompiledProgram)` forwards or installs the translator); the operator path to quiesce; each parity test names the upgrade road it drives (section 2).                                                                                                                                                                                                      |
| m5      | I5-1a is "the only writer after creation"; a reset reaches alpha and beta through its recompute, with a test (I4-1b, I5-1a).                                                                                                                                                                                                                                                                                                                               |
| m6      | I3-3c and I3-3d gain the populated-state resume and upgrade lines and their translator and `restoreState` files.                                                                                                                                                                                                                                                                                                                                           |
| m7      | I3-3a, I3-4c and I4-11 name the `Makefile` fuzz lines; section 2 says the bare-`WorkerContext` tests run in `xuzz_test` and `vagent_*` tests in `vagent_actor_test`.                                                                                                                                                                                                                                                                                       |
| m8      | I3-3D decides the carrier of W1's alias table; W1 depends on it and carries it as a design section, since W1 is a security decision of 900 lines.                                                                                                                                                                                                                                                                                                          |
| m9      | I4-7b drops the verifier-requested token unless I4-D2 defines a message for it.                                                                                                                                                                                                                                                                                                                                                                            |
| m10     | Recorded, not adopted. Running the pure-routine slices in a second worktree beside I3-3 needs a merge procedure the lead has not ruled, and those slices total about 6,800 of 47,800 lines. The lead can relax "strictly sequential" at I4-D2 acceptance.                                                                                                                                                                                                  |

### 9.2 ML (2 MAJOR, 11 MINOR)

| Finding | Resolution                                                                                                                                                                                                                                                                                                                             |
| ------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1      | P2-4. I4-D2 states the reset rule (new calibration epoch in O(1), current-epoch recompute, older entries off the cap, `ExpiredLabel`); I4-1b opens the epoch and names `epistemic_emit_health.cpp`; I5-1a tests reset-then-label, the 16-record budget and the pre-reset observation, and closes 5.1-13 (matrix chain I4-1b to I5-1a). |
| M2      | P2-5. I4-D2 is the one owner of the cap and the cell price; the cap and price leave I4-D1's scope; 7.1 O7, the risk table, I4-1b, I5-1a and I4-11 say I4-D2. The design computes the price from the entry's fields and confirms it with a throwaway fixture, and states the minimum labels per source and the checkpoint interaction.  |
| m1      | Section 9.2 of draft 2 was wrong; `decisions.md` has no profile-variance amendment and 5.2-6 stays Missing, closed by I4-8. I4-D1 rules on it (O6), and a change is then a proposed amendment and a matrix edit.                                                                                                                       |
| m2      | 7.1 O10 now quotes A2 correctly. The three streak sentences in `i3-2-design.md` (`:195`, `:975`, `:1111`) are an I3-2D revision item (I3-2D row); this plan files no edit to the design.                                                                                                                                               |
| m3      | I4-D1 states the accepted false-latch rate per device-year and proves it by a bound; I4-8 and the risk table treat simulation as a smoke test (the bound figures are in the I4-D1 row).                                                                                                                                                |
| m4      | Section 2 states the bump rule (derived-state change and any action, never `R` or `N` drift); I3-2c2 lists the writers; I4-1b tests it.                                                                                                                                                                                                |
| m5      | I3-2a emits the estimator routine parameterised by its two state cells, decay factor and floor; I4-8 reuses it.                                                                                                                                                                                                                        |
| m6      | Section 2 and the I4-D2 row: where the summation loop lives and how a bare-`WorkerContext` test feeds it.                                                                                                                                                                                                                              |
| m7      | I4-D2 fixes the abstention criterion's shape so the key names in I4-1a are right the first time; I5-D fixes the numbers.                                                                                                                                                                                                               |
| m8      | I4-D1 row: hex-float inputs, 50-digit decimal outputs, allowed error per routine, a grep test against `math` and `numpy`.                                                                                                                                                                                                              |
| m9      | W1 and W2 get a named-items ML read of the guide text (section 5).                                                                                                                                                                                                                                                                     |
| m10     | T1-D row and section 2: doubles compare by bit pattern.                                                                                                                                                                                                                                                                                |
| m11     | I4-1a: the checker refuses an SGD-only key under any other model with a typed error, tested.                                                                                                                                                                                                                                           |

### 9.3 Distributed systems (3 MAJOR, 7 MINOR)

| Finding | Resolution                                                                                                                                                                                                                                                                                                                                       |
| ------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| M1      | P2-2. The floor is an issuer-declared low-watermark in the common signed header (I3-3D, I3-3a), with a bounded window, a retryable `VerificationWindowFull`, a monotone issuer incarnation and floor state created only at intake commit (V-D, I3-4b1, I3-4b2). The row that fails under a highest-seen floor is a named test.                   |
| M2      | D2 as revised. I3-4c refuses a second live resume of one checkpoint in one executor with a typed error and tests it; cross-host fork prevention is an operator precondition in the user guide, F1 and F2; the option rows are corrected and the issuer-rollback case is added (section 7.2); I3-4b2 gives a restored issuer a fresh incarnation. |
| M3      | P2-3. V-D defines the two fault models and the expected state per row and per D2 option; I3-4c's matrix runs under them, with "refused as not quiescent" and "state carried" rows for upgrade.                                                                                                                                                   |
| m1      | I3-4c names the trigger: a scan of the arena queue at the end of `upgrade` and at the first turn after `resume`; `needsFirstTurn_` works for resume only; a test covers an idle upgraded actor with a queued entry.                                                                                                                              |
| m2      | I3-2c1 states the precedence: the in-flight snapshot decides for a reset against readings of its own source; control-first applies to everything else, `putBack` included; both tests stay.                                                                                                                                                      |
| m3      | Section 2 and I3-4b1: per-issuer and per-source state is created only when the event commits.                                                                                                                                                                                                                                                    |
| m4      | T1-D row and T1 tests: the mesh mailbox as a third queue; intake handling is program-independent; I3-4b1 tests an upgrade with a verification queued.                                                                                                                                                                                            |
| m5      | I4-7b, I4-D2 and I3-4b2: retention counted in turns or state-version steps, eviction never precedes expiry, a typed refusal at the cap, the pending life setting and the `retention >= pending life + retransmit bound` check at settings load; I4-7b tests only the setting.                                                                    |
| m6      | Section 2, I3-2c1 and T1: which host state an in-process upgrade keeps (the pid survives it) and which a resume loses, each tested, and the user guide says so.                                                                                                                                                                                  |
| m7      | I3-3D states what carries `relayedBy` in the CLI; W1 uses it or says relay is unsupported there; W2 uses the V-D fault-injection hook for "a reply is lost".                                                                                                                                                                                     |

### 9.4 Other changes in draft 3

- T0 is recorded as built (`4a912c69`). Four items that draft 2 listed under it are re-assigned: X-9
  and the typed intention refusal to T1, X-7 and X-2 to I3-2a (F30 keeps the whole-block fallback).
- The I3-2c slices are renumbered to the design's, and eight matrix targets moved with the content.
- Section 7.2 records D1 and D2 as ruled. Section 7.1 gains D1 to D3, P2-1 to P2-5 and R3-11.
- The traceability baseline is `3b86b36d`. Open rows are still 354, each closed by one slice; the I4
  group closes 163 and the I5 group 60, because 5.1-13 moved to I5-1a.

## 10. For the round-3 reviewers

- **Architect.** Does T1 now carry the refusal channel, the shape version and the single guard
  without making any agent populated (M1, M3)? Is the state version well placed in I4-1b, with the
  I3-2 writers retrofitted and guarded by an enumeration test (section 7.3)? Is the I3-2c
  renumbering to the design's acceptable, and are the eight moved rows right? Is T1 at about 1,500
  lines still one slice?
- **ML expert.** Is the reset rule of P2-4 enough, and does I5-1a's recompute from the current epoch
  keep the single-writer claim? Does I4-D2 hold the right inputs to price the cap (P2-5)? Does
  I4-1b's creation-time write of alpha and beta stay inside "the only writer after creation"?
- **Distributed-systems expert.** Is the issuer-declared watermark with a bounded window and
  `VerificationWindowFull` enough for retry and loss (P2-2)? Do D2's local refusal, operator
  precondition and fresh issuer incarnation cover resume, rollback and issuer rollback? Do V-D's two
  fault models and I3-4c's matrix leave any crash point undefined? Is the trigger scan right for
  upgrade?
