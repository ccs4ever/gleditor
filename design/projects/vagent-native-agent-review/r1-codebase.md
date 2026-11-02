# Round 1 critique: Codebase Expert (vagent-native-agent.md)

Paths below are relative to the repo root. Runtime files are under
`apps/common/xanadu/vagent/runtime/` unless stated. Tally: 3 BLOCKING, 5 MAJOR, 4 MINOR.

## Claims verified as accurate

- `AgentProgram` is at `agent_executor.hpp:107` with exactly `programRef()` and `instantiate()`.
- `EffectRequest` (`host_bindings.hpp`) holds only `token`, `method`, `args`. `HostBindings::start`
  takes `(name, method, args, preempting, sink)`. No agent identity anywhere. The call is at
  `agent_executor.cpp:3895`, the `dynamic_cast<const CompiledProgram *>` at `:3271` returning
  `NotArbitrated`. P20.1 is real and correctly scoped.
- `AgentExecutor::upgrade` takes `std::shared_ptr<CompiledProgram>` (`agent_executor.hpp:252-255`);
  the executor also downcasts at `agent_executor.cpp:4906`.
- `percept` is "Callable from any thread" (`agent_executor.hpp:334-341`). `EffectCompletion` may be
  called from any thread and only the first call counts (`host_bindings.hpp` header comment).
- `refOf` is `kProgramFormat ‖ '\0' ‖ source` hashed (`program.cpp:18-27`); `fromSource` is
  `program.cpp:31-110+`.
- No opcode allowlist exists in `program.cpp`, `compiler.cpp` or `mesh_actor.cpp`. The VM treats an
  unrecognised kind as a no-op (`vortex/vortex_vm.cpp:1220-1223`). Spec line 1605 claims one; the
  design's reading of that gap is correct.
- `WireValue`, `encodeValue`/`decodeValue`, `WireError{TooDeep,TooLarge,...}` are as cited
  (`wire.hpp:37-98`). `ValueLimits::operations` is 65536 (`vortex/mesh_value.hpp:28`). `posix_spawn`
  is used in `provenance.cpp:245-249`.

## BLOCKING

### 1. (§4 N4, §14.1, P20.2) There is no scaffolding-compiler entry to call, and the proposal does not price creating one

**Problem:** N4 rests on "the control plane replays declarations through the actor compiler's own
scaffolding ... splices each fragment in where that compiler would have lowered a body", and P20.2
presents this as "expose the scaffolding emitters to a second front end". The compiler has no such
seam.

**Evidence:** The only entry is
`Compiler::compileAgent(const Unit &unit, std::string_view agentType)` (`compiler.hpp:~65`). All
emission lives in `class Gen` in an anonymous namespace (`compiler.cpp:411` to about `:7230`),
driven by walking the AST (`agent_->members`, `compiler.cpp` near `:7197`:
`for (const auto &member : agent_->members)`). Genesis, dispatcher, symbol table,
`emitFieldDefault`, `emitUpgradeHook`, `buildProgramStructure` are private members consuming
`Agent`, `HandlerInfo`, `PlanInfo` and so on (`compiler.cpp:204-282`) built from AST nodes. There is
no "body" callback to splice a prebuilt fragment into. `compiler.cpp` is about 7,250 lines. §15
refuses "Native code builds the AST, and the control plane runs the vagent checker", yet an AST is
the only input the existing scaffolding accepts; §14.1's `scaffold.cpp` "uses the actor compiler's
emitters" is a promise with no referent.

**Fix:** Either (a) price P20.2 honestly as a refactor of `Gen` that separates "declaration model"
(the `HandlerInfo`/`GoalInfo`/`PlanInfo` structs) from "body lowering", name the new seam (for
example a `BodySource` interface with a `lower(...)`/`splice(fragment)` alternative), give a size
estimate and the regression gate (`vagent_compiler_test`, `vagent_runtime_compiler_test` must be
byte-identical on `programRef`), and give a real fallback; or (b) reopen the refused alternative and
let `vn_*` declarations build a synthesised `Unit` (declaration nodes only) with fragments attached
to body nodes as an opaque `RawBody` AST variant, which touches `ast.hpp`, `checker.cpp` and
`compiler.cpp` instead. Either way the register must list `ast.hpp`/`checker.cpp` if the checker
gates the declarations. The current fallback ("a second scaffolding implementation") contradicts
N4's own reason for existing.

### 2. (§3 C12, §7.3, §14.2 P20.6) The drain skips a malformed entry today; C12 "Unchanged" is false, P20.6 is not conditional, and its mechanism is not a drain change

**Problem:** The table says C12 is "Unchanged; the drain additionally treats a malformed outbox
entry as a hard failure", and §7.3/P20.6 say the proposal is "withdrawn" if the drain already fails
the turn. It does not.

**Evidence:** `collectOutbox` does `bumpStat(state, kStatMalformed); continue;` for an event entry
without a record (`agent_executor.cpp:2246-2252`), for an effect entry missing `skill`/`capacity`/
`method`/`deadline` or with non-list args (`:2379-2386`), and counts unknown kinds
`kStatUnsupported; continue` (`:2446-2448`). Further, drain runs at step 9, after the turn has been
committed (`agent_executor.cpp:2180-2184`, `arena.discard(m0)` at `:2156`): at that point the turn
cannot be "released, nothing drained".

**Fix:** State in §3 that C12 is not satisfied today and the test will find the defect for certain.
Re-specify P20.6 as a pre-commit validation pass: after the handler body and before the
`discard(m0)`/`release(m0)` decision (`:2140-2158`), walk the outbox with the same decoder as
`collectOutbox` and set `failure` on any undecodable entry, so the existing `release` path runs.
List the cost (an extra outbox walk per turn for every program, or only when `program->isNative()`),
and note that `kStatMalformed` stays for compiled programs or is made unreachable. Make it
unconditional in the register.

### 3. (§1 part 5, N10, §8.1) "Leverages Vortex's multiprocess support ... in the manner of the Spool-Mesh worker lifecycle (`MeshHost`, `mesh_cli`, incarnation epochs)" cites a process model that does not exist

**Problem:** The doc claims the supervisor is modelled on mesh worker-process lifecycle. The tree
has no worker-process supervision in `vortex/`.

**Evidence:** `grep` for `posix_spawn|fork|execv|socketpair|memfd` under `apps/common/xanadu` finds
only `provenance.cpp` (a one-shot GnuPG pipe capture, `:240-262`: no socket, no supervision, no
restart). `mesh_host.hpp:146` "device epoch" is a persisted per-device counter reserved at start-up,
not a per-child incarnation. `mesh_cli.cpp` mentions epochs only as an `AUTHOR:DEVICE:EPOCH`
argument (`:53, :237`). `vortex-multithreading-study.md` describes cursors in one arena, not
processes. The owner's request names "vortex's multiprocess support" as something to leverage; the
design neither points at real support nor says there is none.

**Fix:** Replace the §1 reading with the truth: the only reuse is the codec conventions and the
`ActorGrant`/epoch vocabulary; process spawn, supervision, rings, incarnations and restart policy
are all new code (the new `supervisor.cpp`). Say so as a deviation from the request, price it, and
record in §15 that no existing multiprocess worker exists to extend. If a real precedent is intended
(`design/vortex-mesh-cli.md`, `vortex-distribution.md`), cite its file and line for the code that
implements it.

## MAJOR

### 4. (§4 N5, §5 "Load", §16 "Load-time replay") Per-programRef replay cost is wrong: `instantiate` recompiles per agent, per resume

**Problem:** N5 says "replay costs O(ops) at load, paid once per `programRef` per node under P17's
shared base". Today a program's `instantiate` builds the agent into each agent's own arena every
time.

**Evidence:** `CompiledProgram::instantiate` constructs a `Compiler(context.core)` and calls
`compileAgent(unit_, agentType)` (`program.cpp:148-165`); the adapter calls `instantiate` on an
agent's first turn (`agent_executor.cpp:1174`), on `resume` re-instantiation (`:4500`), and on live
upgrade (`:4984`). The shared base is consulted only through `setProgramResolver`
(`program.cpp:258-264`) as the overlay's read-through; the per-agent arena still holds its own
compiled cells.

**Fix:** Specify what `NativeAgentProgram::instantiate` does. Either re-run the whole replay plus
scaffolding plus splice into the agent's `WorkerContext` at every instantiate (then say so and price
O(ops) per spawn/resume, and put the staging-then-promote path only in a load-time verifier), or
define a new mode that clones from the promoted base (a change to what `CompiledProgram` does too,
hence a proposal). The `sound(...)` cell checks at `agent_executor.cpp:1180-1186` bound what the
returned `CompiledAgent` must hold; list that as part of the contract.

### 5. (§13.2, §10.2, §14.2 P20.3, §14.3) Several existing files need changes that P20.x does not list: `LocalHost`, `AgentSettings`, `apps/vagent/main.cpp`

**Problem:** The test plan and the settings plan assume plumbing that is absent.

**Evidence:**

- `LocalHost`'s constructors and `launch` overload take `CompiledProgram` by value or
  `shared_ptr<CompiledProgram>`; `programs_` is a `map<string, shared_ptr<CompiledProgram>>`;
  `program_->setBeliefEventDepth(...)` is called on it (`program.hpp:~170-215`,
  `program.cpp:250-262`). A `NativeAgentProgram` cannot be hosted by it. §13.2 step 2 ("into an
  `AgentExecutor` with a manual clock ... `HostBindings`") needs `LocalHost` generalised to
  `AgentProgram`, or a second harness.
- No `vagent.*` setting reaches `AgentSettings`: `grep` for
  `vagent.compact_every`/`value_operations` finds only `system_docs.cpp/.hpp`, the executor/limits
  header comments and `system_docs_test.cpp`; `apps/vagent/main.cpp:11-14` says "this command does
  not open the user's system store, so a changed setting does not reach `run` yet". P20.3's
  `defaultSettingSpecs()` entries are therefore inert for all sixteen `vagent.native.*` settings,
  and `grant_policy`/`approvals` cannot be read by `vagent run`.
- `vagent run` has no approval prompt and no `native seal` subcommand (`main.cpp:3` lists `check`,
  `check-runtime`, `run`). §10.2 "vagent run's prompt" and §5 "`vagent native seal module.so`" are
  new command-line surface, not listed.

**Fix:** Add proposals for `program.{hpp,cpp}` (`LocalHost` over `AgentProgram`, or a documented
second test harness), for `agent_executor.hpp` (`AgentSettings` or a sibling `NativeSettings` struct
and who fills it from `system://settings`), and for `apps/vagent/main.cpp` (`seal`, running a
`.vnprog`, the approval callback). Say which of them §13 needs before the first test.

### 6. (§10.1, §14.2) The `std:native` prelude capacity, record and event need `prelude.cpp`, the checker and the compiler, and are not in the register

**Problem:** §10.1 declares `capacity std:native/Authorities`, `record Grant` and
`event std:native/AuthorityRevoked` as "prelude declarations for native programs only". The prelude
cannot declare any of those.

**Evidence:** `prelude.hpp:15-36` exposes only `preludeEvents()` (a `span<const PreludeEvent>` of
fields), `preludeImplicitFields()` and `findPreludeEvent`. There is no table of capacities, records
or effect methods, and no per-program-kind gating. The table is "read by the checker and the
compiler alike" (`prelude.hpp:5-7`), so a native-only addition touches `checker.cpp` and
`compiler.cpp` as well. Separately `record Grant { kind, id, incarnation }` has no `scope`, yet
§10.4 says "The grant record's `scope` carries the terminal type" and §13.1 calls `t.scope`.

**Fix:** Either declare the Authorities capacity through the manifest/declaration ops
(`vn_capacity`, `vn_effect_method`, `vn_record`) emitted by the control plane when `vn_authority` is
present, so it needs no prelude change, or add a proposal for the prelude/checker/compiler extension
with the same gating. Add `scope: Str` to `Grant`, or remove its uses.

### 7. (§12.2, §14.2 closing paragraph) The C6 checkpoint walk edits `agent_executor.{hpp,cpp}` and should be a registered proposal now, not "P20.7 at that point"

**Problem:** The walk needs a new refusal, a `toString` case and an edit to `checkpoint()`.

**Evidence:** `CheckpointRefusal` is a closed enum (`agent_executor.hpp:153-163`) with a `toString`
switch (`agent_executor.cpp:428-446`); the nine enumerators are all returned from `checkpoint`
(`:4771-4814`). Adding "StateLinksProgram" is a change to a public enum, the switch, and the body.
The §12.2 question about whether the existing path "would silently drag the program into the
checkpoint" is answerable by reading: `promote` skips expansion of any cell that is not ephemeral
and not `holdsOwn` (`zigzag/arena_manifold.cpp:1778-1786`, "A base cell only read through the
overlay is an existing endpoint, not a request to copy"), and `checkpointView`
(`vortex/mesh_actor.cpp:1004-1030`) does no walk. So the program is not copied; the question is what
the state's link to a base cell becomes in the promoted store (I did not trace the emitted link
target; the design should).

**Fix:** Register it as P20.7 now with the files above. Specify the check as a scan of state-cell
edges whose far endpoint is `!holdsOwn` and not a legitimate base reference (the checkpoint
continues to need base references for compiled programs, so define which dimensions are allowed),
not a "reachability walk of the agent cell".

### 8. (§14.1, P20.5) `apps/common/xanadu/vagent/native/*.cpp` is globbed into every binary

**Problem:** §14.1 says "Nothing in `apps/common/xanadu/` links libffi or libseccomp ... only into
`vagent-native-host`", while placing `sandbox_linux.cpp` (Landlock/seccomp) there.

**Evidence:** `COMMON_XANADU_SRCS := $(shell find apps/common/xanadu -name '*.cpp')`
(`Makefile:633`), `XUDU_CORE_SRCS := $(COMMON_XANADU_SRCS)` (`:635`), and `XUDU_CORE_OBJS` is linked
into `xuzz`, `vquery`, `vagent`, and the test binaries. Every `native/*.cpp` is therefore compiled
into each, and a `#include <seccomp.h>` breaks a machine without libseccomp.
`VAGENT_RUNTIME_CORE_OBJS` (`:686`) filters `vagent/` objects out of one test binary by the same
glob, showing the pattern needs explicit handling.

**Fix:** P20.5 must state the Makefile treatment: filter `vagent/native/{sandbox_linux,host_*}.cpp`
out of `XUDU_CORE_SRCS`, give them to `vagent-native-host` only, and gate on
`GLEDITOR_ENABLE_NATIVE_AGENTS`. Or move host-only code under `apps/vagent-native-host/` (see the
architect critic for layer placement) and keep only device-free, dependency-free code (op log,
manifest, frames) in `apps/common/xanadu/`. Also add the host binary to `TEST_PROGRAMS`
(`Makefile:1334`), which the register does not mention.

## MINOR

### 9. (§9.1, §14.3) `frame_bytes` "matches the mesh's `kMaxPayloadBytes`" is the wrong constant

`kMaxPayloadBytes` is `64 KB per frame` in `identity/identity_layout.hpp:30`, a BEP 10 identity
frame bound, not a mesh bound. Actor messages and mesh outputs are 32 KiB (`wire.hpp:~31` "A message
is at most 32 KiB"; `kLocalMaxOutputBytes = 32768`, `program.cpp:~185`). "Fails `too_large` as an
oversized event does today" is therefore true at 32 KiB, not 64 KiB. `decodeValue` also caps at
`kMaxWireNodes = 16384` and depth 32 (`wire.hpp:32-33`); a 64 KiB frame can hold more nodes than the
decoder accepts. Say which bound governs and cite it.

### 10. (§14.3) `op_log_ops` "matching `ValueLimits::operations`" and `program_cells` 262,144

`ValueLimits::operations` bounds a checkpoint's Structure operations (`mesh_value.hpp:28`), a
different quantity from ops in a native build log. `program_cells` has no stated origin. Either
derive them (measure the cell count of a compiled program such as the existing compiler tests) or
state they are placeholders, as §14.3's last paragraph partly does.

### 11. (§7.2 step 3) The allowlist "the actor compiler emits" cannot be computed by the compiler today

There is no table of emitted opcode kinds; `Gen` emits at hundreds of sites. If P20.2 chooses the
refactor route of finding 1, say how the set is produced (an instrumented sweep of the compiler test
corpus is a test, not a list; a hand-kept list is exactly the drift the proposal tries to avoid).

### 12. (§14.2 P20.4, P20.5) Packaging and installation

`vagent_native.h` "installed by `make dist`" and the second and third test modules imply `Makefile`
`dist` and packaging edits (`debian/`, `packaging/`); list them in P20.5.

VERDICT: NOT CONVERGED
