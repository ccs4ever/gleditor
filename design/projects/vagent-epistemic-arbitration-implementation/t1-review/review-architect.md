# T1 design review: ARCH

Verdict: FAIL (3 MAJOR, 5 MINOR). Checked against HEAD 49149e4b.

## MAJOR

### M1. Every `agent_executor.cpp` citation is stale; the I3-2 premise is false

- Where: header paragraph (lines 7-11); all of sections 2.1, 2.3, 3.2, 4.1, 5.1, 6.
- Failure: the cited lines moved about 370-390 lines when I3-2 landed. `upgradeRefusal` is at
  `agent_executor.cpp:5449` (design 5069), `restoreState` at `:5049` (design 4680), the tail of
  `resume` at `:5440` (design 5060), the root test at `:5468-5478`, the reconciler call site at
  `:5843`, and `LocalHost::upgrade` at `program.cpp:383,405`. An implementer following `path:line`
  edits the wrong place. The premise "I3-2 is not implemented" is false, so section 7's "from the
  design, confirmed by the slice's code" rows are now checkable and must be checked.
- Fix: re-pin every citation at 49149e4b. Replace the health rows of section 7 and the table in 5.4
  with the shipped names. Constants are in `epistemic_abi_health.hpp` (`kProfileResetEpoch`,
  `kProfileActionSeq`, `kProfileFaultSeq`, `kProfileReasons`, `kProfileHealthCells`,
  `kProfileActions`), not in `runtime_abi.hpp`.

### M2. The quiescence report and the admission gate do not cover the I3-2c1 host state

- Where: section 5.1, 5.2, 5.4.
- Failure: I3-2c1 put a second queue and second lock beside the ones the design models.
  `AgentExecutor::pendingHealth_`, `healthOutcomeRing_` and `healthActionSeq_` sit under
  `healthMutex_` (`agent_executor.hpp:871-877`). `healthAction` takes `healthMutex_` at
  `agent_executor.cpp:4094` and `ingress_->mutex` separately (`:4091,4115`). Section 5.1's `posts`
  is `PostBox::pendingFor`, which does not see `pendingHealth_` tickets awaiting an outcome. An
  upgrade then commits with a ticket that names a pre-swap grant and epoch. Section 5.2 says the
  gate is "set in the same critical section as the emptiness check" under the ingress mutex only,
  but `healthAction` consults `healthMutex_` state before or after it, so a ticket can be created
  between the check and the flag. Section 5.4 lists "audit sink, health ticket counter" and omits
  `healthActionSeq_` (host memory that restarts from 1 on resume, per its own comment),
  `healthOutcomeRing_` and `ingress_->healthGrants`/`inFlight` keyed by pid. It also states that the
  in-flight set is empty at quiescence without a test.
- Fix: add `pendingHealth` (count from `healthMutex_`) to `QuiescenceReport`. State the lock order
  ingress -> healthMutex\_ -> postbox and take both mutexes in the quiescence-plus-flag critical
  section. Have `healthAction` read the gate under the same lock it uses to create the ticket.
  Complete table 5.4 from the members listed above. Add a test with a queued health ticket that
  expects `NotQuiescent{pendingHealth,1}`, and an `inFlight`-empty-at-quiescence assertion.

### M3. Decision 7 widens the reconciler-road change from P19 agents to every agent

- Where: 4.6 last paragraph, decision 7, open question 4, T1a `ThePublicEntryPointsAgree`.
- Failure: the agreement requirement is "both public entry points accept and refuse the same P19
  state". Forwarding `LocalHost::upgrade(pid, CompiledProgram)` to the schema road for all agents
  removes, for agents with no epistemic state at all, the carry of asks and staged effects that the
  reconciler road exists for. That is a behaviour regression outside P19. The design admits it (4.6,
  Q4) and then relies on only one T0 test using that overload. `program.cpp:405-408` is a public
  API. A user of `LocalHost` with in-flight asks now gets `NotQuiescent` where it used to get a
  carry. The shared 2.3 pre-flight already gives agreement without forwarding.
- Fix: keep the by-value overload on the reconciler road. Make `preflightUpgrade` the shared first
  step on both roads so that populated or arbitrating state, policy and signature produce identical
  codes. `ThePublicEntryPointsAgree` then compares the refusal for the same P19 state, and a second
  test pins that a non-epistemic agent with asks still reconciles. Delete the "second copy of the
  passes" cost argument, because the reconciler road never runs the passes.

## MINOR

### m1. Decision 10

**The oracle is three mechanisms where two suffice, and one breaks under `TEST_FILTER`.**

- Where: section 7 (3), `make test` hook.
- Failure: the pass-0 runtime check already refuses an unregistered dimension or field on a live
  graph, and `RegistryIsConsistent` checks the registry. The Python script, with the
  `coversStructure` gtest `Environment` writing `build/translator-covered.txt`, adds a third
  mechanism. A filtered run (`make test TEST_FILTER=...`, documented in AGENTS.md) writes a partial
  covered file and the script then fails. The "ids parsed from `epistemic_abi_*.hpp`" list is also
  circular: those headers hold bare `string_view` field names, not `OwnedStructure{id,...}`, so
  there is nothing to parse unless the registry is the source. `allOwnedStructures()` concatenating
  per-area headers is the right seam; the grep of `ensureDimension("d....")` against `dimension`
  plus a `kNotOwned` list is the only independent signal.
- Fix: make the script check only (a) dimensions grepped from source against registry plus
  `kNotOwned`, and (b) that each ABI field constant is referenced by some registry entry. Run it
  only on an unfiltered `make test`. Drop `coversStructure`/`Environment`, and let the parity test
  iterate `allOwnedStructures()` itself so coverage is by construction.

### m2. Decision 11: T1b bundles two unrelated mechanisms; T1a ships dead codes

- Where: 8.1, 8.2.
- Failure: T1b is the shape header plus the probe/guard plus the admission gate, pause and setting.
  Admission (a thread-safety change with SEC review) has no dependency on the header, while the
  intention rule depends on it ("for a program whose shape header exists"). A defect in the gate
  blocks the guard landing. T1a defines `UnknownShapeVersion`, `NoTranslator`,
  `ReplayStateUnavailable`, `StateInconsistent`, `UnregisteredOwnedStructure` and
  `RouteCarriesNoEpistemicState`, which cannot be produced until T1b/T1c.
  `RefusalVocabularyIsStable` then tests codes with no producer.
- Fix: split T1b into T1b1 (header, probe, guard, intention rule) and T1b2 (admission, pause,
  setting, `upgradeQuiescing`). T1a declares only the codes it produces and later slices append them
  (appending is already the practice for `EpistemicRefusal`).

### m3. API shape against AGENTS.md

- `TranslationRequest::into` is described as "optional for the pre-flight" but is a reference
  member. Use `cpp26::optional<T&>` or a separate `PlanRequest` type. Two members that are
  optional-by-convention are the sentinel pattern the rules forbid.
- `pauseAdmission(pid, ttl)`, `resumeAdmission(pid)` and `setOwnedStateTranslator` follow no stated
  return convention. Setters chain: `pauseAdmission`/`resumeAdmission` return `AgentExecutor*` (or
  `std::expected<AgentExecutor*, TransferRefusal>` for an unknown pid, with
  `Runtime/UnknownProcess`). Say so.
- `TransferRefusal::actor{}` value-initialises to enumerator 0; check it is not a success-looking
  value, or make it `std::optional<ActorError>`. `ActiveIntention ... limit 0` encodes "none
  allowed" as a number; leave `limit` empty.
- `operator==(TransferRefusal, ActorError)` is an asymmetric equality kept only so old assertions
  compile. A one-time edit of the T0 tests to `.actor` is cheaper and avoids two meanings of `==`.

### m4. Format changes without a stated version position

- Where: 3.1, 8.1.
- Failure: T1a adds `encoding` and `signature` to the policy master (four cells a predicate) but the
  header that carries "shape 1" arrives in T1b. Between T1a and T1b an arbitrating program has the
  new master layout and no version, and the T1b rule "no header with profiles =>
  `UnknownShapeVersion`" refuses a state that was written with the T1a layout. The AGENTS.md rule
  (bump the version, delete the old reader) needs a position here.
- Fix: say that shape 1 is defined by T1b and that nothing persisted before it is supported, and
  that T1a's master fields are provisional until then. Or move the master fields into T1b.

### m5. Resume plan runs twice; the "second read" test is vacuous

- Where: 2.3 last paragraph, T1a `ASecondPlanFailingAfterAGoodPreflight...`.
- Failure: `resume` hands the same `*received` to `runtime_.resume` (`agent_executor.cpp:5440`) and
  `prepare` reads it later, so a base cannot differ between the two reads. The only real apply-time
  failures are budget (work grant) and a throwing translator. The test as described needs a custom
  source that serves two carriers, which exercises the test double rather than the code. Planning
  twice also doubles the work at the 26k-cell bound.
- Fix: replace with an injected failing translator at apply time (it already exists for the rollback
  test) and assert `fatal(init)` plus one WARN. State that `WorkBudget` cannot be decided in the
  resume pre-flight because no grant exists, and give the check to `restoreState`.
