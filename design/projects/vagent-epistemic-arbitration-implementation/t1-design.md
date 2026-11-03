# T1 design: the translator spine and typed refusals

Status: revision 1, for review. Authority: approved P19 v1.5 section 2.2, "Upgrade and checkpoint
translation" (lines 192-252), and section 8.2 gate 5 with its regression bullets (lines 963-1010);
the plan's T1-D row and slice T1 ([remaining-plan.md](remaining-plan.md) sections 4.4 and 4.5); the
lead's rulings in [decisions.md](decisions.md): P2-1 (one typed refusal channel), R3-11 (shape
version and one guard), D3 (policy change against bindings), O1 with A5 (supported-agent upgrade
only) and section 0 (review cadence). Code citations are `path:line` at HEAD `47d00222`, relative to
`apps/common/xanadu/` unless they start with `tests/`. I3-2 is not implemented at HEAD, so every
health structure below is taken from the accepted I3-2 design, revision 4
([i3-2-design.md](i3-2-design.md) sections 6.1 and 13) and is marked as such.

## 1. Decisions

| #   | Decision                                                                                                                                                                                                                             | Section |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------- |
| 1   | One vocabulary, `TransferRefusal`, with a code, the route, the subject and the offending texts and numbers. `resume` and both `upgrade` overloads return it. A pre-flight produces it before any state changes.                      | 2       |
| 2   | Resume's pre-flight runs inside `AgentExecutor::resume`, after `fetchValue` and before `trustNode`, `issuer_.issue` and `runtime_.resume`, on the received base. No pid exists when it refuses.                                      | 2.3     |
| 3   | The shape version is a three-cell record at the head of the agent's `d.epistemic` rank, minted at compile time only for a program with an arbitrated predicate. It is compared by number and never copied.                           | 3       |
| 4   | `hasTranslatableEpistemicState` is the only place that decides whether an agent holds state the executor may carry. It ignores the header.                                                                                           | 3.2     |
| 5   | The three copiers stay three. They share the P19 half: one routine, `translateEpistemicState`, in four read-only-then-write passes over one old-to-new map. The generic halves stop following `d.trust`, `d.epistemic` and clones.   | 4       |
| 6   | Predicate compatibility is a full key decoded from the graph on both roads. A digest is only an index into buckets that are searched by full-key equality.                                                                           | 4.3     |
| 7   | The reconciler road refuses populated P19 state with a typed code and also compares policy, which it does not today. `LocalHost::upgrade(pid, CompiledProgram)` forwards to the schema road, so both public entry points agree.      | 4.6     |
| 8   | Quiescence is one report. Admission is closed by an operator pause (`pauseAdmission`) and, for the length of a swap, by a gate set in the same critical section as the emptiness check. A non-empty mesh mailbox refuses an upgrade. | 5       |
| 9   | D3: a policy change is refused on every road in T1, so bindings cannot go stale. T2's typed migration revokes the bindings of the migrated predicate and the operator rebinds. Nothing rebinds silently.                             | 6       |
| 10  | A registry of owned structures is declared once per area. Each translator test registers what it covers. A script and a runtime check fail on a structure or dimension that has no entry.                                            | 7       |
| 11  | T1 splits into T1a (channel), T1b (shape, guard, admission) and T1c (routine, digest, oracle). Together about 2,900 lines, against the plan's 1,500.                                                                                 | 8       |

## 2. The refusal channel

### 2.1 What exists at HEAD

- `resume` returns `std::expected<ActorPid, ActorError>` (`vagent/runtime/agent_executor.hpp:450`).
  `ActorError` is a closed enum without a payload (`vortex/mesh_actor.hpp:30`).
- Resume of a changed policy looks like a success. `resume` returns a pid at
  `vagent/runtime/agent_executor.cpp:5060`. The first `execute` calls `prepare` (`:1469`), which
  calls `restoreState` (`:1304`). `restoreState` throws `std::runtime_error` at `:4844`. `prepare`
  catches it and logs at DEBUG (`:1409`), and `execute` ends in `fatal(state, kCauseInit)`
  (`:1470`). The caller holds a pid for an actor that dies with cause `init`.
- The existing test `AChangedPolicyRefusesTheResume`
  (`tests/xuzz/vagent_epistemic_resume_test.cpp:326`) does not reach that path. A program built by
  `CompiledProgram::fromSource` has a content-derived reference
  (`vagent/runtime/program.cpp:23,39`), so a changed policy has a different reference and the
  authorizer refuses it (`vagent/runtime/program.cpp:318`) with a bare `NotAuthorized`. `vagent run`
  builds programs with `fromUnit` and an explicit reference (`apps/vagent/main.cpp:1050,1087,1220`),
  so there the mismatch reaches `restoreState` and the actor dies. The comment in the test
  (`"restoreState compares the hashes again regardless"`) names a check no test exercises.
- Both upgrade overloads return `ExecutionFailed` or `NotQuiescent` for every cause
  (`vagent/runtime/agent_executor.cpp:5069-5100,5123,5154-5160`). A caller cannot tell a changed
  policy from a populated profile from an active intention.
- The `EpistemicRefusal` name is taken. It is the ingress enum
  (`vagent/runtime/epistemic_ingress.hpp:68`). The plan's "executor-level `EpistemicRefusal`" would
  collide with it, so this design names the new type `TransferRefusal`.
- Callers of `resume` and `upgrade` outside tests: `LocalHost::upgrade` twice
  (`vagent/runtime/program.cpp:395,402`) and `AgentTui::upgrade` (`vagent/tui/agent_tui.cpp:649`).
  `cluster_node.cpp` calls neither: its `ActorError` mentions are `launch` and `send`
  (`vagent/cluster/cluster_node.cpp:646,713,719`). The plan lists it under T1; no edit is needed
  there. `apps/vagent/main.cpp` calls neither.

### 2.2 The vocabulary

New files `vagent/runtime/epistemic_refusal.{hpp,cpp}`, namespace `xanadu::vagent::runtime`.

```cpp
enum class TransferRoute : std::uint8_t { Resume, UpgradeSchema, UpgradeReconciler };

enum class TransferCode : std::uint8_t {
  Runtime, NotQuiescent, ActiveIntention, UnknownShapeVersion, NoTranslator,
  PolicyChanged, PolicyEncodingChanged, PredicateIncompatible, PredicateRemoved,
  RouteCarriesNoEpistemicState, UnregisteredOwnedStructure, ReplayStateUnavailable,
  StateInconsistent, PeakCopyBudget, WorkBudget,
};

struct TransferRefusal {
  TransferCode code{TransferCode::Runtime};
  TransferRoute route{TransferRoute::Resume};
  zigzag::vortex::mesh::ActorError actor{};    // what callers compared before this type existed
  std::optional<std::string> subject;           // predicate, principal token, reason or dimension
  std::optional<std::string> saved, current;    // texts found in the old state and in the new program
  std::optional<std::uint64_t> savedCount, currentCount, limit;
  friend bool operator==(const TransferRefusal &, zigzag::vortex::mesh::ActorError) noexcept;
};
[[nodiscard]] std::string_view toString(TransferCode) noexcept;
[[nodiscard]] std::string toString(const TransferRefusal &);
```

No field is a sentinel: an absent value is an empty `std::optional`. Every text is cut at 128 bytes.
No document text, key or secret is ever put in one: the subjects are predicate names, principal
tokens (public-key digests, which `isPrincipalToken` checks at `agent_executor.cpp:3425`), field
names and reason names. `toString` prints
`PolicyChanged(route=Resume, subject=reading, saved=ab12..., current=cd34...)`. The `operator==`
against `ActorError` compares `actor`, so the existing assertions such as
`EXPECT_EQ(refusal.error(), ActorError::NotQuiescent)` keep compiling and holding.

| Code                           | Decided by (first check that fails wins, in this order)                                      | `subject`                  | `saved` / `current`                        | Numbers                                       | `actor`           |
| ------------------------------ | -------------------------------------------------------------------------------------------- | -------------------------- | ------------------------------------------ | --------------------------------------------- | ----------------- |
| `Runtime`                      | any lower layer: unknown pid, authorizer, fetch, grant                                       | none                       | none                                       | none                                          | the lower code    |
| `NotQuiescent`                 | the quiescence report (section 5.1)                                                          | reason name                | none                                       | `savedCount` is the count                     | `NotQuiescent`    |
| `ActiveIntention`              | an arbitrating agent, or the schema road of any agent, has intentions                        | none                       | none                                       | `savedCount` intentions, `limit` 0            | `ExecutionFailed` |
| `UnknownShapeVersion`          | saved header differs from the new program's header, or is absent while state is populated    | `d.epistemic`              | none                                       | `savedCount` (absent if none), `currentCount` | `ExecutionFailed` |
| `NoTranslator`                 | populated state, the header matches, no translator is registered for it                      | `d.trust`                  | none                                       | `savedCount` is the shape                     | `ExecutionFailed` |
| `PolicyEncodingChanged`        | a saved master's `encoding` text differs from the new program's                              | predicate                  | the two tags                               | none                                          | `ExecutionFailed` |
| `PolicyChanged`                | typed policies differ; either side absent means a gain or a loss                             | predicate                  | the two policy hashes (absent = no policy) | none                                          | `ExecutionFailed` |
| `PredicateIncompatible`        | name, arity, ordered types or key positions differ                                           | predicate                  | the two signatures                         | none                                          | `ExecutionFailed` |
| `PredicateRemoved`             | a saved predicate with a policy is absent from the new program                               | predicate                  | the saved signature                        | none                                          | `ExecutionFailed` |
| `RouteCarriesNoEpistemicState` | reconciler road with populated state                                                         | `d.trust` or `d.epistemic` | none                                       | none                                          | `ExecutionFailed` |
| `UnregisteredOwnedStructure`   | the old graph links a dimension that no registry entry owns (section 7)                      | dimension name             | none                                       | none                                          | `ExecutionFailed` |
| `ReplayStateUnavailable`       | a profile lacks `incarnation`, `streams` or `dedup`, or a dedup entry is malformed           | principal token            | the missing field                          | none                                          | `ExecutionFailed` |
| `StateInconsistent`            | a record has no profile, an edge's far end is unmapped, or a pass copied other than its plan | structure id               | none                                       | `savedCount` copied, `currentCount` planned   | `ExecutionFailed` |
| `PeakCopyBudget`               | planned cells exceed the allowance (section 4.5)                                             | none                       | none                                       | `savedCount` needed, `limit` allowed          | `ResourceLimit`   |
| `WorkBudget`                   | planned work exceeds the remaining instruction grant                                         | none                       | none                                       | `savedCount` needed, `limit` available        | `ResourceLimit`   |

The `actor` column reproduces what `upgrade` returns today, so a caller that only knew `ActorError`
sees no change.

### 2.3 Where it is returned

| Entry point                                                                 | Return type                                  | Pre-flight                                                                                                                                         |
| --------------------------------------------------------------------------- | -------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| `AgentExecutor::resume(const AgentCheckpoint&)`                             | `std::expected<ActorPid, TransferRefusal>`   | `preflightResume`, after `fetchValue` (`agent_executor.cpp:5016`) and before `trustNode` (`:5029`), `issue` (`:5048`), `runtime_.resume` (`:5060`) |
| `AgentExecutor::upgrade(pid, shared_ptr<CompiledProgram>, grant)`           | `std::expected<void, TransferRefusal>`       | `preflightUpgrade(pid, program, Schema)`, in place of `upgradeRefusal` at `:5118` and the intention test at `:5123`                                |
| `AgentExecutor::upgrade(pid, CompiledProgram&)`                             | `std::expected<void, TransferRefusal>`       | `preflightUpgrade(pid, program, Reconciler)`, in place of `upgradeRefusal` at `:5462`                                                              |
| `LocalHost::upgrade` (both overloads, `vagent/runtime/program.hpp:287,291`) | `std::expected<void, TransferRefusal>`       | forwards                                                                                                                                           |
| `AgentTui::upgrade` (`vagent/tui/agent_tui.cpp:649`)                        | unchanged `std::expected<void, std::string>` | prints `toString(refusal)` instead of `static_cast<int>(res.error())`                                                                              |

`preflightResume(const AgentCheckpoint&) const` and
`preflightUpgrade(pid, const CompiledProgram&) const` are also public, return
`std::optional<TransferRefusal>`, change nothing, and give an operator a dry run. `resume` and
`upgrade` call them; there is one implementation of each check.

The resume pre-flight reads the received base through a bare
`zigzag::ArenaManifold view( received->manifold.get())` (`vortex/mesh_value.hpp:98-105`; the
constructor is `zigzag/arena_manifold.hpp:255`). It needs no `WorkerContext`, so it pays no
standard-library bootstrap. The old dimensions are found by name from `received->root`, with the
walk now in `restoreState` (`agent_executor.cpp:4686-4703`) moved to `discoverDimensions` in
`epistemic_translate.cpp` and called from both. The new program's keys come from the program's own
`UpgradeSchema` (`program.hpp:171`), which carries typed parameters and the policy
(`upgrade_schema.hpp:36-45`).

A refusal leaves nothing behind. The test reads `runtime().actorCount()`, the issuer's issued count
(through a counting `CapabilityIssuer` double), `needsFirstTurn_` (through `pump` doing nothing) and
the trusted-node set. `resume` runs the same plan again inside `restoreState`, because that is the
copier. A plan that succeeded in the pre-flight and fails there means the checkpoint changed between
two reads of the same source. That case ends as today in `fatal(kCauseInit)`, with a WARN that
prints `toString` of the refusal. A test makes it happen with a source that serves a second,
different carrier.

### 2.4 Tests

Each asserts `refusal.error().code`, `.subject`, `.saved`, `.current` and the numbers, never a log
line (section 9 lists them). Every one also checks that nothing changed: no pid for `resume`; for
`upgrade` the context pointer, the grant, the latch, both program bases and the arena's cell count.

## 3. The shape version and its guard

### 3.1 The header

`kEpistemicShapeVersion = 1`, in a new `vagent/runtime/epistemic_shape.hpp` together with the label
`kShapeHeaderName = "EpistemicShape"` and the field name `version`. The header is a record of three
cells: the head cell (content `EpistemicShape`), a `version` label on `d.vars` and an Int64 scalar
on `d.values`, the same shape as the policy master's fields (`compiler.cpp:3183-3230`). It is the
first cell of the agent cell's `d.epistemic` rank.

- **Minted by `Gen`** when `anyArbitrated`, a flag set while the predicate loop runs
  (`compiler.cpp:3150-3173`). The cells live in the program base, like the policy masters, so they
  are not in any actor's epistemic cell count and not in the ceiling. `compiler.cpp` gains at most
  100 lines, in a new out-of-class member in `epistemic_emit_shape.cpp` plus one call.
- **An agent with no arbitrated predicate has no header.** Its `d.epistemic` rank stays empty, and
  nothing changes for it.
- **The number changes only when the shape does.** Any slice that renames a field, changes a field's
  kind, moves a rank or changes a meaning bumps it, as the plan says. An additive field whose
  absence reads as a documented initial value does not. When it is bumped the old reader is deleted
  (AGENTS.md, "bump the version, don't carry a shim"), so a state saved under the old number refuses
  by number.
- **Compared, never copied.** The new program's `instantiate` already holds its own header. Neither
  copier follows the saved one. `translateEpistemicState` reads the saved number and compares it
  with the new agent's by value. A test asserts that after a resume and after an upgrade the header
  cell of the new agent is the program's own cell, by ref.
- **The master gains two fields.** `mintPolicyMaster` adds `encoding` (the text of the tag that
  `canonicalPolicy` starts with, `epistemic_policy.cpp:343`, today `vagent-policy-2`) and
  `signature` (section 4.3). The checkpoint stores only the hash today (`compiler.cpp:3228`), so a
  tag change is indistinguishable from a field change and the plan's "tag readable from the
  checkpoint" is not true. The two fields are part of shape 1. They add four cells a predicate to
  the base.

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
holds its profile's clone occurrence, `kObsProfile`), so support histories need no walk of their
own; a test builds a record without a profile and expects `StateInconsistent`.
`hasTranslatableEpistemicState` is true when `!populated`, or when `shape` equals a number the
registry has a translator for. It never reads the header's cells as state.

`upgradeRefusal`'s root test (`agent_executor.cpp:5088-5100`, `linked(trust) || linked(epistemic)`)
is replaced by this function. Later slices change the registry and the probe, not each call site.
The `bool translates` computation there goes with it.

| Agent                                                   | `shape` | `populated` | Result                                                   |
| ------------------------------------------------------- | ------- | ----------- | -------------------------------------------------------- |
| no arbitrated predicate                                 | absent  | false       | translatable; upgrades as before                         |
| arbitrating, no source yet                              | 1       | false       | translatable; the header does not make it populated      |
| arbitrating, profiles                                   | 1       | true        | translatable if shape 1 has a translator (T1c)           |
| arbitrating, profiles, saved shape differs from program | 2       | true        | `UnknownShapeVersion{savedCount=2, currentCount=1}`      |
| profiles, no header (written before T1)                 | absent  | true        | `UnknownShapeVersion{savedCount absent, currentCount=1}` |

### 3.3 Intentions

A supported agent is one with no active intention (O1(b), A5). Today only the schema road refuses
one (`agent_executor.cpp:5123`); the reconciler road copies intentions (`reconciler.cpp:839-936`)
and `restoreState` copies them (`agent_executor.cpp:4880-4887`). T1 types the schema road's refusal
and extends it, for a program whose shape header exists, to all three roads. Non-arbitrating agents
keep resuming and reconciling with intentions, so nothing outside P19 regresses. Intentions may
later head query tokens (plan O3), and T1 has no translator for that coupling. This extends A5 to
resume; it is proposed as A12 (section 10).

## 4. The routine shared by resume and upgrade

### 4.1 The copiers today and what is shared

| Copier                                               | Entry                                                                              | Copies                                                                                                                                                                                          | P19 state                                                                                                                                         | Reports refusal as                         |
| ---------------------------------------------------- | ---------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------ |
| `restoreState` (resume)                              | `agent_executor.cpp:4680`, from `prepare` `:1304` on the first turn of the new pid | scalar fields overwritten, structured fields cloned, profiles, belief clauses with supports and their `d.epistemic` histories, spaces, holons, intentions; dimensions of the base found by name | profiles and histories copied; `last_receive`, `last_contact` cleared (`:4796-4798`); policy hash compared by predicate name (`:4827-4846`)       | `std::runtime_error`, DEBUG, `fatal(init)` |
| `UpgradeGraph` and the translator seam (schema road) | `agent_executor.cpp:5113`; class `:277`; seam call `:5365`                         | fields by schema (add, drop, default), clauses and supports by closure, hooks; `d.epistemic` and `d.trust` skipped (`:343`)                                                                     | none at HEAD: no translator is registered anywhere but one test (`tests/xuzz/vagent_epistemic_ingress_test.cpp:1496`); T0 refuses populated roots | `ActorError`                               |
| `executeReconcile` (reconciler road)                 | `agent_executor.cpp:5453`; `vagent/runtime/reconciler.cpp:1049`                    | clauses (`termFrom`: grab, step, source, `reconciler.cpp:484`), intentions, asks, outbox                                                                                                        | none: no `d.trust`, no `d.epistemic`; policy never compared; T0 refuses populated roots                                                           | `ActorError`                               |

They stay three. They differ in what an upgrade means: resume overwrites a fresh agent from a saved
image, the schema road reconciles two schemas and runs `on Upgrade`, and the reconciler road carries
asks and staged effects. Collapsing them is the P17 pipeline, which the spec does not claim (line
205, "does not claim an automatic full P17 upgrader already exists"). What can be shared, and is, is
the half they all treat as foreign: everything under `d.trust`, `d.epistemic`, the profile `d.clone`
chains and the support histories. One routine owns that half. The rule for the generic halves is
that they must not follow those dimensions:

- `restoreState` drops `d.epistemic` and `d.clone` from its `belief` and `epistemic` pair lists
  (`:4783-4813`) and no longer copies profiles or compares policy. It copies clauses with their
  supports as now, with the `copies` map shared, and then calls the routine.
- `UpgradeGraph` already skips both dimensions; its `translation()` map is the routine's starting
  map.
- The reconciler road refuses (section 4.6).

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

struct TranslationRequest {
  Disposition disposition;
  ClockContinuity clock;                    // Same on in-process upgrade, Changed on resume
  const zigzag::ArenaManifold &from; zigzag::CellRef fromAgent; DimBinding fromDims;
  zigzag::ArenaManifold &into;       zigzag::CellRef intoAgent; DimBinding intoDims;
  const UpgradeSchema &intoSchema;          // the new program's typed predicates
  const TranslatorRegistry &registry;
  std::unordered_map<zigzag::CellRef, zigzag::CellRef> &map;   // old -> new; the caller seeds it
  TransferBudget budget;
};

[[nodiscard]] std::expected<TranslationPlan, TransferRefusal>
planTranslation(const TranslationRequest &);             // read-only: the pre-flight
[[nodiscard]] std::expected<TranslationReport, TransferRefusal>
applyTranslation(const TranslationPlan &, const TranslationRequest &);
```

`planTranslation` takes a request whose `into` is optional for the pre-flight (it needs only the new
program's schema); the resume pre-flight passes no `into`. `applyTranslation` is the only writer. It
targets a fresh context that the caller discards on any refusal, so it needs no undo of its own: the
schema road's candidate `State` and context are swapped only at commit (`:5426`), and a resume that
refuses at apply time ends the new actor as today. The old arena is never written.

The routine carries no executor pointer. `U-3` asks for "checked clock and identity bindings": the
request holds `ClockContinuity`, and the profile tokens it carries are checked with
`isPrincipalToken` (`agent_executor.cpp:3425`). The executor's owner fingerprint is not an input in
T1: a principal token names a source, not the operator, and the plan has no use for the owner yet.
I3-4's issuer records are the first consumer and add the field then.

The seam `OwnedStateTranslator` (`agent_executor.hpp:287`) takes `TranslationRequest&` and returns
`std::expected<void, TransferRefusal>`. T1c registers the default at construction.
`setOwnedStateTranslator` stays so a test can inject a failing one for the rollback test.

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
  `UpgradeSchema` (`UpgradePredicate::parameters` with `type` and `key`, `upgrade_schema.hpp:36`). A
  text compare is a compare of the full typed values.
- **The policy** is read back from the master's typed cells (strategy, model, doubles by their value
  bits, durations, optional fields by absence) into an `ArbitrationPolicy` and compared with its
  defaulted `operator==` (`epistemic_policy.hpp:80`). `policyHash(decoded)` must equal the master's
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
  which T1 never returns. T2 gives it content (section 8.4), so the matcher's type is final now.
- **Old predicates with state** that the new program lacks refuse `PredicateRemoved`. Old predicates
  without a policy are ignored. A policy gained or lost is `PolicyChanged` with one side absent, the
  same refusal the schema comparison gives today (`upgrade_schema.cpp:705-715`), now typed and
  applied on the reconciler road too.

### 4.4 The passes

One map, `old -> new`, built in bounded passes. The caller seeds it with what the generic copier
made (clauses, terms, supports, fields). Every loop has a count from the plan, charges `work` per
step through `into.consumeWork()` as `UpgradeGraph::charge` does (`agent_executor.cpp:285`), uses an
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
observations" order of the spec; the health records of I3-2 revision 4 hang on the profile and move
with pass 1. A field the registry does not list for a profile is not copied silently: pass 0 refuses
`UnregisteredOwnedStructure`.

The routine never copies a raw old `CellRef` as a value. A `CellRef` appears only as a map key, and
a field that holds text, an Int64, a Double or a Bool is copied with `canonicalDoubleBits`
untouched: the stored bits move as they are, so `-0.0` stays `-0.0`.

### 4.5 Price and the refusals that bound it

- **Cells.** The owned graph is bounded by `vagent.epistemic_cells = 24,576`
  (`epistemic_limits.hpp`) plus the exempt headroom `cells / 16 = 1,536` of R3-1: 26,112 cells.
  `/tmp/p19-t1d/price.py` computes the rest.
- **Checkpoint limit.** A checkpoint encodes about two operations a cell. 26,112 cells are 52,224
  operations of the 65,536 of `vagent.value_operations` (`agent_executor.hpp:74`), leaving 13,312
  (20.3%) for the rest of the agent. The comment at `epistemic_limits.hpp` records "24k cells
  checkpoint and 32k do not". A resume onto an executor whose `epistemic.cells` is lower than what
  the image holds refuses
  `PeakCopyBudget{savedCount = cells in image, limit = ceiling + ceiling/16}` and does not truncate.
- **Peak copy.** Resume holds the base (the image) and the copy: 2 x 26,112 = 52,224 cells of arena.
  An in-process upgrade holds the old graph and the new copy: the same. `UpgradeGraph` already
  refuses a closure above `maxCells` (`:321`); the routine adds the check against the plan before
  pass 1, so the refusal comes before any write.
- **Workspace.** The map costs about 40 bytes an entry with its buckets: 1.2 MiB at the bound.
- **Work.** About 14 charges a cell over the passes: 360,000 for a full graph. That is 3.6% of the
  default grant (10,000,000, `program.hpp:217`) and 36% of the TUI's grant (1,000,000,
  `agent_tui.cpp:640`), under the plan's half-grant rule. These are estimates. T1c measures them
  with the budget-parity test and records the figures; a worst case above half of the TUI grant
  moves the TUI to a larger grant rather than loosening the rule.
- **Refusals.** `PeakCopyBudget` and `WorkBudget` come from pass 0, so they leave the old actor,
  context and grant as they were.

### 4.6 The two upgrade roads, and the deliberate differences

| Aspect                 | Resume                                                                      | Upgrade                                                                                  | Why                                                                                                     |
| ---------------------- | --------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------- |
| Old graph              | received base, viewed by a bare `ArenaManifold`; dimensions found by name   | the live old context's arena; dimensions by ref                                          | the rebuilt base registers none (`agent_executor.cpp:4684-4688`)                                        |
| Old program known      | no, only the image                                                          | yes                                                                                      | keys are decoded from the graph on both, so one function is tested                                      |
| Clock stamps           | cleared                                                                     | kept                                                                                     | the executor's clock instance changes on resume (`Ingress::clockInstance`, `:770`)                      |
| Policy or key mismatch | refused before a pid                                                        | refused before mutation                                                                  | the same compare                                                                                        |
| Ranked continuations   | same program: preserved and their versions checked (later slices)           | invalidated with `StaleEpistemicQuery` (later slices)                                    | spec 2.2 item 4                                                                                         |
| Pid and grant          | new pid, new grant from the issuer                                          | same pid, replacement grant                                                              | `resume` mints, `upgrade` keeps                                                                         |
| Non-epistemic extras   | `turn_count` raised to wall milliseconds, pending requests fail as migrated | requests, flights and staged effects refuse the schema road                              | unchanged by T1                                                                                         |
| Admission              | none to close: the new pid has no host state                                | closed for the swap (section 5)                                                          | posts can reach a live pid                                                                              |
| Failure                | pre-flight refuses; apply-time failure ends the new actor with a WARN       | pre-flight refuses; apply-time failure discards the candidate; context swapped at commit | `runtime_.upgrade` installs the new context only when the callback succeeded (`mesh_actor.cpp:803-814`) |

**The reconciler road** does not run the routine. It refuses populated state with
`RouteCarriesNoEpistemicState`, and it compares policy and signatures like the schema road, which it
does not today (`reconciler.cpp` has no policy code). T0's rule that it refuses whether or not a
translator is registered stays. `LocalHost::upgrade(pid, CompiledProgram)` retains the program and
forwards to `upgrade(pid, shared_ptr, instructionLimit)` (`program.cpp:377-402`), so the two public
entry points accept and refuse the same state. The by-value overload loses one behaviour: it used to
reach the reconciler road, which carries asks and staged effects, and it now refuses those with
`NotQuiescent`. Only the T0 test uses it (`tests/xuzz/vagent_epistemic_ingress_test.cpp:1473`), and
that test moves to the shared overload's expectations.
`AgentExecutor::upgrade(pid, CompiledProgram&)` stays public for callers that want the reconciler's
carry of asks and effects, and refuses P19 state.

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

`AgentExecutor::quiescence(pid) const -> std::expected<QuiescenceReport, TransferRefusal>`:

| Field                           | Source at HEAD                                                                  | Refuses on which road          |
| ------------------------------- | ------------------------------------------------------------------------------- | ------------------------------ |
| `turnInFlight`                  | `State::inTurn`                                                                 | both                           |
| `hostEvents`                    | `State::hostEvents.size()` (settle and discard events included, I3-2b)          | both                           |
| `posts`                         | `PostBox::pendingFor(pid)` (queued observations, notes, source changes, health) | both                           |
| `choiceDepth`, `marks`          | `vm.choiceDepth()`, `arena.outstandingMarks()`                                  | both                           |
| `mailbox`                       | `runtime_.pending(pid)`                                                         | arbitrating agents, both roads |
| `flights`, `requests`           | `State::flights`, `State::requests`                                             | schema road                    |
| `deliveries`                    | `outstanding_` entries owned by the pid                                         | schema road                    |
| `stagedOutbox`, `pendingSettle` | `linked(agent, d.outbox)`, `linked(agent, d.pending)`                           | schema road                    |
| `admission`                     | `Open`, `Paused` or `Swapping`                                                  | informational                  |

`upgradeRefusal` (`agent_executor.cpp:5069`) is rewritten to build this report and return
`NotQuiescent{subject = field name, savedCount = value}` for the first non-zero field in the order
of the table. The checks are those of T0 (`:5075-5093`) plus `mailbox`. The T0 tests keep their
assertions through the `operator==` of section 2.2.

The mesh mailbox is the third queue. At HEAD the schema road ignores it unless an event layout
changed (`:5154-5160`); the reconciler road ignores it. For an arbitrating agent T1 refuses any
non-empty mailbox: I3-4's verification messages arrive there, and whether intake is
program-independent is V-D's to settle. The executor cannot close the mesh mailbox, but it needs no
gate for it. `upgrade` is an owner-thread API (`agent_executor.hpp:271-272`) and the runtime's
`receive` runs on the same thread, so the count read at the start is the count at the swap. A test
sends a message through `runtime().receive`, expects `NotQuiescent{mailbox, 1}`, pumps, and then
upgrades.

### 5.2 The post that races the swap

Posts come from other threads: `observe`, `bindSource`, `unbindSource`, `retireSource`,
`healthAction` (I3-2c1) and `percept` all take the ingress mutex or the post-box mutex. The lock
order is ingress then post box. At HEAD `pendingFor` is read, the swap runs, and a post made in
between is consumed by the new program, with an item that was checked against the old binding.

`Ingress` gains `admission[pid]`: `{paused-until, swapping}`. `upgrade` does, in one critical
section under the ingress mutex: `quiescence(pid)`, and if it is quiet, sets `swapping`. A guard
object clears `swapping` on every exit, including exceptions, after the commit or the refusal.
`observe`, `bindSource`, `unbindSource`, `retireSource` and `healthAction` read the flag first,
under the same mutex, before they read a binding or push, and refuse
`EpistemicRefusal::AdmissionClosed` (new, appended, `OutcomeClass::Retryable`). It is not logged to
the outcome log, not charged to the source, and not posted as a refusal note: the host closed the
door, the device did nothing. A post is therefore either before the check (it is in `posts`, and the
upgrade refuses `NotQuiescent{posts}`) or after the flag (it is refused). There is no third case.

| Post kind                    | Program-dependent?                                                              | Rule                                                                                        |
| ---------------------------- | ------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| Observation (`ingest`)       | yes: checked against the binding's policy, parameter kinds and domain           | gated; before the gate it is counted in `posts`                                             |
| Source change, health action | no: a principal and an incarnation or action, run by routines of the same shape | gated, so the translation sees one state                                                    |
| Refusal note                 | no: made only by `observe`, which is gated                                      | counted in `posts` if made earlier                                                          |
| Percept, effect result       | not epistemic                                                                   | unchanged from T0: kept for the new program; an effect result needs a flight, which refuses |

A percept posted against the old event layout during a swap is consumed by the new program. That is
the existing behaviour (`percept` returns `void` and has no refusal to give), it concerns no P19
state, and it is recorded as a limit in the user guide.

### 5.3 The operator path

An actor under steady ingestion is rarely quiescent, so the path is explicit:

1. `pauseAdmission(pid, ttl)`. From then on the five calls above refuse `AdmissionClosed`. Readings
   already queued still run. `ttl` is optional and defaults to the setting
   `vagent.admission_pause_ms` (default 30,000, range 1,000 to 600,000; a `defaultSettingSpecs()`
   entry and an expected-list entry in `system_docs_test.cpp`). The pause expires lazily: each
   admission call compares the steady clock with the deadline, so no timer thread exists, and a
   forgotten pause cannot close a source for ever.
1. Run the reactor until `quiescence(pid)->quiet()`. In tests that is `LocalHost::pump`; in a host
   it is its own loop. The executor does not drive the reactor.
1. `upgrade(...)`. A paused or swapping actor still upgrades; the swap gate is separate from the
   pause and is not subject to the ttl.
1. `resumeAdmission(pid)`. It is idempotent and is also what `forget(pid)` does when an actor stops.

`LocalHost::upgradeQuiescing(pid, program, timeout)` is steps 1 to 4 with `pump`. It is the only
form the tests and the user guide use for a populated agent. Exposing the sequence on the `vagent`
command line is W1's.

### 5.4 Host state, kept and lost

The pid survives an upgrade and a resume mints a new one, so the two lose different things.

| State                                                  | In-process upgrade                                               | Resume                                                                        |
| ------------------------------------------------------ | ---------------------------------------------------------------- | ----------------------------------------------------------------------------- |
| Bindings and aliases (`Ingress::bindings`, `aliases`)  | kept; D3 (section 6) says why they stay valid                    | lost; the host rebinds, and the arena's incarnation floor refuses a lower one |
| Incarnation floors the host knows                      | kept                                                             | lost; relearned from the first stale advance turn                             |
| Outcome log and newest change outcomes                 | kept                                                             | lost; a resend meets the arena's dedup window and answers `Duplicate`         |
| Queue counters, open charges                           | zero: the post box is empty at the check                         | lost                                                                          |
| `last_receive`, `last_contact` (arena)                 | kept: the executor's clock instance is the same                  | cleared (`:4796-4798`)                                                        |
| `clockInstance`, `wallSynchronised`                    | the executor's own                                               | the new executor's own                                                        |
| Health grants, tokens and the reference grant (I3-2c1) | kept: they name the pid and the principal, not the program       | lost; operators re-grant                                                      |
| `ResetMark`, the in-flight set (I3-2c1)                | kept; the in-flight set is empty at quiescence                   | lost                                                                          |
| Estimator replay mark (I3-2a, R3-5)                    | kept                                                             | lost; one replay can count again, the bound R3-5 states                       |
| Latch cache (I3-2c2, R3-13)                            | cleared (R3-13)                                                  | cleared                                                                       |
| Audit sink, health ticket counter                      | the executor's, kept                                             | the new executor's                                                            |
| Queued health actions, queued readings                 | refuse the upgrade (`posts`)                                     | refused earlier by `checkpoint` (`HostDeliveryPending`)                       |
| Admission pause                                        | kept until `resumeAdmission` or its ttl                          | not applicable                                                                |
| Adapter `State`: flights, requests, `schedule`         | flights and requests refuse the schema road; schedule recomputed | rebuilt; pending requests fail as migrated (`failMigrated`, `:1407`)          |

The user guide gains this table (T1c). Nothing in the left column can admit a replay: the profile's
last sequence, dedup window and incarnation are arena state and move with the profile.

## 6. D3: what a policy change does to source bindings

T0 found that a binding copies the policy it was made under (`Ingress::Binding::policy` and
`domain`, `agent_executor.cpp:707-716,3444-3453`) and survives an upgrade. A binding is an
authorization the operator gave a source to speak under one policy for a lifetime and an alias. It
is not a property of the program.

**Ruling.** T1 refuses a policy change on every road, so a binding cannot be stale after a commit.
The schema road already does (`UpgradeChangeKind::EpistemicPolicy`, `upgrade_schema.cpp:705-715`,
`upgradeChangeRequiresRefusal` `:728`), as an untyped `ExecutionFailed`; T1 types it
(`PolicyChanged`) and adds the reconciler road. The refusal holds whether or not the agent has a
profile or a binding, which is what keeps the rule simple.

**T2's migration revokes; it does not rebind and does not migrate in place.** When T2 accepts a
typed policy migration for a predicate, the commit unbinds every binding of the pid for that
predicate in the same ingress critical section as the swap, finishes queued items for them
`UnknownSource`, and keeps the profiles. The operator rebinds under the new policy. The reasons:

- Rebinding by the host would stamp the new `domain` and `policy` on readings from a source whose
  operator authorized the old one. That is an authorization nobody gave.
- Migrating the cached copy in place has the same defect with less visibility.
- Revoking costs one `bindSource` per source, which is idempotent for a live profile at the same
  incarnation (`Ingress::Floor::strict` is false for it, `:732-737`). The profile, its latch and its
  floor survive, so nothing is lost but the authorization.

A test in T1c asserts both halves of what T1 can: a changed policy is refused and the binding still
accepts a reading afterwards; an unchanged policy upgrades and the binding accepts without a rebind.
The revoke path is T2's, with a test that a migrated predicate's bindings are gone and its profiles
are not.

## 7. The coverage oracle

A structure added without a translation must fail a test. Three checks, from cheapest to latest:

1. **The registry.** Each area's ABI header declares what it owns, once, as constants:

   ```cpp
   struct OwnedStructure {
     std::string_view id;           // "profile.streams"
     std::string_view dimension;    // "d.trust"
     std::string_view where;        // "profile field", "profile list", "support rank", "predicate master"
     Fate resume, upgrade;          // Carried, Derived, Dropped
     std::uint32_t since;           // shape version that introduced it
   };
   ```

   `Derived` means the new program re-mints it and the translator only compares it (the shape
   header, the policy master). `Dropped` is allowed only with a reason text (liveness stamps on
   resume). Headers: `runtime_abi.hpp` for the I3-1 structures (T1b adds the include), then
   `epistemic_abi_health.hpp`, `_calibration`, `_ledger`, `_verify`, `_query` as their slices create
   them (plan section 4.2). `epistemic_translate_registry.hpp` concatenates them into
   `allOwnedStructures()`.

1. **A runtime check.** Pass 0 walks the old agent's graph along every dimension named `d.trust`,
   `d.epistemic` or `d.clone`, plus every field name of a profile or record, and refuses
   `UnregisteredOwnedStructure` for anything the registry does not own. A structure that a later
   slice adds and forgets to register therefore refuses a populated actor instead of being dropped
   by the next resume.

1. **The tests and the script.** A translator test calls `coversStructure("profile.streams")` for
   each id it checks. A gtest `Environment` writes the set to `build/translator-covered.txt` at
   exit. `tools/check-translator-coverage.py` (new, T1c) compares four lists and fails on any
   difference: the ids parsed from `epistemic_abi_*.hpp` and `runtime_abi.hpp`; the ids in the
   covered file; the dimension names grepped from `ensureDimension("d....")` in `vagent/runtime/`
   against the `dimension` fields of the registry plus an explicit `kNotOwned` list with one reason
   each (`d.outbox`, `d.belief` and so on); and the ids a test registers that the registry does not
   declare. `make lint` does not run it; `make test` does, after `vagent_actor_test`, and the plan's
   iteration-end gate reruns it.

The registry also has one gtest that needs no other test: ids are unique, every `dimension` is one
`registerDimensions` makes (`runtime_abi.hpp:72-96`), and every `Dropped` fate has a reason.

The structures that exist after I3-2 (the first rows are in the code at HEAD; the health rows are
from the I3-2 design and are confirmed by that slice's code):

| Id                        | Where                                                                                                                                                    | Resume               | Upgrade              |
| ------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------- | -------------------- |
| `agent.shape_header`      | agent `d.epistemic` head                                                                                                                                 | Derived              | Derived              |
| `agent.profiles`          | agent `d.trust` rank                                                                                                                                     | Carried              | Carried              |
| `profile.scalars`         | `incarnation`, `faulted`, `rejection_streak`, `reject_count`, `content_refusals`, `refused_count`, `last_refusal`, `retired` (`runtime_abi.hpp:635-651`) | Carried              | Carried              |
| `profile.liveness`        | `last_receive`, `last_contact`                                                                                                                           | Dropped (new clock)  | Carried              |
| `profile.streams`         | `streams` record, per-predicate last sequence                                                                                                            | Carried              | Carried              |
| `profile.dedup`           | `dedup` list of (incarnation, event, digest)                                                                                                             | Carried              | Carried              |
| `profile.views`           | the profile master's `d.clone` chain, one view per record                                                                                                | Carried              | Carried              |
| `profile.health_scalars`  | `reset_epoch`, `action_seq`, `fault_seq`, `health_cells`, the two Double weights, the `health` and `health_fraction` stamps (I3-2 sections 6.1 and 13)   | Carried              | Carried              |
| `profile.reasons`         | `reasons` list with `origin` (I3-2 6.1)                                                                                                                  | Carried              | Carried              |
| `profile.actions`         | `actions` list, the audit window (I3-2 6.1)                                                                                                              | Carried              | Carried              |
| `support.history`         | each support's `d.epistemic` rank of observation records                                                                                                 | Carried              | Carried              |
| `record.fields`           | the positional fields of `kObservationOrder` and the named ones, `retired_by` included                                                                   | Carried              | Carried              |
| `profile.retired_chain`   | records unlinked from a support and kept to the `retained` bound                                                                                         | Carried              | Carried              |
| `predicate.policy_master` | predicate `d.epistemic` head                                                                                                                             | Derived and compared | Derived and compared |

Host-only state (section 5.4) has its own short list in the same file, `kHostState`, so the
user-guide table and the tests read one source.

## 8. Slices

Size: the plan estimated T1 at about 1,500 lines. This design needs about 2,900, so T1 splits at the
seams the plan allows. Each part builds, passes the earlier tests and is reviewed alone.

### 8.1 T1a: the refusal channel (about 1,000 lines)

- **Content.** Section 2 in full, the pre-flights for the checks that exist today, and the
  quiescence report (5.1, without the gate). Resume refuses a changed policy before a pid (X-9); the
  schema road's `ExecutionFailed` causes become codes; the reconciler road compares policy and
  signature; `LocalHost::upgrade(pid, CompiledProgram)` forwards.
- **Files.** New `vagent/runtime/epistemic_refusal.{hpp,cpp}`;
  `vagent/runtime/agent_executor.{hpp,cpp}` (return types, `preflightResume`, `preflightUpgrade`,
  `quiescence`, at most 400 lines); `vagent/runtime/program.{hpp,cpp}`; `vagent/tui/agent_tui.cpp`;
  `vagent/runtime/upgrade_schema.{hpp,cpp}` (a typed result next to `upgradeChangeRequiresRefusal`);
  `compiler.cpp` (the master's `encoding` and `signature`, at most 40 lines); tests
  `tests/xuzz/vagent_epistemic_refusal_test.cpp` (new), `vagent_epistemic_resume_test.cpp`,
  `vagent_epistemic_ingress_test.cpp`, `vagent_live_upgrade_test.cpp`, `vagent_tui_test.cpp`.
- **Depends on.** T0, E1. It does not need I3-2, and may land before I3-2c2 if the lead wants X-9
  fixed early; the plan keeps the order.
- **C++ and Vortex.** All C++. The pre-flight is host admission, and the refusal type is the
  boundary between the executor and its callers. No Vortex code runs against an arena that is not an
  actor's live context.
- **Price.** No actor cells. Four cells a predicate in the program base.
- **Gate.** ARCH, CODE, DIST.

### 8.2 T1b: the shape version, the guard and admission (about 800 lines)

- **Content.** Section 3 (header, probe, guard, intention rule), section 5.2 and 5.3 (gate, pause,
  `quiescence().admission`, `upgradeQuiescing`), the `AdmissionClosed` refusal and the setting.
- **Files.** New `vagent/runtime/epistemic_shape.hpp`, `epistemic_emit_shape.cpp` (the header);
  `agent_executor.{hpp,cpp}` (gate, at most 400 lines); `epistemic_ingress.hpp` (`AdmissionClosed`,
  appended; `outcomeClassOf`); `program.{hpp,cpp}`; `system_docs.cpp` (`vagent.admission_pause_ms`);
  tests `vagent_epistemic_shape_test.cpp` (new), `tests/xuzz/system_docs_test.cpp`,
  `vagent_epistemic_ingress_test.cpp`.
- **Depends on.** T1a.
- **C++ and Vortex.** C++ for the gate and the pause (thread admission, a clock). The header is
  emitted by the code generator, as every other agent cell is.
- **Price.** Three cells in the program base per program with an arbitrated predicate. No actor
  cell.
- **Gate.** ARCH, DIST, SEC, CODE.

### 8.3 T1c: the routine, the digest and the oracle (about 1,100 lines)

- **Content.** Sections 4, 5.4 and 7: `translateEpistemicState`, the matcher, `discoverDimensions`,
  the digest, the default translator, the registry, the script, the user-guide table. `restoreState`
  loses its profile and policy code and calls the routine. The seam's signature changes (4.2).
- **Files.** New `vagent/runtime/epistemic_translate.{hpp,cpp}`, `epistemic_translate_registry.hpp`,
  `tools/check-translator-coverage.py`, `tests/xuzz/vagent_epistemic_translate_test.cpp`;
  `agent_executor.{hpp,cpp}` (calls, at most 300 lines); `runtime_abi.hpp` (the owned list and one
  include); `docs/vagent-user-guide.md` (host-state table and the operator path); `Makefile`
  (`test/translator-coverage`, run by `test`).
- **Depends on.** I3-2c2, T1a, T1b. The health rows of the registry are the code of 2c2, not the
  design's.
- **C++ and Vortex.** All C++, with the same reason: the routine copies one arena into another
  between two contexts, inside the host's atomic upgrade callback or before a pid exists. A Vortex
  routine would need a context that holds the old graph, and the old graph is read-only data during
  the swap. The bounded loops are plain C++ over the existing arena API. Nothing that decides
  anything about a source (health, scoring, calibration) is in it; those stay Vortex.
- **Price.** Section 4.5. No actor cell in steady state; a transient peak of 2 x 26,112 cells and
  1.2 MiB of map. The budget-parity test (plan section 2) re-measures
  `AFullEpistemicBudgetStillCheckpoints` and records the work figure.
- **Gate.** ARCH, DIST, CODE, EPI, SEC. The plan lists these five for T1.

### 8.4 T2: typed migrations and the claim

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

Names are indicative. A test registers the ids it covers with `coversStructure`. "Unchanged" means
the context pointer, the grant, the latch, both program bases (a digest of every cell's text and
value bits) and the arena's cell count are as before.

| Slice | Test                                                            | Asserts                                                                                                                                                                                                                                    |
| ----- | --------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| T1a   | `RefusalVocabularyIsStable`                                     | every code has a `toString`, an `actor` and the fields of the table in 2.2; a text over 128 bytes is cut; `operator==` against `ActorError`                                                                                                |
| T1a   | `ResumeOfAChangedPolicyUnderTheSameReferenceRefusesBeforeAPid`  | two programs with one explicit reference (`fromUnit`), different `decay`: `code == PolicyChanged`, subject, both hashes; no pid; red at HEAD (X-9)                                                                                         |
| T1a   | `ResumeOfAChangedPolicyTagNamesTheTag`                          | a master whose `encoding` differs: `PolicyEncodingChanged{saved, current}`                                                                                                                                                                 |
| T1a   | `AResumeRefusalLeavesNoTrace`                                   | `actorCount`, the issuer's issued count, trusted nodes and `needsFirstTurn_` as before                                                                                                                                                     |
| T1a   | `ASecondPlanFailingAfterAGoodPreflightEndsTheActorWithAWarning` | a source that serves a different carrier on the second read: `fatal(init)`, one WARN naming the code                                                                                                                                       |
| T1a   | `PreflightResumeAndPreflightUpgradeAreDryRuns`                  | same answer as the real call, no change                                                                                                                                                                                                    |
| T1a   | `EveryUpgradeRefusalIsTyped`                                    | schema road: each of `NotQuiescent{reason, count}`, `ActiveIntention`, `PolicyChanged`, `PredicateIncompatible`, `PredicateRemoved`, `PeakCopyBudget`, `WorkBudget`; each unchanged                                                        |
| T1a   | `TheReconcilerRoadComparesPolicy`                               | a bound arbitrating agent with no profile and a changed policy: `PolicyChanged` (red at HEAD, which accepts it)                                                                                                                            |
| T1a   | `ThePublicEntryPointsAgree`                                     | `LocalHost::upgrade` by value and by pointer give the same code for the same state                                                                                                                                                         |
| T1a   | `TheTuiPrintsTheTypedReason`                                    | the action log holds `toString`, not an integer                                                                                                                                                                                            |
| T1b   | `TheHeaderIsMintedOnlyForAnArbitratingProgram`                  | present for one, absent for the other; value 1                                                                                                                                                                                             |
| T1b   | `ANonArbitratingAgentUpgradesUnchangedAfterT1`                  | the agent with no header upgrades; fields, timers and `on Upgrade` as before                                                                                                                                                               |
| T1b   | `AnArbitratingAgentWithNoProfileUpgradesAndResumes`             | the header does not make the agent populated                                                                                                                                                                                               |
| T1b   | `TheGuardIgnoresTheHeaderAndReadsTheRest`                       | table 3.2, every row; `UnknownShapeVersion` carries the numbers; absent header with profiles                                                                                                                                               |
| T1b   | `TheSavedHeaderIsComparedAndNotCopied`                          | after resume and after upgrade the header cell is the program's own, by ref                                                                                                                                                                |
| T1b   | `AnActiveIntentionRefusesOnEveryRoadForAnArbitratingAgent`      | resume, schema road, reconciler road: `ActiveIntention{savedCount}`; a non-arbitrating agent still resumes and reconciles with an intention                                                                                                |
| T1b   | `APostBeforeTheGateIsCountedAndAPostAfterItIsRefused`           | a queued reading: `NotQuiescent{posts, 1}`; inside the swap (a test observer on the executor) `observe`, `bindSource`, `retireSource` give `AdmissionClosed`, uncharged, unlogged, no note                                                 |
| T1b   | `AdmissionReopensOnEveryExit`                                   | after a refusal, after a commit, after an exception in the callback                                                                                                                                                                        |
| T1b   | `TheMailboxRefusesAnArbitratingUpgrade`                         | `runtime().receive` of a message: `NotQuiescent{mailbox, 1}`; after `pump` it upgrades; a non-arbitrating agent with the same mailbox upgrades as before                                                                                   |
| T1b   | `ThePauseExpires`                                               | a fake steady clock past the ttl reopens admission without a call                                                                                                                                                                          |
| T1b   | `UpgradeQuiescingDrainsAndUpgrades`                             | a flooded source, then a committed upgrade and a resumed admission                                                                                                                                                                         |
| T1c   | `PredicateKeysDecodeAndRoundTrip`                               | `decode(mint(p)) == p`, every optional present and absent; the stored hash is verified                                                                                                                                                     |
| T1c   | `AChangedSignatureIsNotCompatible`                              | arity, key position, parameter type and frame each give `PredicateIncompatible` or `PolicyChanged`; unchanged state; old actor still ingests                                                                                               |
| T1c   | `AForcedFullKeyHashCollisionDoesNotMatch`                       | the matcher with a constant `KeyDigest`: two predicates that differ in one parameter type are not matched; matches equal the default's                                                                                                     |
| T1c   | `PopulatedStateSurvivesResumeAndUpgrade` (spec 8.2 gate 5)      | two sources, one shared clause, current and retired records, a latch, a reason: digests equal across source, resume and upgrade `ModuloClock`; clone occurrences, shared-clause topology and retired chain intact; program bases unchanged |
| T1c   | `ResumeAndUpgradeAgreeOverRandomIngestion`                      | 24 fixed seeds, up to 40 events (readings, refusals, advance, retire, rebind, and from 2c2 isolate, clear, reset): the same digest on both roads                                                                                           |
| T1c   | `AFailingOnUpgradeRollsBackTheTranslation`                      | the hook fails after the routine ran: unchanged, no new cells visible                                                                                                                                                                      |
| T1c   | `AFailingTranslatorRollsBack`                                   | an injected translator that returns a refusal: unchanged                                                                                                                                                                                   |
| T1c   | `AFullBudgetResumesAndUpgradesWithinThePeak`                    | `AFullEpistemicBudgetStillCheckpoints` at its bound, then both roads; work and peak recorded; `PeakCopyBudget{needed, limit}` on a host with a lower ceiling                                                                               |
| T1c   | `ALatchAndReasonsSurviveBothRoads`                              | `faulted`, `fault_seq`, a reason and its provenance, byte-equal; the latch is never cleared                                                                                                                                                |
| T1c   | `LivenessStampsFollowTheDisposition`                            | resume clears `last_receive` and `last_contact`, upgrade keeps them                                                                                                                                                                        |
| T1c   | `DoublesCrossByBitPattern`                                      | a `-0.0` health weight stays `-0.0` on both roads                                                                                                                                                                                          |
| T1c   | `ARecordWithoutAProfileIsInconsistent`                          | `StateInconsistent`                                                                                                                                                                                                                        |
| T1c   | `AMissingStreamsListIsReplayStateUnavailable`                   | `ReplayStateUnavailable{principal, "streams"}`                                                                                                                                                                                             |
| T1c   | `AnUnregisteredDimensionRefuses`                                | a profile with an extra owned dimension: `UnregisteredOwnedStructure{dimension}`                                                                                                                                                           |
| T1c   | `BindingsSurviveAnUnchangedPolicyAndAChangedOneIsRefused` (D3)  | an unchanged upgrade accepts a reading without a rebind; a changed policy is refused and the binding still accepts                                                                                                                         |
| T1c   | `RegistryIsConsistent`                                          | unique ids; every `dimension` registered; every `Dropped` has a reason                                                                                                                                                                     |
| T1c   | `TheCoverageScriptFailsOnAnUncoveredStructure`                  | a fixture registry with one extra id and no test: the script exits non-zero                                                                                                                                                                |

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

1. **Size.** T1 is 2,900 lines in this design against 1,500 in the plan. T1a, T1b and T1c are the
   split; the plan's totals move by about 1,400.
1. **Name.** `TransferRefusal`, not `EpistemicRefusal` (taken, `epistemic_ingress.hpp:68`).
   `cluster_node.cpp` needs no edit.
1. **Resume and intentions.** A12 refuses resume for an arbitrating agent with an intention. Today
   it works. The alternative is to refuse only when profiles exist.
1. **Reconciler road.** It now compares policy and signature, and
   `LocalHost::upgrade(pid, CompiledProgram)` now reaches the schema road, which refuses asks and
   staged effects the reconciler used to carry. The alternative is to keep the by-value overload on
   the reconciler road and give it the translator, at the cost of a second copy of the passes.
1. **Master fields.** `encoding` and `signature` join the policy master (shape 1). Without them
   neither the tag nor the full key is readable from a checkpoint.
1. **Setting.** `vagent.admission_pause_ms` is new.
1. **TUI grant.** Its 1,000,000-instruction grant leaves 36% of the work for a full graph; a larger
   grant is a one-line change if the T1c measurement disagrees.
