# T1 design review: DIST (lifecycle, atomicity, admission races)

Verdict: FAIL. 4 MAJOR, 6 MINOR. Code checked at HEAD 49149e4b (`apps/common/xanadu/vagent/runtime/`);
line numbers are current, not the design's 47d00222 numbers.

## MAJOR

### M1. `observe` has two critical sections; the gate in 5.2 does not close the race (section 5.2)

Claim: "observe ... read the flag first, under the same mutex, before they read a binding or push ...
There is no third case."

Code: `observe` reads the binding and the latch under `ingress_->mutex` at `agent_executor.cpp:3768-3781`
and RELEASES it. It then builds the record, shapes the item and takes the mutex again at `:3956` to
do the queue-fairness test and `posts_->push`. Unlocked work sits between the two.

Scenario: thread A passes the flag check in section 1 (Open). Pump thread runs `upgrade`: takes the
mutex, `pendingFor(pid)` is false, sets `swapping`, releases. A enters section 2 and pushes. The item
carries the old binding copy (`b`, `policy`, `domain`, params) and the pre-swap `generation`. The new
program consumes it. This is exactly the post the gate exists to stop, and `posts` was read before it
landed.

Fix: the admission test belongs in the SECOND section, in the same lock scope as `posts_->push`, and
nothing else is sufficient. Keep the first-section check only as an early-out. State the invariant in
the design as "the flag check and the push are one critical section" and make
`APostBeforeTheGateIsCountedAndAPostAfterItIsRefused` race the two sections (a hook between them), not
just call `observe` inside the swap.

### M2. `healthAction` and `healthActionBatch` push outside any ingress critical section (5.2)

Code (`agent_executor_health.cpp:160-340`): the grant is matched under the ingress mutex (`:160-185`),
the mutex is released, then the code takes `healthMutex_`, mints a receipt, inserts
`pendingHealth_[receipt.counter]` (`:243`) and finally `posts_->pushControl` (`:300`, and `:494` area
for the batch) with no ingress lock held. For `Reset`, the generation and the in-flight snapshot are
taken under a later, separate ingress lock (`:260-290`).

Scenarios:

- Flag check at entry (Open), swap gate set, push lands during the swap. The design says
  "health control items" are gated; as written the check cannot be atomic with the push.
- If the check is added only at entry and a refusal is returned after `pendingHealth_` was inserted,
  the record is never started or erased and counts against `healthPending` and `healthPendingPerGrant`
  forever: a refused call leaks backpressure.
- A `Reset` that took `requestReset(...)` (bumps `resetMarks.requested`) and then refuses
  `AdmissionClosed` has burned a generation. Harmless to correctness (`committed` is what supersedes),
  but readings queued after it carry a generation that no reset will ever commit.

Fix: check the flag at the top of both calls AND re-check it in a single ingress-locked scope that
also performs `pushControl`. Lock order ingress then postbox is already the order `observe` uses.
Insert into `pendingHealth_` only inside that scope, after the check, or erase it on refusal. Take the
reset generation in the same scope. Add a test with a hook between the grant match and the push.

### M3. Resume budget refusals cannot come from a pre-flight that runs before the grant exists (2.3, 4.5, 2.2 rows `WorkBudget`, `PeakCopyBudget`)

Claim: all refusals including `PeakCopyBudget` and `WorkBudget` "leave the old actor ... as they were"
and "No pid exists when it refuses."

Code: the resume grant is produced by the authorizer inside `runtime_.resume`, after `issuer_.issue`
(`agent_executor.cpp:5428-5444`; `program.cpp:325-345`: `instructions`, `kLocalMaxCells`). The
pre-flight sits before `issue`, so `TransferBudget::work` and `maxCells` are unknown to it. `WorkBudget`
("exceeds the remaining instruction grant") is therefore decidable only inside `restoreState`
(`:888`), after the pid exists, which ends as `fatal(kCauseInit)`: the failure mode the section was
written to remove. `PeakCopyBudget` against `settings_.epistemic.cells` is decidable early; against
the grant's `maxCells` is not.

Fix: say so, and pick one. Either (a) pre-flight checks the planned work and cells against the same
constants the issuer will use (the issuer is the executor's collaborator, so give it a
`grantFor(ticket)` dry call that mints nothing), or (b) resume refuses these two codes at apply time,
the pid is returned, the table says "pid exists" for them, and the refusal is stored for retrieval
(see m1). Option (a) keeps "no pid on refusal" true for every code and is preferable. Add a resume
test at a grant one instruction below the plan.

### M4. A removed or re-signed predicate that has a binding but no state upgrades with a stale binding (4.3, 6, decision 9)

Claim (D3): "a policy change is refused on every road ... so a binding cannot be stale after a
commit", and `PredicateRemoved` fires for "a saved predicate with a policy absent from the new
program" taken from saved STATE.

Code: `upgradeChangeRequiresRefusal` (`upgrade_schema.cpp:728-740`) does not include
`RemovedPredicate`. `bindSource` needs no profile (`:3622-3650` snapshots from the program) and a
fresh binding creates no profile until the first reading. So: arbitrating agent, source bound, no
reading yet (`populated == false`, header only). Upgrade to a program that drops the predicate (or, on
the reconciler road, anything at all) passes the guard, commits, and the binding keeps `policy`,
`params`, `domain` for a predicate the new program does not declare. The next `observe` queues a
reading against it. The D3 test (`BindingsSurviveAnUnchangedPolicy...`) only covers a policy change.

The design's own "whether or not the agent has a profile or a binding" sentence is true for the
`EpistemicPolicy` change kind only. Signature and removal checks are keyed to state in 4.3.

Fix: decide predicate compatibility from `(old schema or master, new schema)` for every arbitrated
predicate of the agent, plus every predicate with a live binding (`Ingress::bindings` keyed by pid),
independent of populated state. Refuse `PredicateRemoved` when a bound predicate vanishes; add a test
with a bound, empty agent. On the reconciler road run the same schema comparison (it has none today).

## MINOR

### m1. The "second plan fails after a good pre-flight" case is not a TOCTOU, and its test is unbuildable (2.3 last paragraph, test `ASecondPlanFailingAfterAGoodPreflight...`)

`restoreState` reads the saved agent from the context built from the same in-memory `received` value
that `resume` moves into `runtime_.resume` (`:888`, `:5444`). Nothing is re-fetched, so a source that
"serves a second, different carrier" is never consulted a second time. Genuine apply-time failures are
the grant budgets (M3), `instantiate` refusing, and arena limits. Delete the TOCTOU wording and the
test; replace with a test of the real apply-time failures. Whatever remains must be observable: a
holder of the pid sees only an actor that dies with cause `init`, so expose the stored
`TransferRefusal` (`lastRefusal(pid)` or in the death report) instead of a WARN only.

### m2. Swap-gate release order and the post-commit tail (5.2)

"A guard clears `swapping` ... after the commit or the refusal" is ambiguous. At HEAD the commit is
followed by code that is outside the `try` and outside `runtime_.upgrade`: `clearLatchCache(pid)`
(`:5810`), `liftLimits`, `drain` (runs a turn), `setCounter`, `flushStats`, `deriveSchedule`
(`:5811-5818`). Specify that the guard is destroyed after `clearLatchCache` and the drain, so no
reading or health action is admitted against a latch cache that predates the commit or while the
first post-swap drain runs. Also note that an exception in that tail leaves a committed upgrade
reported as a throw; "all-or-nothing" holds only up to `found->second.swap(candidate)`. Say so, or
make the tail `noexcept`/return a status.

### m3. Pause ttl versus `upgradeQuiescing(timeout)` (5.3)

Default ttl 30 s lazily expires. A host loop that pumps until quiet for longer silently reopens
admission and may never become quiet; nothing in `quiescence` tells the loop the pause lapsed
(`admission` is "informational"). Fix: `upgradeQuiescing` renews the pause for its whole `timeout`
(or clamps `timeout` to the ttl and says so), and `upgrade` refuses if its own gate was acquired
after a lapse. Add a test with a fake clock past the ttl in the middle of the drain.

### m4. A busy mesh mailbox makes an arbitrating agent permanently un-upgradeable (5.1)

`pauseAdmission` closes only the five host calls. Mesh messages to the pid (peer chatter, I3-4
verification traffic) still arrive through `runtime_.receive`, and the mailbox refusal has no
operator remedy. Add a `holdMailbox` effect to the pause (delay, not drop) or document the limit and
make `NotQuiescent{mailbox}` carry the count so a controller can tell progress from stagnation.
Separately: the "same thread" argument for the mailbox read needs a citation. `cluster_node.cpp:544`
calls `runtime().receive`, and `MeshActorRuntime` has no lock; confirm which thread that is.

### m5. Host state the table calls "zero" is not checked by the quiescence report (5.4)

`Ingress::charges` (an open charge waiting for its note), `uncounted`, `refusedWhileFaulted` and
`marks` survive an upgrade and are not in the report. An open charge carries a `generation`; it
closes into a note under the new program. Probably benign, but "zero: the post box is empty at the
check" is not an invariant of those maps. Either add `openCharges` to `QuiescenceReport` (refuse) or
list them in `kHostState` as kept, with the reason.

### m6. `LocalHost` by-value upgrade: retention side effect and same-reference aliasing (4.6, test `ThePublicEntryPointsAgree`)

`LocalHost::retainProgram` (`program.cpp:369-381`) returns the ALREADY registered program when the
reference exists. With explicit references (`fromUnit`, the X-9 scenario) a by-value upgrade to a
program with a changed policy and the same reference silently upgrades to the OLD object and
reports success, so no refusal test can be built through this entry point; the shared-pointer
overload refuses the same input as `NotAuthorized` (`resolveProgram(...).get() != program.get()`).
Also, a program with a new reference is registered in `programs_` and in the executor before the
pre-flight runs, so a refused upgrade leaves it authorized for resume. "Refusal leaves nothing
behind" (2.4) lists the context, grant, latch, bases and cell count, not the program registry. Fix:
run `preflightUpgrade` before retaining, refuse a same-reference/different-content program by name
(`TransferCode::Runtime` with `NotAuthorized` is too coarse), and add the registry to the
unchanged list.

### m7. Resume refusal ordering and the rest of the "leaves nothing" list (2.3)

(a) The pre-flight needs the new program (`resolveProgram(checkpoint.programRef)`); the design does
not say what happens when it is absent, and the table puts `Runtime` first. State that a missing or
unauthorized program is `Runtime` and is decided before any policy compare.
(b) After the pre-flight passes, `trustNode` (`:5413`) and `issuer_.issue` (`:5436`) are state
changes that survive a later `runtime_.resume` failure at HEAD. The design makes the refusal
clean only for its own codes. Either move `trustNode` after `runtime_.resume` succeeds, or add the
trusted-node set and issued count to the failure-path tests too.
