# I3-2 design review, persona: codebase

Scope: `i3-2-design.md` revision 1 at commit e226c507, read against `apps/common/xanadu/vagent/runtime/`
(`compiler.cpp`, `agent_executor.{hpp,cpp}`, `runtime_abi.hpp`, `epistemic_ingress.hpp`,
`epistemic_limits.hpp`, `program.cpp`), `system_docs.{hpp,cpp}` and `tests/xuzz/vagent_epistemic_*`,
`epistemic_invariants.hpp`, `tests/fuzz/fuzz_epistemic_ingress.cpp`. Read only; no repository file but this
one was touched. No BLOCKING finding: the design can be built on the existing code. Eight MAJOR findings
are places where the text says "shared" or "like X" and the code does not allow that as written.

## BLOCKING

None.

## MAJOR

**M1. A queued health action is authorized once, at queue time.** Section 6.2 checks the grant in
`healthAction()` and queues. Section 6.1 lets `revokeHealthGrant` remove it and lifetimes expire, but the
turn that runs later never re-checks. `observe` does re-check at turn time (`agent_executor.cpp` around
3790-3815: "Revoked, expired or superseded while queued ... UnknownSource"). Scenario: an operator
revokes a mistaken reset grant; the reset already in the queue still wipes the source's readings.
Remedy: store the grant ID in the posted item and re-validate under `ingress_->mutex` when the item is
turned into a `HostEvent`, refusing `UnauthorizedHealthAction` into the health outcome map. Say so in 6.2.

**M2. The "advance body shared for reset" (sections 6.3 and 7) is not a parameter of today's code.**
In `emitSourceChange` (`compiler.cpp:4720-4850`) the advance is straight-line inline IR using the
function's local lambdas (`head`, `recordOf`, `find`) and labels. Three things differ for a reset:
(a) the retire walk filters `older?` = record incarnation < `inc` (4735-4738); at an unchanged
incarnation that retires nothing, so the filter must become "every non-retired record";
(b) the tail of the advance ends with `freshWindows(prof)` (clears dedup and streams), which reset
explicitly must not do (6.3: floors stay); (c) the trim step needs `retained` (history x keysPerSource),
which is in `source_change` (`agent_executor.cpp:3739`) but is absent from the section 6.3 turn-record
list ("... the reason bound and max_sources"). Remedy: name the refactor in the design: extract
"retire records matching P, drop supports, move to retired chain, trim to `retained`, settle" into one
emitted routine (or a Gen member lambda called from both entries) taking the filter mode as a register;
keep `freshWindows` in the advance caller only; add `retained` to the health record. Size it: this edits
the I3-1c code that carries the wedge and the replay-floor invariants, so the I3-1c tests
(`vagent_epistemic_ingress_test` advance and retire cases) become a gate for 2b.

**M3. A reset can be re-poisoned by refusals that precede it.** `postRefusals` pushes with plain
`push`, and `pushControl` places the reset ahead of everything non-control (`agent_executor.cpp:592-597`).
So a coalesced `Ingress::charges` entry (charged for refusals received before the reset) runs after the
reset and re-charges the zeroed counters. With the default threshold 3, three queued refusals make a
just-reset source SUSPECT again. Remedy (time-based, no new state beyond one field): the reset turn writes
`reset_at` (its steady time) on the profile; the refusal bookkeeping drops the decayed charge when the
note's `at` is not after `reset_at` (it still counts in `refused_count`, which is diagnostic). The note
already gains `at`, so this costs one `Lte` in IR. Test: refuse, reset before the bookkeeping turn runs,
pump, expect NOMINAL.

**M4. Health turns need the whole control path, not only `pushControl`.** The ordering guarantee in 6.2
rests on three places that today key on `sourceChange.has_value()`: `PostBox::pushControl`
(`agent_executor.cpp:592`), the `hostEvents` insertion in the pump (3750-3758), and the `epistemic` and
`abandon` lambdas (3690-3703). Design 6.2 names only `pushControl`. A health item must be a second control
kind in all four (or a variant on `SourceChange`), with `dropHostEvents` and `forgetIngress`
(`agent_executor.hpp:517,524`) cleaning its outcome. The outcome must live in its own map: `changes` is read
by `finishSourceChange` to revoke a binding the arena refused, and a health outcome landing there would
mask or fake that signal. Also "newest outcome per principal" is lossy for operator actions (isolate then
degrade: the host reads only the second). Prefer a receipt (`HealthReceipt{ticket}` like `TransportReceipt`)
and `healthActionOutcome(target, ticket)` over a bounded log (reuse `epistemic.outcomeLog`).

**M5. Int64 to Double on `content_refusals` and `reject_count` breaks readers silently.**
- `EpistemicView::integer()` returns `std::nullopt` on a Double cell (`ArenaManifold::asInt64` checks the
  value kind, `arena_manifold.cpp:1100`). `epistemic_invariants.hpp:70-71` builds `ProfileHealth` from it, so
  `contentRefusals` and `rejectCount` become permanently nullopt and equal: the fuzzer invariant "a
  refused relayed copy leaves health and contact alone" (`fuzz_epistemic_ingress.cpp:350-357`) turns
  vacuous without failing. Change `ProfileHealth` to `optional<double>` and add a fuzz assertion that the
  read is engaged.
- Test helper `counter()` (`vagent_epistemic_ingress_test.cpp:176`) returns -1 for a Double; about 25
  `EXPECT_EQ(counter(...), n)` sites (lines 401, 794-876, 1230, 1302-1326 ...) need a `decayed()` helper and
  `EXPECT_NEAR`. Sites on `refused_count` and `rejection_streak` stay Int.
- The suites run the real `steady_clock`; exact "burst of k reads k" tests need the injectable
  `AgentClock::steady` (`agent_executor.hpp:121`) in the fixture. Section 8 should say tests inject it.
- Initial values: `emitProfileFor` inits all four counters with the Int `zero_` (`compiler.cpp:4469`);
  the two decayed ones need `cFloat(0.0)`. `putScalar` overwrites a cell's value in place so a Double
  written over a checkpoint's Int is possible; a pre-I3-2 checkpoint is refused by hash anyway under
  the no-shim rule, but state the version/refusal that makes it so (checkpoints from I3-1 resumed under
  I3-2 programs would see an Int `content_refusals`).

**M6. Absence rules for `t_c` and for bare profiles are underspecified.**
- 3.2 says an absent `t_c` means "the first charge sets `c = n`". The resume code today clears steady
  fields to absent (`agent_executor.cpp:4620-4623`); an implementer copying that pattern for `t_c` while
  `c > 0` would discard the evidence. State the rule as `c <- c + n` when `t_c` is absent (identical when
  `c = 0`), and make reset write `c = 0.0` and clear `t_c` together.
- Bare profiles exist: a retire of an unseen principal mints a tombstone with only `incarnation` and
  `retired` (`compiler.cpp:4695-4706`), and a revive does not add the counters. `rtHealth` and the
  decay IR must treat any absent counter or latch as zero/false, unlike the current `charge` lambda
  (`compiler.cpp:4544-4548`), which ends `IngestionFault` on an absent counter. Define the health turn
  on a tombstone: recommend refusing `NoProfile` (a retired principal has nothing to isolate or reset);
  if instead allowed, `isolate` must survive revive (it will: fields persist).

**M7. The wall time has two owners.** 6.2's `HealthActionRequest::atWall` is caller-supplied (default
`0.0`, so "unset" is a magic value) while 7 lists "the steady and wall timestamps (clocks)" as executor
C++. The executor already has `wallNow()` (`agent_executor.hpp:~545`) and `wallSynchronised`. Remedy: drop
`atWall` from the request; the executor stamps wall at queue time, and writes `at_wall` only when the
wall clock is synchronised (absent otherwise), the same rule observations already follow
(`ClockUnsynchronised`). This also removes an unauthenticated audit field.

**M8. The refusal vocabulary and its plumbing are not listed.** The design uses `InvalidHealthGrant`,
`UnauthorizedHealthAction`, `NoProfile` and, for a bad code, `InvalidObservation`, but:
- Synchronous refusals are `EpistemicRefusal` values (`epistemic_ingress.hpp:57-82`); the two new ones
  plus a health-code refusal need enumerators and `epistemicRefusalName` cases. Reusing
  `InvalidObservation` for a reason code is wrong (that code is an observation's wire refusal and is
  counted by observation stats); add `InvalidHealthCode`.
- Turn-result codes go in `runtime_abi.hpp` beside `kTurnResultNoProfile`; `outcomeClassOf`
  (`agent_executor.cpp:3242`) maps by exact code and falls through to `Terminal`. `NoProfile` for health
  clears after the host binds the source, so it is `NeedsHostAction`; `EpistemicBudget` is already so mapped.
- Constants: `kObsHealthSuspect`, `kObsHealthDegraded` (`runtime_abi.hpp:587-589` has only NOMINAL and
  FAULTED), `kProfileFault`, `kProfileReasons`, `kProfileResetAt`, the `t_c` field names, `kTurnHealth`
  and the reason-record field names. The ABI test pins names (`vagent_epistemic_abi_test.cpp`).
- New entry label: `CompiledAgent::healthEntry` (`runtime_abi.hpp:871` region), emission at
  `compiler.cpp:7192-7196`, and a fourth remap in `program.cpp:119-130`. The design says "beside
  `sourceChangeEntry`" but omits the remap; a missed remap is a silent no-op entry after program upgrade.

## MINOR

- **m1. The 3% claim is false at sample level (3.3).** "A 1 Hz sensor with a 3% refusal rate stays
  NOMINAL" holds for the mean (steady `c` = 0.03*60/ln2 = 2.60) but not the state: shot noise has
  variance lambda*h/(2 ln 2) = 1.30 (sd 1.14). Simulated 200k Bernoulli(0.03) steps at 1 Hz: `c >= 3`
  on 34.2% of steps (5%: 82.6%). Probe: `/tmp/i3-2-codebase/sim.py`. The 2a noisy-sensor test
  ("honest peers NOMINAL") will flake or need a rate far below 3%. The burst arithmetic is right
  (60*log2(10/3) = 104.2 s) and the late-event overcount `2^(d/h)-1` is 1.2e-4 at d = 10 ms. Hand this to
  the ML gate with Q1 (hysteresis); for the build, state the declared honest-peer rate in the test.
- **m2. "the VM's own float checks" (3.2 Range) is overstated.** Only `Exp`/`Ln`/`Sqrt` check finiteness
  (`vortex_vm.cpp:922-945`); `Add` and `Mul` on doubles do not (`:454-515`). Finiteness holds by construction
  (contraction, bounded `n`, finite wire reals, validated `h`), not by a VM check; write that, and keep the
  10^6-charge test. `Exp` of a large negative argument underflows to 0.0, which is finite, so a 10 h gap is
  fine; `Exp` requires a Float input, so keep every operand a Double (`cFloat`).
- **m3. The coalescing window depends on `h`.** The bound "below 1e-4" assumes millisecond delay against
  a 60 s `h`. The note's `at` is the newest refusal in a batch charged to older ones, and a bookkeeping turn
  runs behind up to `ingressQueue` (64) ingestion turns, so under flood the delay is much longer.
  `2^(d/h)-1` is 1.0 at d = h. Give `h` a lower bound in the domain (suggest 1 s... but argue it) or
  state the bound as a function of `h`.
- **m4. `Ingress::Charge` needs `at` as `std::optional<double>`** (or required at construction); `double at{}`
  would be the 0.0 sentinel the owner rules out. Only direct content refusals feed the decayed counter, so
  stamping `at` on relayed/other charges (3.4) is unused by IR; keep it only if M3's `reset_at` check needs it
  for `other`.
- **m5. API shape (section 6).** `actions` is bounded by the three enumerators: use a small bit-set or
  `cpp26::inplace_vector<HealthActionKind, 3>` per AGENTS. `HealthActionRequest::kind` lacks a default
  initialiser; `kind` + `optional code` permits Reset-with-code and Isolate-without-code states, so prefer
  a variant (`Isolate{code}`, `Degrade{code}`, `Reset{}`). `revokeHealthGrant` should return
  `AgentExecutor *` (setters chain); `grantHealth` returns `expected<AgentExecutor *, EpistemicRefusal>`
  like `bindSource`. `issuer` in the request duplicates the grant lookup; keep (the adapter attests it) but
  log mismatch distinctly from "missing". Bound grants per actor (and the ID length) and erase them in
  `forgetIngress`; guard the grant map with `ingress_->mutex` and document the calling thread as for
  `bindSource` (reads the program).
- **m6. Settings plumbing (8).** No code outside `system_docs.cpp` and tests reads the `vagent.epistemic_*`
  settings; the host copies them into `AgentSettings::epistemic` (`EpistemicLimits`). So the work is: three
  fields in `EpistemicLimits` with defaults in `epistemic_limits.hpp`, `validate()` cases and new
  `EpistemicLimitError` enumerators (half-life is a double: reject NaN, inf, <= 0 and > 31 years; threshold
  the same up to 1e6), `settings::kVagentEpistemic...` names in `system_docs.hpp:287-311`, three
  `defaultSettingSpecs` entries (`float` for the first two, `integer` for the reasons bound), the expected
  list in `system_docs_test.cpp:172-184`, and the Schema page text (`system_docs.cpp:97-123`, which today
  lists none of the `vagent.epistemic_*` settings; the declared sample-rate interpretation of 3.3 belongs
  there). The turn records that carry them (`limits` for ingestion, `refusal` note, health record) use
  `readInOrder` in ingestion, so extend `kIngestOrder` in lockstep with the writer (`compiler.cpp:5004`,
  `agent_executor.cpp:3844`).
- **m7. Cell price of reasons.** 12 cells x 8 reasons = 96, plus `fault` (about 12), `reset_at`, two `t_c`
  (2 cells each): a worst-case profile grows by about 115 cells. Across `max_sources` = 128 that is 14.7k of
  the 24,576-cell ceiling (`epistemic_limits.hpp:38`) which is sized for about 18 fully active principals.
  Cap bound times `max_sources` in the defaults rationale, or lower the reasons default (4).
- **m8. Resume write of `t_c`.** `restoreState` only clears steady fields today via
  `setField(..., std::nullopt)`; writing the restore time needs a minted scalar cell
  (`arena.makeScalarCell(double)`) or `copyCellData` on an existing target. Resume uses the restoring
  executor's `clock_.steady()` as seconds; confirm it is the same epoch-relative basis as
  `received_steady` (`agent_executor.cpp:3434`). The `reasons` list and `fault` record clone with the
  profile because the profile clone pair list already carries `grab`/`step` (`:4601-4608`); add a resume
  test for each.
- **m9. `isolate` leaves current records stamped NOMINAL.** The record's `health` is "at arrival", so after an
  isolate every existing support still reads NOMINAL. I4 must consult the profile, not the stamp, for
  eligibility; add one sentence in 4 and a test that a post-isolate record is stamped FAULTED while a
  pre-isolate one is not rewritten.
- **m10. Fuel and bound for the health turn.** "Bounded like an advance" should quote the grant: K x H
  records with a settle per dropped support; check that a reset of a source with `keysPerSource` x `history`
  = 16 records fits `TurnExhausted` at the default turn fuel (measure with the I3-1 probe).

## Section 10 questions (codebase persona)

1. Half-life and threshold: defer to the ML gate; see m1 for the evidence a sample-level test will need.
   Hysteresis would add a stored latch (a field) or a second threshold read by `rtHealth`; either is cheap
   in IR, and a derived-only state is simpler to test.
2. Clearing a `degrade` without `reset`: cheap to build (excise reasons by `code` from the list with the
   same list helpers; the `reasons` list is already walked). It needs a fourth `HealthActionKind`, which the
   spec's three directives do not list, so it is the owner's approval; the alternative is a per-reason
   expiry steady time, which needs no new action but a clock read in `rtHealth`.
3. The carried `-pred` wedge on reset: acceptable if the refusal is typed and mapped `NeedsHostAction`
   (M8) and the operator can still `isolate` (a latch write, no `-pred`), which is the safer action
   for a source that wedges retraction.

Sentinel check: none is required by the design's data model (absent `t_c`, absent `fault` and
`optional code` are arena or optional absence); the exceptions to fix are `atWall = 0.0` (M7) and a
default-zero `Charge::at` (m4).

Vortex check: the counter update, state derivation, reasons and the three actions are IR; the C++ left
(grants, clocks, queue, resume rebase) is host authorization, transport and clocks, which the brief allows.
The only part that stretches it is the `t_c` rebase in `restoreState`, which is the resume copy itself.

Verdict: FAIL
