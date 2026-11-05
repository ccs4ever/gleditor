# T1 design: the translator spine and typed refusals

Status: revision 2, after review round 1 (ARCH and DIST both failed revision 1; dispositions are the
last section). Authority: approved P19 v1.5 section 2.2, "Upgrade and checkpoint translation" (lines
192-252), and section 8.2 gate 5 with its regression bullets (lines 963-1010); the plan's T1-D row
and slice T1 ([remaining-plan.md](remaining-plan.md) sections 4.4 and 4.5); the lead's rulings in
[decisions.md](decisions.md): P2-1 (one typed refusal channel), R3-11 (shape version and one guard),
D3 (policy change against bindings), O1 with A5 (supported-agent upgrade only) and section 0 (review
cadence). Code citations are `path:line` at HEAD `49149e4b`, relative to `apps/common/xanadu/`
unless they start with `tests/`. I3-2 is implemented at HEAD, so every health structure below is
read from its code: the profile field names are in `vagent/runtime/epistemic_abi_health.hpp:89-100`
(`kProfileResetEpoch`, `kProfileActionSeq`, `kProfileFaultSeq`, `kProfileReasons`,
`kProfileHealthCells`, `kProfileActions`), the executor's health state is in
`vagent/runtime/agent_executor.hpp:870-877` and `agent_executor_health.hpp:25-52`, and the host's in
`vagent/runtime/agent_executor_internal.hpp:335-399`.

## 1. Decisions

| #   | Decision                                                                                                                                                                                                                                                                                                        | Section |
| --- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------- |
| 1   | One vocabulary, `TransferRefusal`, with a code, the route, the subject and the offending texts and numbers. `resume` and both `upgrade` overloads return it. A pre-flight produces it before any state changes.                                                                                                 | 2       |
| 2   | Resume's pre-flight runs inside `AgentExecutor::resume`, after `fetchValue` and before `trustNode`, `issuer_.issue` and `runtime_.resume`. A missing program is decided first. The grant-dependent budgets are decided there too, from a grant probe the host installs, so no pid exists when any code refuses. | 2.3     |
| 3   | The shape version is a three-cell record at the head of the agent's `d.epistemic` rank, minted at compile time only for a program with an arbitrated predicate. Shape 1 is defined by T1b1 and nothing persisted before it is supported. It is compared by number and never copied.                             | 3       |
| 4   | `hasTranslatableEpistemicState` is the only place that decides whether an agent holds state the executor may carry. It ignores the header.                                                                                                                                                                      | 3.2     |
| 5   | The three copiers stay three. They share the P19 half: one routine, `translateEpistemicState`, in four read-only-then-write passes over one old-to-new map. The generic halves stop following `d.trust`, `d.epistemic` and clones.                                                                              | 4       |
| 6   | Predicate compatibility is a full key decoded from the graph on both roads. A digest is only an index into buckets that are searched by full-key equality. Policy, signature and removal are decided from `(old schema, new schema, live bindings)`, never from whether state exists.                           | 4.3     |
| 7   | Both upgrade overloads start with the same `preflightUpgrade`. The by-value `LocalHost::upgrade(pid, CompiledProgram)` stays on the reconciler road, so a non-epistemic agent still carries its asks and staged effects; populated P19 state on that road is a typed refusal that names the other overload.     | 4.6     |
| 8   | Quiescence is one report over every queue and every piece of host state it must cover (post box, health tickets, in-flight set, open charges, mailbox). The gate is set under the ingress and health mutexes in the same scope as the emptiness check, and every post is admitted in the scope that pushes it.  | 5       |
| 9   | D3: a policy change or the removal of an arbitrated or bound predicate is refused on every road in T1, so bindings cannot go stale. T2's typed migration revokes the bindings of the migrated predicate and the operator rebinds. Nothing rebinds silently.                                                     | 6       |
| 10  | A registry of owned structures is declared once per area. The parity test iterates it, so coverage is by construction. A script checks only what no test can see: dimensions against the registry and field constants against the registry. A runtime check refuses an unregistered structure.                  | 7       |
| 11  | T1 splits into T1a (channel), T1b1 (shape and guard), T1b2 (admission) and T1c (routine, digest, oracle). T1b1 and T1b2 are independent. Together about 3,400 lines, against the plan's 1,500.                                                                                                                  | 8       |

## 2. The refusal channel

### 2.1 What exists at HEAD

- `resume` returns `std::expected<ActorPid, ActorError>` (`vagent/runtime/agent_executor.hpp:510`).
  `ActorError` is a closed enum without a payload (`vortex/mesh_actor.hpp:30`).
- Resume of a changed policy looks like a success. `resume` returns a pid at
  `vagent/runtime/agent_executor.cpp:5440`. The first `execute` calls `prepare` (`:1052`), which
  calls `restoreState` (`:888`). `restoreState` throws `std::runtime_error` at `:5213`. `prepare`
  catches it and logs at DEBUG (`:992-994`), and `execute` ends in `fatal(state, kCauseInit)`
  (`:1054`). The caller holds a pid for an actor that dies with cause `init`.
- The existing test `AChangedPolicyRefusesTheResume`
  (`tests/xuzz/vagent_epistemic_resume_test.cpp:325`) does not reach that path. A program built by
  `CompiledProgram::fromSource` has a content-derived reference
  (`vagent/runtime/program.cpp:33,39`), so a changed policy has a different reference and the
  authorizer refuses it (`vagent/runtime/program.cpp:326`) with a bare `NotAuthorized`. `vagent run`
  builds programs with `fromUnit` and an explicit reference (`apps/vagent/main.cpp:1050,1087,1220`),
  so there the mismatch reaches `restoreState` and the actor dies. The comment in the test
  (`"restoreState compares the hashes again regardless"`) names a check no test exercises.
- Both upgrade overloads return `ExecutionFailed` or `NotQuiescent` for every cause
  (`vagent/runtime/agent_executor.cpp:5449-5480,5498,5503,5534-5546`). A caller cannot tell a
  changed policy from a populated profile from an active intention.
- The `EpistemicRefusal` name is taken. It is the ingress enum
  (`vagent/runtime/epistemic_ingress.hpp:71`). The plan's "executor-level `EpistemicRefusal`" would
  collide with it, so this design names the new type `TransferRefusal`.
- Callers of `resume` and `upgrade` outside tests: `LocalHost::upgrade` twice
  (`vagent/runtime/program.cpp:401,408`) and `AgentTui::upgrade` (`vagent/tui/agent_tui.cpp:649`).
  `cluster_node.cpp` calls neither: its `ActorError` mentions are `launch` and `send`
  (`vagent/cluster/cluster_node.cpp:646,713,719`). The plan lists it under T1; no edit is needed
  there. `apps/vagent/main.cpp` calls neither.

### 2.2 The vocabulary

New files `vagent/runtime/epistemic_refusal.{hpp,cpp}`, namespace `xanadu::vagent::runtime`.

```cpp
enum class TransferRoute : std::uint8_t { Resume, UpgradeSchema, UpgradeReconciler };

enum class TransferCode : std::uint8_t {
  Runtime, NotQuiescent, ActiveIntention, PolicyChanged, PredicateIncompatible, PredicateRemoved,
  PeakCopyBudget, WorkBudget, ProgramReferenceReused,        // T1a
  UnknownShapeVersion, PolicyEncodingChanged,                // T1b1
  RouteCarriesNoEpistemicState,                              // T1a: the reconciler road
  NoTranslator, UnregisteredOwnedStructure, ReplayStateUnavailable, StateInconsistent,  // T1c
};

struct RefusalDetail {                         // designated initialisers fill what applies
  std::optional<std::string> subject;           // predicate, principal token, reason or dimension
  std::optional<std::string> saved, current;    // texts found in the old state and in the new program
  std::optional<std::uint64_t> savedCount, currentCount, limit;
};

class TransferRefusal {
public:
  [[nodiscard]] static TransferRefusal make(TransferCode, TransferRoute,
                                            zigzag::vortex::mesh::ActorError actor,
                                            RefusalDetail = {});
  TransferCode code;
  TransferRoute route;
  zigzag::vortex::mesh::ActorError actor;       // what callers compared before this type existed
  RefusalDetail detail;
private:
  TransferRefusal() = default;
};
[[nodiscard]] std::string_view toString(TransferCode) noexcept;
[[nodiscard]] std::string toString(const TransferRefusal &);
```

Only `make` constructs one, so `actor` is never value-initialised to enumerator 0 (the first
`ActorError` is a failure code in any case, but a defaulted member would let a refusal exist whose
`actor` was never decided). No field is a sentinel: an absent value is an empty `std::optional`, and
`ActiveIntention` leaves `limit` empty because it states a count, not a ceiling. Every text is cut
at 128 bytes. No document text, key or secret is ever put in one: the subjects are predicate names,
principal tokens (public-key digests, which `isPrincipalToken` checks at `agent_executor.cpp:3536`),
field names and reason names. `toString` prints
`PolicyChanged(route=Resume, subject=reading, saved=ab12..., current=cd34...)`.

There is no `operator==` against `ActorError`. The T0 assertions that compare `result.error()` with
an `ActorError` (`tests/xuzz/vagent_epistemic_ingress_test.cpp:1469,1506,1516,1523`, and the resume
and live-upgrade tests that do the same) are edited once to `result.error().actor`. That is about
twenty lines, and it avoids two meanings of `==`.

The order of checks is part of the contract, because the first failing check names the refusal:
**(1)** a missing or unauthorized program, `Runtime`, decided before any comparison; **(2)** host
quiescence (upgrade only); **(3)** `ActiveIntention`; **(4)** shape; **(5)** policy encoding,
policy, signature, removal; **(6)** `PeakCopyBudget` and `WorkBudget`.

| Code                           | Decided by                                                                                   | `subject`                  | `saved` / `current`                        | Numbers                                       | `actor`           |
| ------------------------------ | -------------------------------------------------------------------------------------------- | -------------------------- | ------------------------------------------ | --------------------------------------------- | ----------------- |
| `Runtime`                      | any lower layer: unknown pid, missing program, authorizer, fetch, grant                      | none                       | none                                       | none                                          | the lower code    |
| `ProgramReferenceReused`       | a program under an already retained reference whose schema differs from the retained one     | the reference              | none                                       | none                                          | `NotAuthorized`   |
| `NotQuiescent`                 | the quiescence report (section 5.1)                                                          | reason name                | none                                       | `savedCount` is the count                     | `NotQuiescent`    |
| `ActiveIntention`              | an arbitrating agent, or the schema road of any agent, has intentions                        | none                       | none                                       | `savedCount` intentions                       | `ExecutionFailed` |
| `UnknownShapeVersion`          | saved header differs from the new program's header, or is absent while state is populated    | `d.epistemic`              | none                                       | `savedCount` (absent if none), `currentCount` | `ExecutionFailed` |
| `NoTranslator`                 | populated state, the header matches, no translator is registered for it                      | `d.trust`                  | none                                       | `savedCount` is the shape                     | `ExecutionFailed` |
| `PolicyEncodingChanged`        | a saved master's `encoding` text differs from the new program's                              | predicate                  | the two tags                               | none                                          | `ExecutionFailed` |
| `PolicyChanged`                | typed policies differ; either side absent means a gain or a loss                             | predicate                  | the two policy hashes (absent = no policy) | none                                          | `ExecutionFailed` |
| `PredicateIncompatible`        | name, arity, ordered types or key positions differ                                           | predicate                  | the two signatures                         | none                                          | `ExecutionFailed` |
| `PredicateRemoved`             | an arbitrated or bound predicate of the old program is absent from the new one               | predicate                  | the saved signature or hash                | none                                          | `ExecutionFailed` |
| `RouteCarriesNoEpistemicState` | reconciler road with populated state                                                         | `d.trust` or `d.epistemic` | none                                       | none                                          | `ExecutionFailed` |
| `UnregisteredOwnedStructure`   | the old graph links a dimension that no registry entry owns (section 7)                      | dimension name             | none                                       | none                                          | `ExecutionFailed` |
| `ReplayStateUnavailable`       | a profile lacks `incarnation`, `streams` or `dedup`, or a dedup entry is malformed           | principal token            | the missing field                          | none                                          | `ExecutionFailed` |
| `StateInconsistent`            | a record has no profile, an edge's far end is unmapped, or a pass copied other than its plan | structure id               | none                                       | `savedCount` copied, `currentCount` planned   | `ExecutionFailed` |
| `PeakCopyBudget`               | planned cells exceed the allowance (section 4.5)                                             | none                       | none                                       | `savedCount` needed, `limit` allowed          | `ResourceLimit`   |
| `WorkBudget`                   | planned work exceeds the remaining instruction grant                                         | none                       | none                                       | `savedCount` needed, `limit` available        | `ResourceLimit`   |

The `actor` column reproduces what `upgrade` returns today, so the T0 behaviour is the same apart
from the comparison syntax. Each slice declares only the codes it can produce (the enum above is the
end state, with the owning slice marked) and appends the rest, as `EpistemicRefusal` has been
extended; `RefusalVocabularyIsStable` in a slice tests the codes that slice declares.

### 2.3 Where it is returned

| Entry point                                                                 | Return type                                  | Pre-flight                                                                                                                                         |
| --------------------------------------------------------------------------- | -------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| `AgentExecutor::resume(const AgentCheckpoint&)`                             | `std::expected<ActorPid, TransferRefusal>`   | `preflightResume`, after `fetchValue` (`agent_executor.cpp:5396`) and before `trustNode` (`:5409`), `issue` (`:5428`), `runtime_.resume` (`:5440`) |
| `AgentExecutor::upgrade(pid, shared_ptr<CompiledProgram>, grant)`           | `std::expected<void, TransferRefusal>`       | `preflightUpgrade(pid, program, Schema)`, in place of `upgradeRefusal` at `:5498` and the intention test at `:5503`                                |
| `AgentExecutor::upgrade(pid, CompiledProgram&)`                             | `std::expected<void, TransferRefusal>`       | `preflightUpgrade(pid, program, Reconciler)`, in place of `upgradeRefusal` at `:5843`                                                              |
| `LocalHost::upgrade` (both overloads, `vagent/runtime/program.hpp:287,291`) | `std::expected<void, TransferRefusal>`       | forwards; the by-value overload runs `preflightUpgrade` before it retains (section 4.6)                                                            |
| `AgentTui::upgrade` (`vagent/tui/agent_tui.cpp:649`)                        | unchanged `std::expected<void, std::string>` | prints `toString(refusal)` instead of `static_cast<int>(res.error())`                                                                              |

`preflightResume(const AgentCheckpoint&) const` and
`preflightUpgrade(pid, const CompiledProgram&, UpgradeRoute) const` are also public, return
`std::optional<TransferRefusal>` (absence is a pass), change nothing, and give an operator a dry
run. `resume` and `upgrade` call them; there is one implementation of each check. Both upgrade
overloads call the same `preflightUpgrade`, differing only in the `UpgradeRoute` argument, which
switches on the checks that belong to one road (the schema road's flights and requests, the
reconciler road's populated-state refusal). That is what makes the two public entry points agree on
a P19 state without sharing a copier (section 4.6).

The resume pre-flight reads the received base through a bare
`zigzag::ArenaManifold view(received->manifold.get())` (`vortex/mesh_value.hpp:98-105`; the
constructor is `zigzag/arena_manifold.hpp:255`). It needs no `WorkerContext`, so it pays no
standard-library bootstrap. The old dimensions are found by name from `received->root`, with the
walk now in `restoreState` (`agent_executor.cpp:5056-5080`) moved to `discoverDimensions` in
`epistemic_translate.cpp` and called from both. The new program's keys come from the program's own
`UpgradeSchema` (`program.hpp:171`), which carries typed parameters and the policy
(`upgrade_schema.hpp:37-45`). The program is `resolveProgram(checkpoint.programRef)`; when it is
absent the answer is `Runtime{NotAuthorized}`, the code the authorizer would give
(`program.cpp:326`), and nothing else is compared.

**Budgets are decided before the pid, from a grant probe.** The resume grant is produced by the
authorizer inside `runtime_.resume` (`mesh_actor.cpp:228-262`; `program.cpp:321-345`:
`instructions`, `kLocalMaxCells`), after `issuer_.issue`, so the pre-flight cannot read it. Revision
1 claimed `WorkBudget` and `PeakCopyBudget` come from the pre-flight without saying how; they
cannot. Two ways keep "no pid on refusal" true. Reclassifying both as apply-time codes, with a pid
and a stored refusal, makes the table lie for the two codes the operator is most likely to hit on a
large agent, and an actor that dies with `init` is the failure mode this section exists to remove.
The chosen way is a dry run that mints nothing:
`AgentExecutor::setResumeGrantProbe(ResumeGrantProbe)` (a setter, so it returns `AgentExecutor*`),
where `ResumeGrantProbe` is a `std::function` from a program reference to
`std::expected<ResumeLimits, ActorError>` and
`ResumeLimits{std::uint64_t instructions; std::uint32_t maxCells;}`. `LocalHost` builds the
authorizer and the probe from one function, `grantLimits(programRef)`, so they cannot drift. The
pre-flight compares the plan's work and cell counts with the probe's limits, and the cell count with
`settings_.epistemic.cells`. A host with no probe cannot decide the grant-dependent half, so the
pre-flight decides only the `settings_` half and the apply-time path is the backstop: it ends in
`fatal(init)`, one WARN, and a stored refusal that the holder of the pid reads with
`AgentExecutor::lastRefusal(pid) -> std::optional<TransferRefusal>` (one entry per pid, erased by
`forget`). Every host in this tree installs the probe; only an out-of-tree host reaches the
backstop.

**A refusal leaves nothing behind.** For a T1 code the test reads `runtime().actorCount()`, the
issuer's issued count (through a counting `CapabilityIssuer` double), `needsFirstTurn_` (through
`pump` doing nothing), the trusted-node set and the retained-program registry, and expects all of
them as before. After the pre-flight, `trustNode` and `issue` run and then `runtime_.resume` can
still refuse with a lower-layer `Runtime` code (authorizer, hashcash, unknown node).
`MeshActorRuntime` has `trustNode` and no way to untrust (`vortex/mesh_actor.hpp:162`), and the
trust is the owner's own spawn node, implied by the owner's signature on the checkpoint, so it is
idempotent; the issued capability is an unused bearer token. Moving `trustNode` after
`runtime_.resume` is impossible because the runtime reads the trust set (`mesh_actor.cpp:228`). The
design therefore states the boundary instead of hiding it: the "no trace" test covers every T1 code;
a second test forces a `Runtime` refusal after the pre-flight and pins the exact residue (trust
unchanged for a known node, issued count plus one, no pid, no actor).

The resume plan runs again inside `restoreState`, because that is the copier. Both runs read the
same `*received` value, which `resume` moves into `runtime_.resume` (`agent_executor.cpp:5440`) and
`prepare` reads later, so a base cannot change between them and revision 1's "checkpoint changed
between two reads" case does not exist. What can fail at apply time is the work budget on a host
with no probe, a translator that returns a refusal, and the arena's own limits. Those end as today
in `fatal(kCauseInit)` with a WARN that prints `toString` of the refusal, and the refusal is stored
for `lastRefusal(pid)`. Planning twice doubles the read-only wall time at the 26k-cell bound. It
spends no grant: the pre-flight counts, it does not charge (section 4.5), and T1c measures the added
time with the budget-parity test and records it. Caching the pre-flight's plan for `restoreState` is
refused: the plan borrows the received base, which only exists once the pid does.

### 2.4 Tests

Each asserts `refusal.error().code`, `.detail.subject`, `.detail.saved`, `.detail.current` and the
numbers, never a log line (section 9 lists them). Every one also checks that nothing changed: no pid
for `resume`; for `upgrade` the context pointer, the grant, the latch, both program bases, the
arena's cell count and the retained-program registry.

## 3. The shape version and its guard

### 3.1 The header

`kEpistemicShapeVersion = 1`, in a new `vagent/runtime/epistemic_shape.hpp` together with the label
`kShapeHeaderName = "EpistemicShape"` and the field name `version`. The header is a record of three
cells: the head cell (content `EpistemicShape`), a `version` label on `d.vars` and an Int64 scalar
on `d.values`, the same shape as the policy master's fields (`compiler.cpp:3183-3230`). It is the
first cell of the agent cell's `d.epistemic` rank.

- **Minted by `Gen`** when `anyArbitrated`, a flag set while the predicate loop runs
  (`compiler.cpp:3148-3175`). The cells live in the program base, like the policy masters, so they
  are not in any actor's epistemic cell count and not in the ceiling. `compiler.cpp` gains at most
  100 lines, in a new out-of-class member in `epistemic_emit_shape.cpp` plus one call.
- **An agent with no arbitrated predicate has no header.** Its `d.epistemic` rank stays empty, and
  nothing changes for it.
- **Shape 1 is defined by T1b1, and nothing persisted before it is supported.** The header, the
  master's `encoding` and `signature` fields and the probe arrive in one slice, so there is no
  window in which a master has the new layout and no version, and no state to refuse as
  `UnknownShapeVersion` that an earlier slice wrote. T1a changes no persisted layout. A store, a
  checkpoint or a sample written before T1b1 and holding P19 state refuses by number, which is the
  AGENTS.md rule (bump, write the new shape, delete the old reader) applied from the first version.
- **The number changes only when the shape does.** Any slice that renames a field, changes a field's
  kind, moves a rank or changes a meaning bumps it, as the plan says. An additive field whose
  absence reads as a documented initial value does not. When it is bumped the old reader is deleted,
  so a state saved under the old number refuses by number.
- **Compared, never copied.** The new program's `instantiate` already holds its own header. Neither
  copier follows the saved one. `translateEpistemicState` reads the saved number and compares it
  with the new agent's by value. A test asserts that after a resume and after an upgrade the header
  cell of the new agent is the program's own cell, by ref.
- **The master gains two fields.** `mintPolicyMaster` (`compiler.cpp:3183`) adds `encoding` (the
  text of the tag that `canonicalPolicy` starts with, `epistemic_policy.cpp:343`, today
  `vagent-policy-2`) and `signature` (section 4.3). The checkpoint stores only the hash today
  (`compiler.cpp:3228`), so a tag change is indistinguishable from a field change and the plan's
  "tag readable from the checkpoint" is not true. The two fields are part of shape 1. They add four
  cells a predicate to the base.

### 3.2 The guard

```cpp
struct EpistemicStateProbe {
  std::optional<std::int64_t> shape;   // the header's version, absent when there is no header
  bool populated{false};               // anything beyond the header
};
[[nodiscard]] EpistemicStateProbe probeEpistemicState(const zigzag::ArenaManifold &,
                                                      zigzag::CellRef agent, const AgentDims &);
[[nodiscard]] bool hasTranslatableEpistemicState(const EpistemicStateProbe &,
                                                 const TranslatorRegistry &) noexcept;
```

`populated` is true when the agent cell links `d.trust` (any profile, a tombstone included) or links
`d.epistemic` to anything other than the header. A record cannot exist without its profile (a record
holds its profile's clone occurrence, `kObsProfile`, `runtime_abi.hpp:599`), so support histories
need no walk of their own; a test builds a record without a profile and expects `StateInconsistent`.
`hasTranslatableEpistemicState` is true when `!populated`, or when `shape` equals a number the
registry has a translator for. It never reads the header's cells as state.

Before T1b1 the T1a pre-flight uses the root test that exists, replaced in place: `upgradeRefusal`'s
`translates` computation and the `linked(trust) || linked(epistemic)` test
(`agent_executor.cpp:5473-5478`) move into `preflightUpgrade` unchanged. T1b1 replaces that test
with `hasTranslatableEpistemicState`. Later slices change the registry and the probe, not each call
site.

| Agent                                                   | `shape` | `populated` | Result                                                   |
| ------------------------------------------------------- | ------- | ----------- | -------------------------------------------------------- |
| no arbitrated predicate                                 | absent  | false       | translatable; upgrades as before                         |
| arbitrating, no source yet                              | 1       | false       | translatable; the header does not make it populated      |
| arbitrating, profiles                                   | 1       | true        | translatable if shape 1 has a translator (T1c)           |
| arbitrating, profiles, saved shape differs from program | 2       | true        | `UnknownShapeVersion{savedCount=2, currentCount=1}`      |
| profiles, no header (written before T1b1)               | absent  | true        | `UnknownShapeVersion{savedCount absent, currentCount=1}` |

### 3.3 Intentions

A supported agent is one with no active intention (O1(b), A5). Today only the schema road refuses
one (`agent_executor.cpp:5503`); the reconciler road copies intentions (`reconciler.cpp:839-936`)
and `restoreState` copies them (`agent_executor.cpp:5248-5256`). T1a types the schema road's
refusal. T1b1 extends it, for a program whose shape header exists, to all three roads.
Non-arbitrating agents keep resuming and reconciling with intentions, so nothing outside P19
regresses. Intentions may later head query tokens (plan O3), and T1 has no translator for that
coupling. This extends A5 to resume; it is proposed as A12 (section 10).

## 4. The routine shared by resume and upgrade

### 4.1 The copiers today and what is shared

| Copier                                               | Entry                                                                                           | Copies                                                                                                                                                                                          | P19 state                                                                                                                                         | Reports refusal as                         |
| ---------------------------------------------------- | ----------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------ |
| `restoreState` (resume)                              | `agent_executor.cpp:5049`, from `prepare` `:846` (call `:888`) on the first turn of the new pid | scalar fields overwritten, structured fields cloned, profiles, belief clauses with supports and their `d.epistemic` histories, spaces, holons, intentions; dimensions of the base found by name | profiles and histories copied; `last_receive`, `last_contact` cleared (`:5165-5168`); policy hash compared by predicate name (`:5181-5215`)       | `std::runtime_error`, DEBUG, `fatal(init)` |
| `UpgradeGraph` and the translator seam (schema road) | `agent_executor.cpp:5483`; class `:319`; seam call `:5745`                                      | fields by schema (add, drop, default), clauses and supports by closure, hooks; `d.epistemic` and `d.trust` skipped (`:385`)                                                                     | none at HEAD: no translator is registered anywhere but one test (`tests/xuzz/vagent_epistemic_ingress_test.cpp:1495`); T0 refuses populated roots | `ActorError`                               |
| `executeReconcile` (reconciler road)                 | `agent_executor.cpp:5835`; `vagent/runtime/reconciler.cpp:372,1058`                             | clauses (`termFrom`: grab, step, source, `reconciler.cpp:484`), intentions, asks, outbox                                                                                                        | none: no `d.trust`, no `d.epistemic`; policy never compared; T0 refuses populated roots                                                           | `ActorError`                               |

They stay three. They differ in what an upgrade means: resume overwrites a fresh agent from a saved
image, the schema road reconciles two schemas and runs `on Upgrade`, and the reconciler road carries
asks and staged effects. Collapsing them is the P17 pipeline, which the spec does not claim (line
205, "does not claim an automatic full P17 upgrader already exists"). What can be shared, and is, is
the half they all treat as foreign: everything under `d.trust`, `d.epistemic`, the profile `d.clone`
chains and the support histories. One routine owns that half. The rule for the generic halves is
that they must not follow those dimensions:

- `restoreState` drops `d.epistemic` and `d.clone` from its `belief` and `epistemic` pair lists
  (`:5150-5180`) and no longer copies profiles or compares policy. It copies clauses with their
  supports as now, with the `copies` map shared, and then calls the routine.
- `UpgradeGraph` already skips both dimensions; its `translation()` map (`:435`) is the routine's
  starting map.
- The reconciler road refuses populated state (section 4.6).

### 4.2 Signature

```cpp
enum class Disposition : std::uint8_t { Resume, Upgrade };
enum class ClockContinuity : std::uint8_t { Same, Changed };

struct DimBinding {                         // role -> dimension cell, in one arena
  zigzag::DimRef vars, values, grab, step, clone, clause, source, belief, trust, epistemic;
  static DimBinding byName(const zigzag::ArenaManifold &, zigzag::CellRef root);   // resume
  static DimBinding registered(const AgentDims &, const zigzag::vortex::VortexCoreDims &);  // upgrade
};

struct TransferBudget {                     // shared with the caller's copier
  std::uint64_t &work;                      // remaining instruction grant, charged per step
  std::uint32_t maxCells;                   // new arena's allowance
  std::size_t maxBytes;
  std::size_t epistemicCells;               // settings_.epistemic.cells
};

struct PlanRequest {                        // everything the read-only pre-flight needs
  Disposition disposition;
  ClockContinuity clock;                    // Same on in-process upgrade, Changed on resume
  const zigzag::ArenaManifold &from; zigzag::CellRef fromAgent; DimBinding fromDims;
  const UpgradeSchema &intoSchema;          // the new program's typed predicates
  const TranslatorRegistry &registry;
  TransferBudget budget;
};

struct ApplyRequest {                       // the plan's request plus the writable half
  const PlanRequest &plan;
  zigzag::ArenaManifold &into; zigzag::CellRef intoAgent; DimBinding intoDims;
  std::unordered_map<zigzag::CellRef, zigzag::CellRef> &map;   // old -> new; the caller seeds it
};

[[nodiscard]] std::expected<TranslationPlan, TransferRefusal>
planTranslation(const PlanRequest &);                    // read-only: the pre-flight
[[nodiscard]] std::expected<TranslationReport, TransferRefusal>
applyTranslation(const TranslationPlan &, const ApplyRequest &);
```

Two request types replace revision 1's single request with an "optional" reference member: a
reference member cannot be optional, and a member that is optional by convention is the sentinel
pattern AGENTS.md forbids. `planTranslation` cannot be handed an `into` it does not need, and
`applyTranslation` cannot be called without one. `applyTranslation` is the only writer. It targets a
fresh context that the caller discards on any refusal, so it needs no undo of its own: the schema
road's candidate `State` and context are swapped only at commit (`:5806`), and a resume that refuses
at apply time ends the new actor as today. The old arena is never written.

The routine carries no executor pointer. `U-3` asks for "checked clock and identity bindings": the
request holds `ClockContinuity`, and the profile tokens it carries are checked with
`isPrincipalToken` (`agent_executor.cpp:3536`). The executor's owner fingerprint is not an input in
T1: a principal token names a source, not the operator, and the plan has no use for the owner yet.
I3-4's issuer records are the first consumer and add the field then.

The seam `OwnedStateTranslator` (`agent_executor.hpp:302-307`) takes `ApplyRequest&` and returns
`std::expected<void, TransferRefusal>`. T1c registers the default at construction.
`setOwnedStateTranslator` already chains (`:308`) and stays so a test can inject a failing one for
the rollback test.

### 4.3 Full-key compatibility

```cpp
struct PredicateKey {
  std::string signature;                       // canonical: name, arity, ordered types, key positions
  std::optional<ArbitrationPolicy> policy;     // typed; absent = not arbitrated
  bool operator==(const PredicateKey &) const = default;
};
```

- **The signature** is one canonical text, `vagent-sig-1\0name\0arity\0t0,t1,...\0k0,k1,...`,
  written by `Gen` into the master's `signature` field and rebuilt for the new program from its
  `UpgradeSchema` (`UpgradePredicate::parameters` with `type` and `key`, `upgrade_schema.hpp:41`). A
  text compare is a compare of the full typed values.
- **The policy** is read back from the master's typed cells (strategy, model, doubles by their value
  bits, durations, optional fields by absence) into an `ArbitrationPolicy` and compared with its
  defaulted `operator==` (`epistemic_policy.hpp:78`). `policyHash(decoded)` must equal the master's
  stored `hash` or the master is corrupt (`StateInconsistent`). A round-trip test pins
  `decode(mint(p)) == p` for a policy with every optional field present and for one with none.
  Frame, units, the model name and the priors are policy fields, so "domain/unit/frame schema" is in
  the key. The feature schema, the model, calibration and policy epochs and the source incarnation
  are not state T1 has: the first three arrive with I4-1b and I5-3 and extend `PredicateKey` in
  their slices; the incarnation is a profile scalar that the routine carries unchanged and the arena
  keeps as a floor.
- **Both sides are decoded by the same function.** The old key comes from the saved master (resume)
  or from the old arena's master (upgrade, which could also use the old `UpgradeSchema` but does
  not, so one function is tested). The new key comes from the new program's schema. A display-name
  match is never enough: matching is by `PredicateKey::operator==`.
- **A hash is an accelerator.** The matcher indexes the new program's keys in
  `std::unordered_map<std::uint64_t, std::vector<std::size_t>>` under `KeyDigest`, the first 64 bits
  of SHA-256 of the signature, and scans the bucket with `operator==`. `KeyDigest` is a template
  parameter of the matcher, defaulted to that function. The collision test instantiates it with a
  constant and expects the same matches as the default, and that two predicates with different
  signatures are never matched (the spec's "forced full-key hash collisions do not match").
- **Outcomes.** `Compatible`, `Incompatible{part}` with the codes of section 2.2, and `Migratable`,
  which T1 never returns. T2 gives it content (section 8.5), so the matcher's type is final now.
- **A predicate the new program lacks.** The decision is made from the pair of schemas and the live
  bindings, not from saved state, because a binding creates no profile (`bindSource` snapshots the
  program, `agent_executor.cpp:3626-3632`) and a fresh agent has a header and nothing else. For
  every predicate of the old program: if it is arbitrated (`UpgradePredicate::epistemicPolicy`
  engaged, `upgrade_schema.hpp:41-45`) or has a live binding in `Ingress::bindings` for this pid,
  and the new program does not declare it, the answer is `PredicateRemoved`. A plain predicate with
  neither is dropped as today (`upgradeChangeRequiresRefusal` returns false for `RemovedPredicate`,
  `upgrade_schema.cpp:720-735`, and stays so for it). A policy gained or lost is `PolicyChanged`
  with one side absent, the same refusal the schema comparison gives today
  (`upgrade_schema.cpp:709-717`), now typed and applied on the reconciler road too. On resume the
  old key comes from the saved master and bindings do not exist (they are host memory), so the rule
  is: a saved master whose predicate the new program lacks is `PredicateRemoved`.

### 4.4 The passes

One map, `old -> new`, built in bounded passes. The caller seeds it with what the generic copier
made (clauses, terms, supports, fields). Every loop has a count from the plan, charges `work` per
step through `into.consumeWork()` as `UpgradeGraph::charge` does (`agent_executor.cpp:332`), uses an
explicit stack, and ends with a check that it copied what the plan said (`StateInconsistent` if
not).

| Pass | Spec 2.2 item | Reads (old)                                                                                                          | Writes (new)                                                                                                                                                                                          | Plan count        |
| ---- | ------------- | -------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------- |
| 0    | 1 (resolve)   | masters and keys, header, profile ranks, support ranks                                                               | nothing                                                                                                                                                                                               | all counts, cells |
| 1    | 1             | each profile master: scalars, `streams`, `dedup`, health scalars, `reasons`, `actions`, its `d.clone` chain of views | the profile master and its fields and lists; its views; `map[old] = new` for each; the new `d.trust` rank in order                                                                                    | profiles, views   |
| 2    | 2             | per matched predicate, per clause, per support (already in `map`), its `d.epistemic` rank of records                 | each record (all positional and named fields, `retired_by` included, status and verdict unchanged), re-linked to the mapped support in the old order, its `profile` field pointing at the mapped view | records           |
| 3    | 3             | every owned edge of every old cell in the map                                                                        | verification only in T1: each edge's two ends are mapped and the new cells hold the same edge. Later slices copy tokens and intentions here, last                                                     | edges             |
| 4    | 4             | -                                                                                                                    | `Resume` clears `last_receive`, `last_contact`. `Upgrade` leaves them. Later slices add the continuation and normalizer rules of item 4                                                               | profiles          |

Pass 1 runs before pass 2 because a record's `profile` field names a view on its profile's clone
chain (`compiler.cpp:3725`). Passes 1 and 2 are the "descriptors first, then supports and
observations" order of the spec; the health records of I3-2 (`reset_epoch`, `action_seq`,
`fault_seq`, `health_cells`, `reasons`, `actions`, `epistemic_abi_health.hpp:89-100`) hang on the
profile and move with pass 1. A field the registry does not list for a profile is not copied
silently: pass 0 refuses `UnregisteredOwnedStructure`.

The routine never copies a raw old `CellRef` as a value. A `CellRef` appears only as a map key, and
a field that holds text, an Int64, a Double or a Bool is copied with `canonicalDoubleBits`
untouched: the stored bits move as they are, so `-0.0` stays `-0.0`.

### 4.5 Price and the refusals that bound it

- **Cells.** The owned graph is bounded by `vagent.epistemic_cells = 24,576`
  (`epistemic_limits.hpp:42`) plus the exempt headroom `cells / 16 = 1,536` of R3-1: 26,112 cells.
  `/tmp/p19-t1d/price.py` computes the rest.
- **Checkpoint limit.** A checkpoint encodes about two operations a cell. 26,112 cells are 52,224
  operations of the 65,536 of `vagent.value_operations` (`agent_executor.hpp:77`), leaving 13,312
  (20.3%) for the rest of the agent. The comment at `epistemic_limits.hpp:41` records "24k cells
  checkpoint and 32k do not". A resume onto an executor whose `epistemic.cells` is lower than what
  the image holds refuses
  `PeakCopyBudget{savedCount = cells in image, limit = ceiling + ceiling/16}` and does not truncate.
- **Peak copy.** Resume holds the base (the image) and the copy: 2 x 26,112 = 52,224 cells of arena.
  An in-process upgrade holds the old graph and the new copy: the same. `UpgradeGraph` already
  refuses a closure above `maxCells` (`:345,362`); the routine adds the check against the plan
  before pass 1, so the refusal comes before any write.
- **Workspace.** The map costs about 40 bytes an entry with its buckets: 1.2 MiB at the bound.
- **Work.** About 14 charges a cell over the passes: 360,000 for a full graph. That is 3.6% of the
  default grant (10,000,000, `program.hpp:217`) and 36% of the TUI's grant (1,000,000,
  `agent_tui.cpp:640`), under the plan's half-grant rule. These are estimates. T1c measures them
  with the budget-parity test and records the figures; a worst case above half of the TUI grant
  moves the TUI to a larger grant rather than loosening the rule.
- **Refusals.** `PeakCopyBudget` and `WorkBudget` are decided in pass 0 against the limits of the
  grant that will be issued. On the upgrade roads that is the replacement grant already in hand
  (`upgrade(pid, program, grant)`, or the grant the reconciler road copies from the old state). On
  resume it is the probe of section 2.3. In both cases they leave the old actor, context and grant
  as they were and, on resume, no pid exists. A test on each road uses a grant one instruction below
  the plan.

### 4.6 The two upgrade roads, and the deliberate differences

| Aspect                 | Resume                                                                                     | Upgrade                                                                                  | Why                                                                                                           |
| ---------------------- | ------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| Old graph              | received base, viewed by a bare `ArenaManifold`; dimensions found by name                  | the live old context's arena; dimensions by ref                                          | the rebuilt base registers none (`agent_executor.cpp:5056-5058`)                                              |
| Old program known      | no, only the image                                                                         | yes                                                                                      | keys are decoded from the graph on both, so one function is tested                                            |
| Clock stamps           | cleared                                                                                    | kept                                                                                     | the executor's clock instance changes on resume (`Ingress::clockInstance`, `agent_executor_internal.hpp:405`) |
| Policy or key mismatch | refused before a pid                                                                       | refused before mutation                                                                  | the same compare                                                                                              |
| Ranked continuations   | same program: preserved and their versions checked (later slices)                          | invalidated with `StaleEpistemicQuery` (later slices)                                    | spec 2.2 item 4                                                                                               |
| Pid and grant          | new pid, new grant from the issuer                                                         | same pid, replacement grant                                                              | `resume` mints, `upgrade` keeps                                                                               |
| Non-epistemic extras   | `turn_count` raised to wall milliseconds, pending requests fail as migrated                | requests, flights and staged effects refuse the schema road                              | unchanged by T1                                                                                               |
| Admission              | none to close: the new pid has no host state                                               | closed for the swap (section 5)                                                          | posts can reach a live pid                                                                                    |
| Failure                | pre-flight refuses; apply-time failure ends the new actor with a WARN and a stored refusal | pre-flight refuses; apply-time failure discards the candidate; context swapped at commit | `runtime_.upgrade` installs the new context only when the callback succeeded (`mesh_actor.cpp:801-815`)       |

**The reconciler road and the by-value overload.** The reconciler road does not run the routine. It
refuses populated state with `RouteCarriesNoEpistemicState` (T0's rule that it refuses whether or
not a translator is registered stays, `agent_executor.cpp:5473-5478`). Revision 1 forwarded
`LocalHost::upgrade(pid, CompiledProgram)` to the schema road so that the two public entry points
agreed. That removed, for every agent and not only for P19 agents, the carry of in-flight asks and
staged effects, which is the reason the reconciler road exists (`reconciler.cpp:372`), and it made a
public API (`program.cpp:404-409`) answer `NotQuiescent` where it used to carry. Agreement needs a
shared decision, not a shared road, so the by-value overload stays on the reconciler road and both
overloads call the one `preflightUpgrade` first:

- **Shared, identical on both roads:** the missing-program and reference check, the host quiescence
  of section 5.1 that is not road-specific, `ActiveIntention` (for an arbitrating agent), shape,
  policy encoding, policy, signature and removal (section 4.3, computed from the two
  `UpgradeSchema`s and the bindings, so the reconciler road gains the comparison it never had).
- **Road-specific, by `UpgradeRoute`:** the schema road's flights, requests, outstanding deliveries,
  staged outbox and pending entries; the reconciler road's `RouteCarriesNoEpistemicState` for
  populated state.

So `ThePublicEntryPointsAgree` compares, for the same P19 state, the two overloads' codes: equal for
every cause that is not the road itself, and for populated state the by-value overload answers
`RouteCarriesNoEpistemicState` with `detail.current` naming "the shared-pointer overload", while the
pointer overload accepts (T1c) or answers `NoTranslator`. That one difference is intended, typed and
documented in the user guide. A second test pins that a non-epistemic agent with an ask in flight
still upgrades by value and the ask is carried. The by-value overload also fixes two defects of
`LocalHost::retainProgram` (`program.cpp:369-381`), which returns the already registered program
when the reference exists and registers a new one before anything is checked:

- It runs `preflightUpgrade` on the unretained value first. A pre-flight refusal therefore leaves
  `programs_` and the executor's registry as they were. A refusal after the pre-flight (the apply
  time of the reconciler road) can leave the program retained; a retained version is authorized for
  resume by design, and the test states it.
- When the reference is already retained it compares the incoming `UpgradeSchema` with the retained
  one. A difference is `ProgramReferenceReused{subject = reference}`, not the silent upgrade to the
  old object that the shared-pointer overload refuses as `NotAuthorized`
  (`resolveProgram(...).get() != program.get()`, `agent_executor.cpp:5490`). An equal schema returns
  the held program, as the documented rule of `retainProgram` says; a change to bodies under one
  reference with an equal schema is the caller's breach of that rule and is not detectable here.

### 4.7 The digest

`epistemicStateDigest(arena, agent, dims, DigestMode)` returns SHA-256 hex of a canonical stream:
profiles in rank order; per profile its fields in rank order as (name, kind byte, payload); lists
and records recursively; supports in clause order with their records; clone occurrences by position
and not by ref. Text goes as length-prefixed bytes; Int64, Bool and Double as the eight stored value
bits (not `canonicalDoubleBits`, so `-0.0` and `0.0` differ); a link is not hashed, only the order
it implies. `DigestMode::ModuloClock` omits `last_receive` and `last_contact` and the header. The
property test (section 9) compares the digest of the source actor, of its resume and of its in-place
upgrade, all `ModuloClock`.

## 5. Quiescence, admission and the operator path

### 5.1 One report

`AgentExecutor::quiescence(pid) const -> std::expected<QuiescenceReport, TransferRefusal>`. Revision
1 modelled one queue and one lock. I3-2c1 added a second of each: `pendingHealth_`,
`healthOutcomeRing_` and `healthActionSeq_` under `healthMutex_` (`agent_executor.hpp:870-877`), and
a ticket is inserted by `healthAction` before its control item is pushed
(`agent_executor_health.cpp:243,286`), so `PostBox::pendingFor` does not see a ticket whose item has
been consumed but whose outcome is not yet recorded. The report covers all of it:

| Field                           | Source at HEAD                                                                                                    | Refuses on which road          |
| ------------------------------- | ----------------------------------------------------------------------------------------------------------------- | ------------------------------ |
| `turnInFlight`                  | `State::inTurn`                                                                                                   | both                           |
| `hostEvents`                    | `State::hostEvents.size()` (settle and discard events included, I3-2b)                                            | both                           |
| `posts`                         | `PostBox::pendingFor(pid)` (`agent_executor_internal.hpp:261`: observations, notes, source changes, health items) | both                           |
| `pendingHealth`                 | `pendingHealth_` entries whose `target == pid`, under `healthMutex_`                                              | both                           |
| `inFlight`, `queued`            | `Ingress::inFlight`, `Ingress::queued` for the pid: asserted empty at quiescence                                  | both                           |
| `choiceDepth`, `marks`          | `vm.choiceDepth()`, `arena.outstandingMarks()`                                                                    | both                           |
| `mailbox`                       | `runtime_.pending(pid)` (`agent_executor.cpp:5337` is the checkpoint's use)                                       | arbitrating agents, both roads |
| `flights`, `requests`           | `State::flights`, `State::requests`                                                                               | schema road                    |
| `deliveries`                    | `outstanding_` entries owned by the pid                                                                           | schema road                    |
| `stagedOutbox`, `pendingSettle` | `linked(agent, d.outbox)`, `linked(agent, d.pending)`                                                             | schema road                    |
| `admission`                     | `Open`, `Paused` or `Swapping`                                                                                    | informational                  |

`inFlight` and `queued` are not independent queues: a reading enters both when it is pushed
(`agent_executor.cpp:3970-3978`) and leaves both when its turn completes, so they are empty whenever
`posts`, `hostEvents` and `turnInFlight` are. The design does not assume it: the report lists them,
and `InFlightIsEmptyAtQuiescence` asserts it after a drain, so a future path that leaves one behind
fails a test and refuses an upgrade rather than carrying a stale generation across a swap.

`upgradeRefusal` (`agent_executor.cpp:5449`) is rewritten to build this report and return
`NotQuiescent{subject = field name, savedCount = value}` for the first non-zero field in the order
of the table. The checks are those of T0 (`:5453-5456` and the schema block `:5459-5470`) plus the
new fields. The checkpoint's own host-delivery test (`:5328`) is untouched: a `pendingHealth_` entry
is erased with the item that completes it (`agent_executor_health.cpp:598`), so a checkpoint that
sees `posts` empty sees no ticket either; a T1b2 test pins that.

The report is computed by one function, `quiescenceLocked(pid, const HostLocks &)`, that takes a
proof that the ingress and health mutexes are held. The advisory `quiescence(pid)` takes them and
calls it; the gate of section 5.2 calls it with its own. There is therefore no second copy of the
rules to drift.

The mesh mailbox is the third queue. At HEAD the schema road ignores it unless an event layout
changed (`:5534-5546`); the reconciler road ignores it. For an arbitrating agent T1 refuses any
non-empty mailbox: I3-4's verification messages arrive there, and whether intake is
program-independent is V-D's to settle. The refusal carries the count, so a controller can tell a
mailbox that is draining from one that is not. The executor cannot close the mesh mailbox, and
revision 1's argument that it needs no gate rested on "`upgrade` and `receive` run on the same
thread" without a citation. What the code shows is narrower: `MeshActorRuntime` has no lock, and its
`receive` is called from `ClusterNode` (`vagent/cluster/cluster_node.cpp:544`). T1 therefore states
the obligation instead of the claim: the host serialises `receive` and `upgrade` (a header comment
on `upgrade`, and the same words in the user guide); `LocalHost` has no second thread and the test
shows the count; whether `ClusterNode`'s receive path shares the pump's thread is a finding for the
I3-4 review, which is the first slice with mesh traffic to an arbitrating pid. A test sends a
message through `runtime().receive`, expects `NotQuiescent{mailbox, 1}`, pumps, and then upgrades. A
`holdMailbox` effect on the pause (delay, not drop) is declined: it is a change to the mesh runtime,
which needs its own approval and design, and the count above makes the limit visible without it.

### 5.2 The post that races the swap

Posts come from other threads: `observe`, `bindSource`, `retireSource`, `healthAction`,
`healthActionBatch` and `percept`. The locks are the ingress mutex, `healthMutex_` and the post-box
mutex, and the order is **ingress, then `healthMutex_`, then post box**. The code shows each pair:
`revokeHealthGrant` takes ingress then health (`agent_executor_health.cpp:129,139`); `observe`,
`bindSource` and `retireSource` take ingress and push under it
(`agent_executor.cpp:3979,3677,3706`); no site in `agent_executor*.cpp` takes ingress while holding
`healthMutex_` or the post-box mutex (T1b2 re-greps this and writes the order as a comment above the
member declarations at `agent_executor.hpp:870`). At HEAD `pendingFor` is read, the swap runs, and a
post made in between is consumed by the new program, with an item that was checked against the old
binding.

**The invariant.** Admission is tested in the same lock scope as the push, for every call. Not
"before reading a binding": revision 1 put the flag read at the top and called the result complete,
but `observe` has two critical sections with unlocked work between them (`:3768-3781` reads the
binding and the latch and releases; `:3956-3979` takes the fair-share test and pushes), and
`healthAction` matches its grant, takes `healthMutex_`, inserts its ticket and pushes in four
separate scopes (`agent_executor_health.cpp:159-185,205,243,286`). A flag read at entry can be
`Open` while the swap sets it before the push lands.

`Ingress` gains `admission[pid]`: `{pausedUntil, pauseOpenedBy, swapping}`. The gate, in one place:

```cpp
// agent_executor_internal.hpp: the one way an upgrade closes the door.
class SwapGate {                       // RAII; constructed only by AgentExecutor::acquireSwap
 public:
  ~SwapGate();                         // clears `swapping` on every exit, in the destructor
  SwapGate(SwapGate &&) noexcept;      // move-only
};
// acquireSwap: lock ingress, lock healthMutex_, quiescenceLocked(pid) and the bound-predicate
// comparison of section 4.3 (both under the locks), and only when quiet set `swapping`.
std::expected<SwapGate, TransferRefusal> acquireSwap(const ActorPid &, UpgradeRoute, bool requirePaused);
```

Every admitting call tests `admission` in the scope that pushes:

| Call                                | Where the test sits                                                                                         | Early-out                        |
| ----------------------------------- | ----------------------------------------------------------------------------------------------------------- | -------------------------------- |
| `observe`                           | the second section (`:3956`), before the fair-share test and in the scope of `posts_->push` (`:3979`)       | the first section (`:3768`)      |
| `bindSource`, `retireSource`        | the top of the existing scope that already holds ingress through `pushControl` (`:3634-3677`, `:3688-3706`) | none                             |
| `healthAction`, `healthActionBatch` | one commit scope, below                                                                                     | at entry, before the grant match |
| `unbindSource`                      | not gated: revocation must always succeed, and it takes the ingress lock, so it is ordered against the gate | not applicable                   |
| `percept`, effect results           | not epistemic; unchanged from T0                                                                            | not applicable                   |

A refused call returns `EpistemicRefusal::AdmissionClosed` (new, appended,
`OutcomeClass::Retryable`). It is not logged to the outcome log, not charged to the source and not
posted as a refusal note: the host closed the door, the device did nothing. In `observe` it does not
go through the `refuse` lambda, which logs and charges. A post is therefore either before the gate
(it is in `posts` or `pendingHealth`, and the upgrade refuses `NotQuiescent`) or after it (it is
refused). There is no third case because the two sides take the same mutex.

**The health commit scope.** `healthAction` and `healthActionBatch` become: validate and match the
grant (as now, its own scope), compute the reset snapshot read-only, then one scope that holds
ingress and then `healthMutex_` and, in this order: **(1)** the admission test; **(2)** the
back-pressure counts (`actorPending`, `grantPending`, the degrade/clear cap, `:205-232`); **(3)**
the receipt (`nextHealthCounter_++`, `:234`); **(4)** the `pendingHealth_` insert (`:243`); **(5)**
for `Reset`, `requestReset`, the in-flight snapshot and the closed charge (`:260-290`); **(6)**
`pushControl`. Every return that refuses precedes step 4 and nothing after step 4 can refuse, so a
refused call creates no ticket (no leak against `healthPending` or `healthPendingPerGrant`), burns
no reset generation and no charge. `sourceHealth` takes the ingress lock itself (`:257`), so the
scope uses a `sourceHealthLocked` variant. Re-finding the grant by id inside the scope also removes
a read through `matchedGrant`, a pointer into a vector that `grantHealth` may reallocate after the
first scope releases (`:159-185`). The counter in step 3 is the only thing a refusal could burn if
it were minted earlier; minting it inside the scope avoids even that.

A test seam makes the races testable: `AgentExecutor::setSwapObserver(SwapObserver)` (a setter,
returns `AgentExecutor*`), a `std::function` (kept, so it is stored) with named points
(`AfterGrantMatch`, `BetweenObserveSections`, `AfterCommit`, `DuringDrain`) that is empty in
production and costs one test of a null function.
`APostBeforeTheGateIsCountedAndAPostAfterItIsRefused` runs the swap inside the observer at
`BetweenObserveSections`; the health test does the same at `AfterGrantMatch` and asserts
`pendingHealth_` size, `resetMarks.requested` and the back-pressure counts are unchanged.

| Post kind                    | Program-dependent?                                                              | Rule                                                                                        |
| ---------------------------- | ------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| Observation (`ingest`)       | yes: checked against the binding's policy, parameter kinds and domain           | gated in the push scope; before the gate it is counted in `posts`                           |
| Source change, health action | no: a principal and an incarnation or action, run by routines of the same shape | gated in the push scope, so the translation sees one state                                  |
| Refusal note                 | no: made only by `observe`, which is gated                                      | counted in `posts` if made earlier                                                          |
| Percept, effect result       | not epistemic                                                                   | unchanged from T0: kept for the new program; an effect result needs a flight, which refuses |

A percept posted against the old event layout during a swap is consumed by the new program. That is
the existing behaviour (`percept` returns `void` and has no refusal to give), it concerns no P19
state, and it is recorded as a limit in the user guide.

**Release order.** The gate lives until the end of the upgrade function, not until the commit. After
`found->second.swap(candidate)` (`agent_executor.cpp:5806`) the schema road runs a tail outside the
`try` and outside `runtime_.upgrade`: `clearLatchCache(pid)` (`:5810`), `liftLimits` (`:5812`),
`drain` (which runs a turn), `setCounter`, `flushStats` (`:5818`) and `deriveSchedule` (`:5819`);
the reconciler road has `clearLatchCache` at `:5884`. The `SwapGate` is a local declared before the
commit and destroyed after the tail, so no reading or health action is admitted against a latch
cache that predates the commit, or while the first post-swap drain runs. All-or-nothing holds up to
the swap and not after it. The tail is wrapped in a `catch` that logs at WARN under
`vagent.executor` and does not rethrow: the commit stands, so the call returns success, and the next
turn re-derives the schedule. A test with an observer that throws at `AfterCommit` asserts success,
the gate released and the admission reopened.

### 5.3 The operator path

An actor under steady ingestion is rarely quiescent, so the path is explicit:

1. `pauseAdmission(pid, ttl)`. It returns `std::expected<AgentExecutor *, TransferRefusal>`
   (`Runtime{UnknownProcess}` for a pid that is not live), so setters chain. `ttl` is
   `std::optional<std::chrono::milliseconds>`; absent means the setting `vagent.admission_pause_ms`
   (default 30,000, range 1,000 to 600,000; a `defaultSettingSpecs()` entry and an expected-list
   entry in `system_docs_test.cpp`). From then on the five calls above refuse `AdmissionClosed`.
   Readings already queued still run. The pause expires lazily: each admission call compares the
   steady clock with the deadline, so no timer thread exists, and a forgotten pause cannot close a
   source for ever.
1. Run the reactor until `quiescence(pid)->quiet()`. In tests that is `LocalHost::pump`; in a host
   it is its own loop. The executor does not drive the reactor.
1. `upgrade(...)`. A paused or swapping actor still upgrades; the swap gate is separate from the
   pause and is not subject to the ttl.
1. `resumeAdmission(pid)`. It returns `AgentExecutor *` and is idempotent (an unknown pid is a
   no-op), and it is also what `forget(pid)` does when an actor stops.

**The ttl does not run out under the drain.** A drain longer than 30 s would silently reopen
admission and might never become quiet, and nothing in the report says the pause lapsed.
`LocalHost::upgradeQuiescing(pid, program, timeout)` sets the pause deadline to
`max(its deadline, now + timeout)` at step 1, so the pause covers the whole drain. It then passes
`requirePaused = true` to `acquireSwap`, which refuses `NotQuiescent{admission}` if `admission` has
reopened since the pause (a lapse by another route, or a clock step), instead of upgrading an actor
that was admitting traffic. A plain `upgrade` passes `false`. A test advances a fake clock past the
default ttl in the middle of the drain and expects a committed upgrade; a second sets a short `ttl`,
calls `upgrade` with `requirePaused`, and expects the refusal.

`upgradeQuiescing` is steps 1 to 4 with `pump`, and it returns
`std::expected<void, TransferRefusal>`. It is the only form the tests and the user guide use for a
populated agent. Exposing the sequence on the `vagent` command line is W1's.

### 5.4 Host state, kept and lost

The pid survives an upgrade and a resume mints a new one, so the two lose different things. The
table is the registry's `kHostState` (section 7) and the user guide's table; every member of
`Ingress` and every health member of the executor appears once.

| State                                                | In-process upgrade                                                                                                                   | Resume                                                                                                                                                                                 |
| ---------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `bindings`, `aliases`                                | kept; D3 (section 6) says why they stay valid                                                                                        | lost; the host rebinds, and the arena's incarnation floor refuses a lower one                                                                                                          |
| `floors`                                             | kept                                                                                                                                 | lost; relearned from the first stale advance turn                                                                                                                                      |
| `outcomes`, `changes`                                | kept                                                                                                                                 | lost; a resend meets the arena's dedup window and answers `Duplicate`                                                                                                                  |
| `queued`, `queuedBy`, `queuedVia`, `inFlight`        | zero at quiescence; asserted, not assumed (`InFlightIsEmptyAtQuiescence`)                                                            | lost                                                                                                                                                                                   |
| `charges` (open), `uncounted`, `refusedWhileFaulted` | kept: counts of refusals decided before the swap, closed into a note under the new program; none reads program state; a test pins it | lost                                                                                                                                                                                   |
| `marks` (estimator replay marks, I3-2a, R3-5)        | kept                                                                                                                                 | lost; one replay can count again, the bound R3-5 states                                                                                                                                |
| `healthGrants` (by pid and principal)                | kept: they name the pid and the principal, not the program                                                                           | lost; operators re-grant                                                                                                                                                               |
| `resetMarks`                                         | kept: `requested` and `committed` order the readings across the swap                                                                 | lost; the arena's `reset_epoch` (`epistemic_abi_health.hpp:89`) is authoritative                                                                                                       |
| `pendingHealth_`                                     | zero for the pid at quiescence (`pendingHealth`)                                                                                     | refused earlier by `checkpoint` (`HostDeliveryPending`)                                                                                                                                |
| `healthOutcomeRing_`                                 | kept: `healthOutcome(receipt)` still answers after the swap                                                                          | lost with the executor                                                                                                                                                                 |
| `healthActionSeq_` (per pid)                         | kept; it numbers audit records only                                                                                                  | restarts at 1 for the new pid (`forgetIngress` erases it, `:4089`); the profile's `action_seq` (`:93`) carries and is authoritative; audit consumers key on (pid, clock instance, seq) |
| `latched` (the latch cache)                          | cleared by `clearLatchCache` (`:4082`) after the commit and before the gate opens                                                    | new executor's, empty                                                                                                                                                                  |
| `last_receive`, `last_contact` (arena)               | kept: the executor's clock instance is the same                                                                                      | cleared (`:5165-5168`)                                                                                                                                                                 |
| `clockInstance`, `wallSynchronised`                  | the executor's own                                                                                                                   | the new executor's own                                                                                                                                                                 |
| Audit sink, `nextHealthCounter_`                     | the executor's, kept                                                                                                                 | the new executor's                                                                                                                                                                     |
| Queued health actions, queued readings               | refuse the upgrade (`posts`)                                                                                                         | refused earlier by `checkpoint` (`HostDeliveryPending`)                                                                                                                                |
| Admission record                                     | kept until `resumeAdmission` or its ttl                                                                                              | not applicable                                                                                                                                                                         |
| Adapter `State`: flights, requests, `schedule`       | flights and requests refuse the schema road; schedule recomputed                                                                     | rebuilt; pending requests fail as migrated (`failMigrated`, `:4561`)                                                                                                                   |

The user guide gains this table (T1c). Nothing in the left column can admit a replay: the profile's
last sequence, dedup window and incarnation are arena state and move with the profile.

## 6. D3: what a policy change does to source bindings

T0 found that a binding copies the policy it was made under (`Ingress::Binding::policy` and
`domain`, `agent_executor_internal.hpp:300`, set at `agent_executor.cpp:3626-3632`) and survives an
upgrade. A binding is an authorization the operator gave a source to speak under one policy for a
lifetime and an alias. It is not a property of the program.

**Ruling.** T1 refuses, on every road, a policy change and the removal of a predicate that is
arbitrated or bound, so a binding cannot be stale after a commit. The schema road already refuses a
policy change (`UpgradeChangeKind::EpistemicPolicy`, `upgrade_schema.cpp:709-717`,
`upgradeChangeRequiresRefusal` `:720`), as an untyped `ExecutionFailed`; T1 types it
(`PolicyChanged`) and adds the reconciler road. Revision 1 wrote that the rule held "whether or not
the agent has a profile or a binding". That is true of the policy change and was false of removal:
`RemovedPredicate` is not in `upgradeChangeRequiresRefusal`, a binding needs no profile, and an
arbitrating agent with a bound source and no reading yet (`populated == false`, header only) would
upgrade to a program that drops the predicate and keep a binding with the old `policy`, `params` and
`domain` for a predicate the new program does not declare. The next `observe` would queue a reading
against it. The decision is therefore taken from the two schemas and the bindings, as section 4.3
says, inside `acquireSwap` so that a bind that lands between the check and the gate cannot escape
it: a binding made before is seen, one made after is refused `AdmissionClosed`. A plain predicate
that is neither arbitrated nor bound can still be removed.

**T2's migration revokes; it does not rebind and does not migrate in place.** When T2 accepts a
typed policy migration for a predicate, the commit unbinds every binding of the pid for that
predicate in the same ingress critical section as the swap, finishes queued items for them
`UnknownSource`, and keeps the profiles. The operator rebinds under the new policy. The reasons:

- Rebinding by the host would stamp the new `domain` and `policy` on readings from a source whose
  operator authorized the old one. That is an authorization nobody gave.
- Migrating the cached copy in place has the same defect with less visibility.
- Revoking costs one `bindSource` per source, which is idempotent for a live profile at the same
  incarnation (`Ingress::Floor::strict` is false for it, `agent_executor_internal.hpp:325-331`). The
  profile, its latch and its floor survive, so nothing is lost but the authorization.

A test in T1c asserts both halves of what T1 can: a changed policy is refused and the binding still
accepts a reading afterwards; an unchanged policy upgrades and the binding accepts without a rebind.
A test in T1a asserts the removal half on both roads with a bound, empty agent. The revoke path is
T2's, with a test that a migrated predicate's bindings are gone and its profiles are not.

## 7. The coverage oracle

A structure added without a translation must fail a test. Three checks, each doing one job:

1. **The registry.** Each area's ABI header declares what it owns, once, as constants:

   ```cpp
   struct OwnedStructure {
     std::string_view id;           // "profile.streams"
     std::string_view dimension;    // "d.trust"
     std::string_view where;        // "profile field", "profile list", "support rank", "predicate master"
     std::span<const std::string_view> fields;   // the ABI constants it covers (kProfileStreams, ...)
     Fate resume, upgrade;          // Carried, Derived, Dropped
     std::uint32_t since;           // shape version that introduced it
     void (*populate)(zigzag::ArenaManifold &, zigzag::CellRef profile);   // builds one sample
   };
   ```

   `Derived` means the new program re-mints it and the translator only compares it (the shape
   header, the policy master). `Dropped` is allowed only with a reason text (liveness stamps on
   resume). Headers: `runtime_abi.hpp` for the I3-1 structures (T1c adds the include), then
   `epistemic_abi_health.hpp`, `_calibration`, `_ledger`, `_verify`, `_query` as their slices create
   them (plan section 4.2). `epistemic_translate_registry.hpp` concatenates them into
   `allOwnedStructures()`. The ids are authored in the registry, not parsed from the headers: those
   hold bare `string_view` field names (`epistemic_abi_health.hpp:89-100`), so there is nothing to
   parse.

1. **A runtime check.** Pass 0 walks the old agent's graph along every dimension named `d.trust`,
   `d.epistemic` or `d.clone`, plus every field name of a profile or record, and refuses
   `UnregisteredOwnedStructure` for anything the registry does not own. A structure that a later
   slice adds and forgets to register therefore refuses a populated actor instead of being dropped
   by the next resume.

1. **The parity test and a script.** `ResumeAndUpgradeAgreeOverRandomIngestion` and
   `PopulatedStateSurvivesResumeAndUpgrade` iterate `allOwnedStructures()` and call each entry's
   `populate`, so a structure is covered by being registered and an entry with no `populate` fails
   `RegistryIsConsistent`. There is no `coversStructure` call and no gtest `Environment`: a filtered
   `make test TEST_FILTER=...` cannot produce a partial covered file because no covered file exists.
   `tools/check-translator-coverage.py` (new, T1c) is a source check and needs no test output: (a)
   the dimension names grepped from `ensureDimension("d....")` in `vagent/runtime/` against the
   registry's `dimension` fields plus an explicit `kNotOwned` list with one reason each (`d.outbox`,
   `d.belief` and so on); (b) every `inline constexpr std::string_view k...` field constant in the
   `epistemic_abi_*.hpp` headers and `runtime_abi.hpp`'s profile and observation sections appears in
   some entry's `fields`. It is a separate Makefile target, `test/translator-coverage`, run by
   `make test` after the gtest binaries and by the plan's iteration-end gate. `make lint` does not
   run it.

The registry also has one gtest that needs no other test: ids are unique, every `dimension` is one
`registerDimensions` makes (`runtime_abi.hpp:80`), every `Dropped` fate has a reason and every entry
has a `populate`.

The structures that exist at HEAD (profile rows from `runtime_abi.hpp:641-657` and
`epistemic_abi_health.hpp:89-100`; the other rows are from the compiler and the executor):

| Id                        | Where                                                                                                                                                                 | Resume               | Upgrade              |
| ------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------- | -------------------- |
| `agent.shape_header`      | agent `d.epistemic` head                                                                                                                                              | Derived              | Derived              |
| `agent.profiles`          | agent `d.trust` rank                                                                                                                                                  | Carried              | Carried              |
| `profile.scalars`         | `incarnation`, `faulted`, `rejection_streak`, `reject_count`, `content_refusals`, `refused_count`, `last_refusal`, `retired` (`runtime_abi.hpp:641-657`)              | Carried              | Carried              |
| `profile.liveness`        | `last_receive`, `last_contact`                                                                                                                                        | Dropped (new clock)  | Carried              |
| `profile.streams`         | `streams` record, per-predicate last sequence                                                                                                                         | Carried              | Carried              |
| `profile.dedup`           | `dedup` list of (incarnation, event, digest)                                                                                                                          | Carried              | Carried              |
| `profile.views`           | the profile master's `d.clone` chain, one view per record                                                                                                             | Carried              | Carried              |
| `profile.health_scalars`  | `reset_epoch`, `action_seq`, `fault_seq`, `health_cells`, the two Double weights, the `health` and `health_fraction` stamps (`epistemic_abi_health.hpp:16-26,89-100`) | Carried              | Carried              |
| `profile.reasons`         | `reasons` list with `origin`                                                                                                                                          | Carried              | Carried              |
| `profile.actions`         | `actions` list, the audit window                                                                                                                                      | Carried              | Carried              |
| `support.history`         | each support's `d.epistemic` rank of observation records                                                                                                              | Carried              | Carried              |
| `record.fields`           | the positional fields of `kObservationOrder` (`runtime_abi.hpp:607`) and the named ones, `retired_by` included                                                        | Carried              | Carried              |
| `profile.retired_chain`   | records unlinked from a support and kept to the `retained` bound                                                                                                      | Carried              | Carried              |
| `predicate.policy_master` | predicate `d.epistemic` head                                                                                                                                          | Derived and compared | Derived and compared |

Host-only state (section 5.4) has its own short list in the same file, `kHostState`, so the
user-guide table and the tests read one source.

## 8. Slices

Size: the plan estimated T1 at about 1,500 lines. This design needs about 3,400, so T1 splits at the
seams the plan allows. Each part builds, passes the earlier tests and is reviewed alone. T1b1 and
T1b2 are independent of each other: the gate (T1b2) does not need the header, and the shape guard
(T1b1) does not need the gate, so a defect in one cannot hold the other back, and they land in
either order after T1a.

### 8.1 T1a: the refusal channel (about 1,250 lines)

- **Content.** Section 2 in full, the pre-flights for the checks that exist today with the codes T1a
  can produce, the quiescence report without the gate and without the T1b2 fields (5.1 as of HEAD),
  the grant probe and `lastRefusal`, the shared `preflightUpgrade` with the schema-and-binding
  comparison (4.3), and the by-value overload's ordering fix (4.6). Resume refuses a changed policy
  before a pid (X-9); the schema road's `ExecutionFailed` causes become codes; the reconciler road
  compares policy, signature and removal; `LocalHost::upgrade(pid, CompiledProgram)` stays on the
  reconciler road. **No persisted layout changes.**
- **Files.** New `vagent/runtime/epistemic_refusal.{hpp,cpp}`;
  `vagent/runtime/agent_executor.{hpp,cpp}` (return types, `preflightResume`, `preflightUpgrade`,
  `quiescence`, `setResumeGrantProbe`, `lastRefusal`, at most 500 lines);
  `vagent/runtime/program.{hpp,cpp}` (`grantLimits`, the probe, the by-value ordering);
  `vagent/tui/agent_tui.cpp`; `vagent/runtime/upgrade_schema.{hpp,cpp}` (a typed result next to
  `upgradeChangeRequiresRefusal`, `PredicateRemoved` for an arbitrated or bound predicate); tests
  `tests/xuzz/vagent_epistemic_refusal_test.cpp` (new), `vagent_epistemic_resume_test.cpp`,
  `vagent_epistemic_ingress_test.cpp`, `vagent_live_upgrade_test.cpp`, `vagent_tui_test.cpp`.
- **Depends on.** T0, E1, I3-2. It may land before I3-2c2 if the lead wants X-9 fixed early.
- **C++ and Vortex.** All C++. The pre-flight is host admission, and the refusal type is the
  boundary between the executor and its callers. No Vortex code runs against an arena that is not an
  actor's live context.
- **Price.** No actor cells, no program-base cells, no format change.
- **Gate.** ARCH, CODE, DIST.

### 8.2 T1b1: the shape version and the guard (about 450 lines)

- **Content.** Section 3 (header, the master's `encoding` and `signature`, probe, guard, intention
  rule), `UnknownShapeVersion` and `PolicyEncodingChanged`, and the signature-based half of
  `PredicateIncompatible` on resume. Shape 1 is defined here.
- **Files.** New `vagent/runtime/epistemic_shape.hpp`, `epistemic_emit_shape.cpp`; `compiler.cpp`
  (the master's two fields, at most 40 lines, one call); `agent_executor.{hpp,cpp}` (probe and
  guard, at most 150 lines); tests `tests/xuzz/vagent_epistemic_shape_test.cpp` (new),
  `vagent_epistemic_ingress_test.cpp`; sample stores regenerated if any holds P19 state.
- **Depends on.** T1a.
- **C++ and Vortex.** C++; the header is emitted by the code generator, as every other agent cell
  is.
- **Price.** Three cells plus four a predicate in the program base per program with an arbitrated
  predicate. No actor cell.
- **Gate.** ARCH, CODE.

### 8.3 T1b2: admission (about 750 lines)

- **Content.** Sections 5.1 (the new fields), 5.2 and 5.3: `acquireSwap`, `SwapGate`, the per-call
  admission tests, the health commit scope, `AdmissionClosed`, `pauseAdmission`, `resumeAdmission`,
  `upgradeQuiescing`, the swap observer and the setting.
- **Files.** `agent_executor.{hpp,cpp}` and `agent_executor_health.cpp` (gate and restructured
  health commit, at most 450 lines); `agent_executor_internal.hpp` (`admission`, `SwapGate`);
  `epistemic_ingress.hpp` (`AdmissionClosed`, appended; `outcomeClassOf`); `program.{hpp,cpp}`;
  `system_docs.cpp` (`vagent.admission_pause_ms`); tests `vagent_epistemic_admission_test.cpp`
  (new), `tests/xuzz/system_docs_test.cpp`, `vagent_epistemic_health_test.cpp`,
  `vagent_epistemic_ingress_test.cpp`.
- **Depends on.** T1a.
- **C++ and Vortex.** C++ for the gate and the pause (thread admission, a clock).
- **Price.** No cells. A mutex acquisition order, written down.
- **Gate.** ARCH, DIST, SEC, CODE.

### 8.4 T1c: the routine, the digest and the oracle (about 1,000 lines)

- **Content.** Sections 4, 5.4 and 7: `translateEpistemicState`, the matcher, `discoverDimensions`,
  the digest, the default translator, the registry, the script, the user-guide table. `restoreState`
  loses its profile and policy code and calls the routine. The seam's signature changes (4.2).
- **Files.** New `vagent/runtime/epistemic_translate.{hpp,cpp}`, `epistemic_translate_registry.hpp`,
  `tools/check-translator-coverage.py`, `tests/xuzz/vagent_epistemic_translate_test.cpp`;
  `agent_executor.{hpp,cpp}` (calls, at most 300 lines); `runtime_abi.hpp` (the owned list and one
  include); `docs/vagent-user-guide.md` (host-state table and the operator path); `Makefile`
  (`test/translator-coverage`, run by `test`).
- **Depends on.** T1a, T1b1, T1b2. I3-2c2 if it has not shipped the rows the registry lists; the
  health rows are the code at HEAD, not the design.
- **C++ and Vortex.** All C++, with the same reason: the routine copies one arena into another
  between two contexts, inside the host's atomic upgrade callback or before a pid exists. A Vortex
  routine would need a context that holds the old graph, and the old graph is read-only data during
  the swap. The bounded loops are plain C++ over the existing arena API. Nothing that decides
  anything about a source (health, scoring, calibration) is in it; those stay Vortex.
- **Price.** Section 4.5. No actor cell in steady state; a transient peak of 2 x 26,112 cells and
  1.2 MiB of map. The budget-parity test (plan section 2) re-measures
  `AFullEpistemicBudgetStillCheckpoints` and records the work figure and the double planning time.
- **Gate.** ARCH, DIST, CODE, EPI, SEC. The plan lists these five for T1.

### 8.5 T2: typed migrations and the claim

T2 keeps the plan's scope and builds on the types T1 fixed.

- **Content.** `Migratable` gets content: a model, feature-schema or incarnation change opens a new
  epoch with declared priors, expires old observations, and gives pending labels and completions
  `ExpiredLabel` or `ExpiredVerificationTarget`, while retained ids, outcomes and floors still
  reject replay (spec 2.2). The policy-migration path of D3 (revoke bindings, keep profiles). The
  full-bound lifecycle test of 8.2-R2. The parity property over every structure the registry then
  holds. The coverage script run over all of P19.
- **Files.** `epistemic_translate.cpp`, `epistemic_translate_<area>.cpp` per area,
  `upgrade_schema.*`, `agent_executor.cpp` (revoke at commit),
  `tests/xuzz/vagent_epistemic_translate_test.cpp`.
- **C++ and Vortex.** As T1c. A migration's declared priors are data a program writes in the policy;
  applying them to a fresh epoch is Vortex in I4-1b, which owns the calibration records.
- **Gate.** ARCH, DIST, EPI, ML, CODE.

## 9. Tests

Names are indicative. "Unchanged" means the context pointer, the grant, the latch, both program
bases (a digest of every cell's text and value bits), the arena's cell count and the
retained-program registry are as before.

| Slice | Test                                                           | Asserts                                                                                                                                                                                                                               |
| ----- | -------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| T1a   | `RefusalVocabularyIsStable`                                    | every declared code has a `toString`, an `actor` and the fields of the table in 2.2; a text over 128 bytes is cut; `make` is the only constructor                                                                                     |
| T1a   | `ResumeOfAChangedPolicyUnderTheSameReferenceRefusesBeforeAPid` | two programs with one explicit reference (`fromUnit`), different `decay`: `code == PolicyChanged`, subject, both hashes; no pid; red at HEAD (X-9)                                                                                    |
| T1a   | `AResumeRefusalLeavesNoTrace`                                  | for each T1a code: `actorCount`, the issuer's issued count, trusted nodes, the program registry and `needsFirstTurn_` as before                                                                                                       |
| T1a   | `AResumeWithAMissingProgramIsRuntimeAndDecidedFirst`           | a checkpoint whose program is not retained, together with a changed policy: `Runtime{NotAuthorized}`, not `PolicyChanged`                                                                                                             |
| T1a   | `AResumeRefusedByTheRuntimeAfterThePreflightHasAKnownResidue`  | a forced lower-layer refusal: no pid, no actor, trust unchanged for a known node, issued count plus one                                                                                                                               |
| T1a   | `ResumeBudgetRefusalsComeBeforeAPid`                           | a grant probe one instruction below the plan: `WorkBudget{needed, available}`; one cell below: `PeakCopyBudget`; no pid; the same two at a lower `settings_.epistemic.cells`; with no probe installed the settings half still refuses |
| T1a   | `PreflightResumeAndPreflightUpgradeAreDryRuns`                 | same answer as the real call, no change                                                                                                                                                                                               |
| T1a   | `EveryUpgradeRefusalIsTyped`                                   | schema road: each of `NotQuiescent{reason, count}`, `ActiveIntention`, `PolicyChanged`, `PredicateIncompatible`, `PredicateRemoved`, `PeakCopyBudget`, `WorkBudget`; each unchanged                                                   |
| T1a   | `TheReconcilerRoadComparesPolicy`                              | a bound arbitrating agent with no profile and a changed policy: `PolicyChanged` (red at HEAD, which accepts it)                                                                                                                       |
| T1a   | `ARemovedBoundOrArbitratedPredicateIsRefusedOnBothRoads`       | a bound, empty agent (no profile) upgraded to a program that drops the predicate: `PredicateRemoved` on the schema and the reconciler road, binding intact; an unbound plain predicate can still be dropped; red at HEAD              |
| T1a   | `ThePublicEntryPointsAgree`                                    | `LocalHost::upgrade` by value and by pointer give the same code for the same P19 state for every cause but the road; populated state: by value `RouteCarriesNoEpistemicState` naming the other overload                               |
| T1a   | `ANonEpistemicAgentWithAsksStillReconcilesByValue`             | an agent with no arbitrated predicate and an ask in flight upgrades by value and the ask is carried (no regression outside P19)                                                                                                       |
| T1a   | `ASameReferenceDifferentProgramIsRefusedByName`                | `fromUnit` under one reference, changed policy: by value gives `ProgramReferenceReused`; an equal schema returns the held program; a pre-flight refusal leaves `programs_` and the executor registry unchanged                        |
| T1a   | `TheTuiPrintsTheTypedReason`                                   | the action log holds `toString`, not an integer                                                                                                                                                                                       |
| T1b1  | `TheHeaderIsMintedOnlyForAnArbitratingProgram`                 | present for one, absent for the other; value 1                                                                                                                                                                                        |
| T1b1  | `ANonArbitratingAgentUpgradesUnchangedAfterT1`                 | the agent with no header upgrades; fields, timers and `on Upgrade` as before                                                                                                                                                          |
| T1b1  | `AnArbitratingAgentWithNoProfileUpgradesAndResumes`            | the header does not make the agent populated                                                                                                                                                                                          |
| T1b1  | `TheGuardIgnoresTheHeaderAndReadsTheRest`                      | table 3.2, every row; `UnknownShapeVersion` carries the numbers; absent header with profiles                                                                                                                                          |
| T1b1  | `TheSavedHeaderIsComparedAndNotCopied`                         | after resume and after upgrade the header cell is the program's own, by ref                                                                                                                                                           |
| T1b1  | `ResumeOfAChangedPolicyTagNamesTheTag`                         | a master whose `encoding` differs: `PolicyEncodingChanged{saved, current}`                                                                                                                                                            |
| T1b1  | `AnActiveIntentionRefusesOnEveryRoadForAnArbitratingAgent`     | resume, schema road, reconciler road: `ActiveIntention{savedCount}`; a non-arbitrating agent still resumes and reconciles with an intention                                                                                           |
| T1b2  | `APostBeforeTheGateIsCountedAndAPostAfterItIsRefused`          | a queued reading: `NotQuiescent{posts, 1}`; the swap run by the observer at `BetweenObserveSections`: `observe` gives `AdmissionClosed`, uncharged, unlogged, no note; `bindSource`, `retireSource` the same                          |
| T1b2  | `AHealthActionRacingTheGateLeaksNothing`                       | the observer sets the gate at `AfterGrantMatch`: `AdmissionClosed`; `pendingHealth_` size, `resetMarks.requested`, the back-pressure counts and the charge map unchanged; same for the batch call                                     |
| T1b2  | `AQueuedHealthTicketRefusesTheUpgrade`                         | a ticket in `pendingHealth_`: `NotQuiescent{pendingHealth, 1}`; after the pump it upgrades                                                                                                                                            |
| T1b2  | `InFlightIsEmptyAtQuiescence`                                  | after a flooded drain `Ingress::inFlight` and `queued` are empty for the pid                                                                                                                                                          |
| T1b2  | `OpenChargesAndResetMarksSurviveAndCloseUnderTheNewProgram`    | a charge open at the swap closes into a note after it; `resetMarks` and `healthOutcomeRing_` answer after it; `healthActionSeq_` continues                                                                                            |
| T1b2  | `TheTailRunsUnderTheGate`                                      | an observer at `DuringDrain` sees `Swapping`; a post there is refused; the latch cache was cleared before it                                                                                                                          |
| T1b2  | `AdmissionReopensOnEveryExit`                                  | after a refusal, after a commit, after an exception in the callback, after an exception in the tail (success, gate released)                                                                                                          |
| T1b2  | `TheMailboxRefusesAnArbitratingUpgrade`                        | `runtime().receive` of a message: `NotQuiescent{mailbox, 1}`; after `pump` it upgrades; a non-arbitrating agent with the same mailbox upgrades as before                                                                              |
| T1b2  | `ThePauseExpiresAndIsRenewedForTheDrain`                       | a fake steady clock past the ttl reopens admission without a call; `upgradeQuiescing` over the same span commits; `requirePaused` after a lapse refuses                                                                               |
| T1b2  | `UpgradeQuiescingDrainsAndUpgrades`                            | a flooded source, then a committed upgrade and a resumed admission                                                                                                                                                                    |
| T1c   | `PredicateKeysDecodeAndRoundTrip`                              | `decode(mint(p)) == p`, every optional present and absent; the stored hash is verified                                                                                                                                                |
| T1c   | `AChangedSignatureIsNotCompatible`                             | arity, key position, parameter type and frame each give `PredicateIncompatible` or `PolicyChanged`; unchanged state; old actor still ingests                                                                                          |
| T1c   | `AForcedFullKeyHashCollisionDoesNotMatch`                      | the matcher with a constant `KeyDigest`: two predicates that differ in one parameter type are not matched; matches equal the default's                                                                                                |
| T1c   | `PopulatedStateSurvivesResumeAndUpgrade` (spec 8.2 gate 5)     | for every `allOwnedStructures()` entry via `populate`: digests equal across source, resume and upgrade `ModuloClock`; clone occurrences, shared-clause topology and retired chain intact; program bases unchanged                     |
| T1c   | `ResumeAndUpgradeAgreeOverRandomIngestion`                     | 24 fixed seeds, up to 40 events (readings, refusals, advance, retire, rebind, isolate, clear, reset): the same digest on both roads                                                                                                   |
| T1c   | `AFailingOnUpgradeRollsBackTheTranslation`                     | the hook fails after the routine ran: unchanged, no new cells visible                                                                                                                                                                 |
| T1c   | `AFailingTranslatorRollsBack`                                  | an injected translator that returns a refusal: unchanged                                                                                                                                                                              |
| T1c   | `AnApplyTimeResumeFailureEndsTheActorWithAStoredRefusal`       | an injected failing translator on resume, and a host with no grant probe and a plan above the grant: `fatal(init)`, one WARN naming the code, and `lastRefusal(pid)` holds the refusal                                                |
| T1c   | `AFullBudgetResumesAndUpgradesWithinThePeak`                   | `AFullEpistemicBudgetStillCheckpoints` at its bound, then both roads; work, peak and the doubled planning time recorded; `PeakCopyBudget{needed, limit}` on a host with a lower ceiling                                               |
| T1c   | `ALatchAndReasonsSurviveBothRoads`                             | `faulted`, `fault_seq`, a reason and its provenance, byte-equal; the latch is never cleared                                                                                                                                           |
| T1c   | `LivenessStampsFollowTheDisposition`                           | resume clears `last_receive` and `last_contact`, upgrade keeps them                                                                                                                                                                   |
| T1c   | `DoublesCrossByBitPattern`                                     | a `-0.0` health weight stays `-0.0` on both roads                                                                                                                                                                                     |
| T1c   | `ARecordWithoutAProfileIsInconsistent`                         | `StateInconsistent`                                                                                                                                                                                                                   |
| T1c   | `AMissingStreamsListIsReplayStateUnavailable`                  | `ReplayStateUnavailable{principal, "streams"}`                                                                                                                                                                                        |
| T1c   | `AnUnregisteredDimensionRefuses`                               | a profile with an extra owned dimension: `UnregisteredOwnedStructure{dimension}`                                                                                                                                                      |
| T1c   | `BindingsSurviveAnUnchangedPolicyAndAChangedOneIsRefused` (D3) | an unchanged upgrade accepts a reading without a rebind; a changed policy is refused and the binding still accepts                                                                                                                    |
| T1c   | `RegistryIsConsistent`                                         | unique ids; every `dimension` registered; every `Dropped` has a reason; every entry has a `populate`                                                                                                                                  |
| T1c   | `TheCoverageScriptFailsOnAnUncoveredFieldOrDimension`          | a fixture header with one extra field constant and a fixture source with one extra `ensureDimension`: the script exits non-zero on each                                                                                               |

Revision 1's test `ASecondPlanFailingAfterAGoodPreflightEndsTheActorWithAWarning` is removed: it
needed a source that serves a second carrier, which tests the double and not the code, and the case
it described cannot happen (section 2.3).

Spec 8.2 coverage: the first regression bullet of section 8.2 ("Resume and composed upgrade
preserve...") is `PopulatedStateSurvivesResumeAndUpgrade` for the structures that exist, and each
later slice that adds a structure extends it through its own registry entry. "A same-named predicate
with changed arity/key/types/frame/schema is rejected" and "forced full-key hash collisions do not
match" are `AChangedSignatureIsNotCompatible` and the collision test. "Program bases are unchanged
on success or refusal" is the base digest in every row. "A prior fault latch and unrelated warning
survive upgrades" is `ALatchAndReasonsSurviveBothRoads`. The clock-mapping, label-expiry and
model-reset bullets belong to I3-3, I4 and I5 and are T2's final run; the reconciliation bullet
"full-key hash collisions, profile-first migration, schema changes, checkpoint clock rebasing" is
split between this table (collisions, profile-first order, schema) and I3-3c (clock rebasing).

Rows this design settles, by the plan's list for T1: 1-12, U-1 (4), U-3 (4.2), U-12 (2, 4.5), U-13
(T1c rollback tests), U-14 (2.3), U-15 and U-16 (`ALatchAndReasonsSurviveBothRoads`), U-23 (4.5);
2.1-33 and 8.2-R2f through the tests above. Evidence is the slices', not this document's.

## 10. Proposed amendment and questions

**A12** (proposed, spec 8.2 gate 0, extends A5). P19 upgrade and resume support is claimed for
agents without an active intention. A program with an arbitrated predicate refuses resume, schema
upgrade and reconciler upgrade while an intention is active, with a typed reason. An upgrade of such
an agent also requires an empty mailbox and a quiet admission.

Questions for the lead, repeated in the handoff:

1. **Size.** T1 is 3,400 lines in this design against 1,500 in the plan. T1a, T1b1, T1b2 and T1c are
   the split; the plan's totals move by about 1,900. The growth since revision 1 (2,900) is the
   health commit scope and the host-state report (T1b2), the grant probe and the shared pre-flight
   (T1a).
1. **Name.** `TransferRefusal`, not `EpistemicRefusal` (taken, `epistemic_ingress.hpp:71`).
   `cluster_node.cpp` needs no edit.
1. **Resume and intentions.** A12 refuses resume for an arbitrating agent with an intention. Today
   it works. The alternative is to refuse only when profiles exist.
1. **Reconciler road.** Settled in revision 2: it stays the road of
   `LocalHost::upgrade(pid, CompiledProgram)` and of non-epistemic agents; it now compares policy,
   signature and removal through the shared pre-flight and refuses populated state with a typed
   code. Only the question of whether populated state should instead be routed to the schema road
   automatically remains, and it is declined here: it would hide which road ran.
1. **Master fields.** `encoding` and `signature` join the policy master as part of shape 1 (T1b1).
   Without them neither the tag nor the full key is readable from a checkpoint.
1. **Setting.** `vagent.admission_pause_ms` is new.
1. **TUI grant.** Its 1,000,000-instruction grant leaves 36% of the work for a full graph; a larger
   grant is a one-line change if the T1c measurement disagrees.
1. **Grant probe.** `setResumeGrantProbe` is a new host obligation; an out-of-tree host that does
   not install one gets the apply-time backstop and a stored refusal. The alternative is to refuse a
   resume for a host with no probe.
1. **Mesh mailbox.** T1 refuses on the count and states the serialisation obligation; whether
   `ClusterNode::receive` shares the pump's thread is left to the I3-4 review, and a `holdMailbox`
   effect is a mesh-runtime change that needs its own approval.

## Review dispositions (round 1)

Reports: `t1-review/review-architect.md` (ARCH, FAIL, 3 MAJOR, 5 MINOR) and
`t1-review/review-distributed.md` (DIST, FAIL, 4 MAJOR, 7 MINOR). Each finding was checked against
the code at HEAD `49149e4b` before it was acted on.

| ID      | Persona | Severity | Disposition                                                                                                                                                                                                                                                                                                                                                                                                        |
| ------- | ------- | -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| ARCH M1 | ARCH    | MAJOR    | Fixed in the header and every section: all citations re-pinned, the "I3-2 is not implemented" premise removed, shipped names from `epistemic_abi_health.hpp`, `agent_executor_health.hpp`, `agent_executor_internal.hpp` used (5.4, 7).                                                                                                                                                                            |
| ARCH M2 | ARCH    | MAJOR    | Fixed in 5.1, 5.2, 5.4: `pendingHealth`, `inFlight`/`queued` in the report; lock order ingress, health, post box; the gate set under both mutexes; `healthAction` re-checks in its commit scope; table completed; tests added (9, T1b2).                                                                                                                                                                           |
| ARCH M3 | ARCH    | MAJOR    | Fixed in 4.6 and decision 7: by-value overload stays on the reconciler road; one `preflightUpgrade` for both; "second copy" argument deleted; tests for agreement and for a non-epistemic agent with asks.                                                                                                                                                                                                         |
| ARCH m1 | ARCH    | MINOR    | Fixed in 7: parity test iterates the registry (`populate`), no `coversStructure`/`Environment`, script checks only dimensions and field constants and is not tied to the gtest filter; the id list is authored in the registry.                                                                                                                                                                                    |
| ARCH m2 | ARCH    | MINOR    | Fixed in 8 and 2.2: T1b split into T1b1 (shape, guard) and T1b2 (admission), independent; each slice declares only the codes it produces.                                                                                                                                                                                                                                                                          |
| ARCH m3 | ARCH    | MINOR    | Fixed in 2.2, 4.2, 5.3: `PlanRequest`/`ApplyRequest` replace the optional reference; `TransferRefusal::make` is the only constructor so `actor` cannot value-initialise; `limit` left empty for `ActiveIntention`; setters chain, `pauseAdmission` returns `std::expected<AgentExecutor *, TransferRefusal>`; the asymmetric `operator==` is dropped for a one-time edit of the T0 assertions.                     |
| ARCH m4 | ARCH    | MINOR    | Fixed in 3.1 and 8.2: shape 1 is defined by T1b1 together with the master fields; T1a changes no persisted layout; nothing persisted earlier is supported.                                                                                                                                                                                                                                                         |
| ARCH m5 | ARCH    | MINOR    | Fixed in 2.3 and 9: the TOCTOU case and test are removed; the resume pre-flight is a dry run; `WorkBudget` is decided there from the grant probe (see DIST M3), and the apply-time failures are tested with an injected translator.                                                                                                                                                                                |
| DIST M1 | DIST    | MAJOR    | Fixed in 5.2: the admission test sits in `observe`'s second section in the scope of `posts_->push`; the first-section check is only an early-out; the invariant is stated as one critical section; the race test runs the swap between the sections.                                                                                                                                                               |
| DIST M2 | DIST    | MAJOR    | Fixed in 5.2: one health commit scope (admission, back-pressure, receipt, insert, reset generation, push) so a refusal precedes every state change and leaks no ticket, generation or charge; `bindSource`/`retireSource` already push under the lock and gain the test there.                                                                                                                                     |
| DIST M3 | DIST    | MAJOR    | Fixed in 2.3 and 4.5: confirmed against `program.cpp:321-345` and `mesh_actor.cpp:228-262`; option (a) taken, a `ResumeGrantProbe` the host installs from the same function as its authorizer; test at a grant one instruction below the plan; apply-time backstop and `lastRefusal(pid)` for a host with no probe.                                                                                                |
| DIST M4 | DIST    | MAJOR    | Fixed in 4.3, 6 and 9: confirmed (`RemovedPredicate` is not in `upgradeChangeRequiresRefusal`, a binding needs no profile); compatibility is decided from both schemas and `Ingress::bindings`, inside `acquireSwap`, on both roads; `PredicateRemoved` for an arbitrated or bound predicate; test with a bound, empty agent.                                                                                      |
| DIST m1 | DIST    | MINOR    | Fixed in 2.3 and 9: TOCTOU wording and test dropped; apply-time failures are observable through `lastRefusal(pid)`.                                                                                                                                                                                                                                                                                                |
| DIST m2 | DIST    | MINOR    | Fixed in 5.2: the `SwapGate` is destroyed after the tail (`clearLatchCache`, drain, `flushStats`, `deriveSchedule`); the tail is caught and logged because it cannot be made transactional, the commit stands.                                                                                                                                                                                                     |
| DIST m3 | DIST    | MINOR    | Fixed in 5.3: `upgradeQuiescing` extends the pause to its whole timeout and passes `requirePaused`; fake-clock tests.                                                                                                                                                                                                                                                                                              |
| DIST m4 | DIST    | MINOR    | Partly fixed in 5.1: the refusal carries the mailbox count; the "same thread" claim is replaced by a stated host obligation and the `cluster_node.cpp:544` receive path is left to the I3-4 review. Declined: a `holdMailbox` pause effect, because it changes the mesh runtime and needs its own approval.                                                                                                        |
| DIST m5 | DIST    | MINOR    | Fixed in 5.4: `charges`, `uncounted`, `refusedWhileFaulted`, `marks` are listed as kept with the reason (none reads program state) and a test; `queued` and `inFlight` are asserted empty. They are not added as refusal fields because an open charge can linger on an idle source and would block upgrades indefinitely.                                                                                         |
| DIST m6 | DIST    | MINOR    | Fixed in 4.6: pre-flight before retaining; a same-reference program with a different schema is `ProgramReferenceReused`; the program registry joins the "unchanged" list.                                                                                                                                                                                                                                          |
| DIST m7 | DIST    | MINOR    | (a) fixed in 2.2 and 2.3: a missing program is `Runtime`, decided before any compare, with the order of checks written down. (b) Partly declined in 2.3: `trustNode` cannot move after `runtime_.resume` (the runtime reads the trust set) and the runtime has no untrust (`mesh_actor.hpp:162`); the trust is idempotent and the issued capability unused, so the residue is stated and pinned by a test instead. |
