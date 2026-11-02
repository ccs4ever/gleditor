# Round 1 critique: Vortex & Spool-Mesh Expert (vagent-native-agent.md)

Paths below are relative to the repo root. Doc line numbers are `design/vagent-native-agent.md`;
code paths are under `apps/common/xanadu/`. Tally: 1 BLOCKING, 10 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§4 N4, §6.3, §14.2 P20.2) "Splice each fragment where the compiler would have lowered a body" has no seam to splice into

**Problem:** N4 (lines 148-157) rests on a "new compiler entry point beside `compileAgent`" that
replays declarations through the actor compiler's scaffolding and splices opcode fragments in place
of lowered bodies. P20.2 prices this as "publish the emitted opcode set and expose the scaffolding
emitters". The compiler cannot be cut that way. Scaffolding and bodies are one AST-driven lowering,
and several things the scaffolding needs are derived from bodies, which a native fragment does not
have as AST.

**Evidence:**

- `vagent/runtime/compiler.cpp:92-7228` is a single anonymous namespace. The whole emitter is
  `class Gen` (`:411`), driven by `compileAgent(*unit, agentName)` (`program.cpp:57`, `:152`) over a
  parsed `Unit` and the checker's `CheckedUnit`. There are no "emitters" to expose short of
  re-architecting a 7,266-line file.
- Whether a routine is `cyclic` (and so takes frame temporaries instead of static registers,
  `compiler.cpp:589-596`, `:909`) is computed from the AST: `hasDynamicCall(*mi.def->body, set)`
  (`:6124-6133`). A fragment has no AST, so replay has no call graph to decide it from. A wrong
  answer lets a recursive activation overwrite static opcode outputs, which the runtime grounding
  names as the hazard (`design/vagent-runtime-grounding.md:133-136`).
- Plan suspension is compiler-internal (`compiler.cpp:5-35`): every plan body is compiled with a
  persistent frame stored on `kIntentionFrame`, all locals are frame-backed handles, the k-th
  suspension gets `stepEntries[k+1]`, and the suspension site sets `intention.step`, pops the frame,
  runs `rtSettle_` and jumps to `haltOp_`. `vn_stmt_effect` "assigns step paths" (line 311) only if
  replay reimplements all of this. The doc mentions none of it.
- The statement-wrapper's `Choice` targets are context-dependent: plan contract fail, plan fail, or
  `failStep_` (`compiler.cpp:1028-1039`). "Replay emits the `#ChoiceDepth` spill, choice point and
  `#Cut` to the recorded depth" (line 307) names no target.
- §15 refuses "Native code builds the AST, and the control plane runs the vagent checker" as "vagent
  with another parser". Yet the only way to reach `compileAgent` (AST in, scaffolding and cyclic
  analysis out) is a synthesized declaration AST with stub bodies, and fragments pasted over those
  stubs. That is a hybrid of the refused alternative and the chosen one, and the doc does not say
  so.

**Fix:** Decide which of two real designs N4 is, and price it in P20.2.

(a) A typed fragment IR (locals, calls, statements and expressions) that replay lowers through a new
shared `Gen` backend. This is a refactor of `compiler.cpp` to move frame, poke, step-entry and
cyclic machinery out of the anonymous namespace. Cost: large. Fallback: refuse native programs whose
bodies are not expressible in the IR.

(b) Accept N4's refused alternative in the narrow form of "native code supplies declarations plus
body ASTs; the control plane checks and compiles". Then say that the graph is built by the control
plane's compiler, and say honestly how that meets "constructing its Vortex graph with native ops".

In either case P20.2 must list the Gen refactor, the cyclic call-graph input, the step-path and
frame model, and the per-context fail targets as the deliverables.

## MAJOR

### 2. (§6.3) The call, return and frame ABI does not match the VM

**Problem:** §6.3 says "Locals live in frames ... Replay emits the spills around every `#Call`" and
"`#Call` targets must be fragments or callable prelude entries". The real conventions are narrower,
and the doc's version would produce graphs that misbehave.

**Evidence:**

- `#Call` pushes a return frame only if the call op has a `d.spin` successor
  (`vortex_vm.cpp:792-805`, `if (nextOp)`). A `#Call` that ends a fragment never returns. The
  compiler always follows it with a `Nop` "ret" (`compiler.cpp:774-777`).
- `#Return` pops the cursor's `d.stack` top (`vortex_vm.cpp:832-847`). The compiler's own plan and
  method frames chain on the same `d.stack` and deliberately do not use `#Call`'s frame, because
  "compiled code has no way to name the cursor" (`compiler.cpp:32-36`). A fragment `#Return` inside
  a compiler frame pops the wrong thing.
- Jump, Branch, Call and Choice targets are raw `Int64` scalar cells holding a `CellRef`
  (`compiler.cpp:582-586`, `vortex_vm.cpp:791-829`, `:1135`). The doc says ids "never denote a
  `CellRef`" and replay "maps them" (line 274), but never says how an `Int` operand is retargeted
  when the staging arena is promoted. `program.cpp:101-133` remaps only the entry points and never
  rewrites operand scalars. P17 says targets "name base `CellRef`s directly"
  (`vagent-shared-program-and-live-upgrade.md:223`), which needs a two-pass build.

**Fix:** State the fragment calling convention against the real one. Say that fragments never
contain raw `#Call`, `#Return`, `#Halt`, `#Jump`, `#Choice` or `#Cut`. They contain only `vn_call`,
`vn_ret`, `vn_end` and `vn_alt`, which replay lowers with the `Nop` continuation and a settle jump.
Add the retargeting pass explicitly and cite P17.2 as its owner.

### 3. (§7.2 step 3, §7.3, C2, C8) The opcode-kind allowlist does not establish "can do nothing a vagent program cannot"

**Problem:** Line 360 claims verification "establishes that the graph can do nothing a vagent
program cannot". Line 370-376 then concedes the staging path is "not airtight". An allowlist of
opcode kinds says nothing about operands, and `vn_op(b, kind, inputs...)` (line 301) is fully
generic.

**Evidence:**

- `Link`, `Value`, `Point`, `Splice` and `Deref` take computed operands. A fragment can relink
  dispatcher `d.spin` edges, forge `d.outbox` entries, or `Point` a handle at any arena cell.
  `Point` checks only `arena.contains(target)` (`vortex_vm.cpp:1196-1198`).
- A `Jump`, `Branch` or `Choice` whose input is another op's computed output, not a constant cell,
  cannot be checked as "in-fragment" (line 319). `Choice` validates only that the target has an
  opcode kind (`vortex_vm.cpp:1135-1138`). An unknown kind silently runs as `Nop` (`:347`, `:1260`,
  `:1222-1224`), so a jump into a data cell is also a silent no-op, not an error.
- `Halt` and `Return` are in the compiler's emitted set. A fragment `Halt` skips `rtSettle_` and the
  turn counters that make request ids unique (`agent_executor.cpp:4746`, "the ids above are spent").
- C8 says fragments run under the same `maxCycles`. `runPipeline` executes up to 1000 opcodes per
  input cell per step and does not charge them to `totalCycles_` (`vortex_vm.cpp:334-376`, `:1268`).
  A fragment can build pipelines the compiler never does.

**Fix:** Drop the claim, or state it for what the verifier does check. Replace generic `vn_op` with
a closed vocabulary whose operand shapes are fixed: control-flow ops take only labels; `Link`,
`Value` and `Point` take only frame-slot or declared-field operands; pipelines are limited to the
`rd` shape the compiler emits. Say plainly that a fragment can still corrupt its own agent's arena
and outbox, and that the drain, the authorizer and effect-signature checks are what bound that.

### 4. (§4 N5, §5 "Load", §16) "Replay once per `programRef` per node" is not what the executor does today

**Problem:** N5 and §5 say replay is paid once per `programRef` and retained, "as
`CompiledProgram::fromSource` does". But `fromSource` produces a sealed `programStore_` and
`programManifold_` and the executor does not run from them yet.

**Evidence:**

- `AgentExecutor` calls `program->instantiate(context, agentType)` on every agent's first turn and
  on every resume (`agent_executor.cpp:1174`). `CompiledProgram::instantiate` runs
  `compileAgent(unit_, agentType)` into that agent's own arena (`program.cpp:152-170`).
  `MeshActorRuntime` already attaches the base manifold (`mesh_actor.cpp:194-196`), so the program
  is compiled twice: once into the base, once into the overlay.
- A `NativeAgentProgram::instantiate` must therefore replay the op log into the agent's overlay per
  agent and per resume, at O(ops) each. The one-off cost is priced as per-ref (line 165, line 949).

**Fix:** Say that native replay follows whatever `instantiate` contract P17 finishes. Until
P17.2/P17.3 land, price replay per agent and per resume, and say which `CompiledAgent` refs
`instantiate` returns (overlay or base). Add a gate: N agents of one native program cost one replay,
not N, once the base is wired.

### 5. (§12.2, C6, §14.2 tail) The C6 reachability check is defined against a base that the running code lacks, and is placed in the wrong file

**Problem:** C6 says the walk "enters a program cell of the shared base" (line 692). Today each
agent's program lives in its own arena, so the test has no meaning. Under P17 it is meaningful, but
the doc's cost (O(live cells + links) walk) and its placement ("new code in the adapter's checkpoint
path") are both wrong.

**Evidence:**

- The adapter calls `runtime_.checkpointView` then `publishValue` (`agent_executor.cpp:4794-4825`).
  The traversal that decides what is checkpointed is `publishValue` in `vortex/mesh_value.cpp`,
  which has no handling of base or shadowed cells (no `isEphemeral` or shadow reference there). A
  walk in the adapter would duplicate that traversal.
- Under P17, linking a state cell to a base cell calls `shadow(*to)`
  (`vagent-shared-program-and-live-upgrade.md:247-248`), which keeps the base `CellRef`
  (non-ephemeral) inside the overlay. `ArenaManifold::shadowCount()` (`arena_manifold.hpp:482-485`)
  is O(1) and `shadowOrder_` is O(shadows). A check at link time or against the shadow list is
  cheaper than a live-cell walk. I did not verify which shadows the compiler's own legitimate writes
  create.
- The doc leaves the placement open ("P20.7 at that point"). It cannot be an adapter-only change.

**Fix:** Make the C6 check a numbered proposal now (P20.7, touching `mesh_value.cpp` or
`arena_manifold.cpp`), folded into the one existing traversal or the shadow list. State that C6
depends on P17.1/P17.3. State what happens today: the check is vacuous without a base, and the gate
test would pass on a non-test.

### 6. (§4 N11, §11.4, P20.1) Binding-level `Contention::Queue` serialises every agent of the node on one library

**Problem:** N11/§11.4 bind `Curses` once in `HostBindings` with `Contention::Queue` "so the control
plane's view matches the host's", and N10 gives each agent its own host. Contention is per binding
name, not per agent.

**Evidence:** `Binding` carries one `live` list and one `queue` per registered name
(`host_bindings.cpp:50-55`); `promoteLocked` admits one live effect per binding (`:90-99`); `start`
queues or refuses against that binding (`:218-224`). Two agents of one program share the name
`Curses`, so agent B's `batch` waits for agent A's, including A's blocking calls up to its deadline.
P20.1 adds only an `agent` field; it does not touch contention.

**Fix:** Bind with `Contention::Shared` and let `RemoteNativeSkill` queue per agent host (the host
already serialises per-library threads). Or make P20.1 key contention by `(binding, agent)`. Say
which, and add a two-agent gate test.

### 7. (§2 "incarnation", §10.3, §12.2) Incarnation is an in-memory counter, so stale names can alias live ones

**Problem:** An incarnation is "a counter the control plane bumps each time it starts a native host
for an agent" (line 87), and grants and handles are `{incarnation, id}`. §12.2 says that after
`resume` "every use fails as stale". That holds only if a fresh host always gets a number the old
state cannot match.

**Evidence:**

- The counter restarts at 1 after a node restart, and on a migration target it starts from that
  node's own count. An agent's checkpointed `Grant{incarnation:1, id:3}` can equal a legitimate
  grant of the resumed agent's first host on the target.
- §12.2 says the host "may stay alive across a checkpoint". If the agent is resumed on the same node
  while that host lives, its incarnation is unchanged and its handles are valid, so the state
  diverges from the checkpoint: the screen the state "remembers" is live. §13.2 step 7 expects
  `stale_handle` after resume, and no rule in the doc causes it.
- The mesh already solved this: `reserveNodeId` reserves a durable, fsynced, flock-protected epoch
  (`vortex/mesh_epoch.hpp:17-24`). The doc names "incarnation epochs" as its model (line 69) and
  does not use them.

**Fix:** Make the incarnation a node-unique value with a durable epoch (use `reserveNodeId` or an
equivalent journal), not a counter. Specify that `checkpoint` of a native agent retires its
incarnation (revoke grants, kill or fence the host), and say what `resume` on the same node does
with a surviving host.

### 8. (§10.3, §12.1, §12.3) Nothing tells the supervisor when an agent stops, expires or migrates

**Problem:** The host and its grants are keyed by agent. N1 says the agent is "stopped, renewed,
monitored, checkpointed" like any other, and §10.4 lists revocation "by lease end or stop". No
component is given the signal.

**Evidence:** `AgentExecutor` exposes `TurnObserver` and `IntentionObserver` only
(`agent_executor.hpp:347-351`, `:410-413`); there is no stop, lease-end or checkpoint hook. `stop`
cancels flights (`agent_executor.cpp:3902-3920`) but not host-side resources. After a stop, a lease
expiry or a checkpoint-and-migrate, the host process, its pty pair and its grant descriptors leak,
and the broker table keeps entries for a dead agent.

**Fix:** Add a numbered proposal (P20.x) for a lifecycle observer (`stopped`, `lease ended`,
`checkpointed`) and state the supervisor's reaction to each. Or have the supervisor poll
`live(state)`, and say so and price it.

### 9. (§1 item 5, §5, §15) "Leverages Vortex's multiprocess support" is met by analogy only, and the cheapest alternative is not refused

**Problem:** The owner asked to leverage Vortex's multiprocess support to isolate native code. §1.5
says the supervisor works "in the manner of" `MeshHost`, `mesh_cli` and incarnation epochs, and that
it "does not ride the BitTorrent transport". What is actually reused is bencode conventions and
`WireValue`. The supervisor, rings, handshake, incarnations, death handling and restarts are all
new.

**Evidence:**

- Vortex's multiprocess lifecycle (`design/vortex-distribution.md:70-76`) is `MeshHost` with
  `--mesh-worker` subprocesses over `MeshHostTransport` (`vortex/mesh_host.hpp:13-44`), an abstract
  interface (`send`, `takeActorMessages`, `announce`). The native host reuses none of it.
- §15 does not refuse "native host as a mesh peer over a socketpair `MeshHostTransport`, effects as
  `ActorMessage`s". The grounds that would support refusing it (no network, no key in the sandbox)
  are in §1.5 but are not argued against the transport interface being swappable.

**Fix:** Either say plainly in §1 and §16 that Vortex has no OS-process isolation facility to
leverage, so this design builds a sibling that borrows its conventions (the owner then decides), or
implement the host side as a `MeshHostTransport` peer and reuse the actor codec. Add the
refused-alternative row with its price.

### 10. (§4 N6, §7.1) `programRef` construction is ambiguous, and the module digest is not bound to the module that runs

**Problem:** N6 says the ref is `sha256(fmt ‖ 0 ‖ opLog ‖ 0 ‖ manifest ‖ 0 ‖ sha256(module))`, "the
same construction as `refOf(source)`".

**Evidence:**

- `program.cpp:14-27` relies on a separator that is "a byte no source file may contain". Bencoded op
  logs and manifests can contain NUL (strings are length-prefixed), so `(opLog, manifest)`
  concatenations are ambiguous. It is not "the same construction".
- The seal produces `module_digest` (line 331), but §8.1 loads the module by path under Landlock and
  §12.3 says it travels "from a local path". Nothing requires that the bytes loaded at serve time
  hash to the digest in the ref, so a swapped file runs under an approval granted per `programRef`
  (§10.2).

**Fix:** Hash length-prefixed parts, or
`sha256(fmt) ‖ sha256(opLog) ‖ sha256(manifest) ‖ sha256(module)`. Have the control plane open the
module, check the digest, and pass an fd (sealed `memfd` or checked descriptor) to the host, so the
digest and the code that runs are the same bytes.

### 11. (§6.1-6.3, §13.1) The "whole ABI" is not specified, and the first test does not exercise the fragment path it claims to

**Problem:** §6.1 says `vagent_native.h` is "the whole ABI a module sees", but §6.2 lists only
declaration ops and §6.3 only statement framing. The example in §13.1 uses `vn->local`, `vn->str`,
`vn->field`, `vn->index`, `vn->steps`, `vn->body_adopt`, `vn->ffi_skill`, `vn->authority`,
`VN_RECORD`, `VN_ARGS`, `VN_DISCARD` and `VN_NONE`, none of which is defined. `vn_node` is "an
opcode cell in a fragment" (line 264) but is used as a local and as an expression value.

**Evidence:** `Grant` is `{kind, id, incarnation}` (line 512), but the example calls `t.scope` and
§10.4 says "the grant record's `scope` carries the terminal type". The example never builds a raw
fragment, yet the gates C6 and C12 (lines 823-826) need one that links a state cell and one that
stages a malformed outbox entry.

**Fix:** Specify the expression, type and local vocabulary, and the `vn_*` signatures, or defer them
to a numbered ABI document. Reconcile `Grant.scope`. Add a raw-fragment case to the first test, or
say that the raw path is untested by it.

## MINOR

### 12. (§11.6) Readiness percepts are level-triggered with no arming rule

Readable is posted whenever the library thread is idle and the descriptor is readable (line
656-659). Until the agent's `getch` effect runs, every idle poll posts another `Readable` as a
percept, each a wake turn. `AgentExecutor::percept` only pushes to the post box
(`agent_executor.cpp:3671-3679`); I saw no cap on it. Specify edge-triggered arming: one `Readable`
per transition, re-armed when a read effect completes.

### 13. (§12.2) The "no effect in flight in the host" quiescence condition is already enforced

`checkpoint` already refuses `EffectInFlight` while `state.flights` is non-empty
(`agent_executor.cpp:4789-4791`), and a `RemoteNativeSkill` effect is a flight. State that no new
check is needed, and that the live `Percept` queue is covered by `HostDeliveryPending`.

### 14. (§7.2 step 3) Cite the VM line for the no-op claim

"The VM treats an unrecognised opcode cell as a no-op (`vortex_vm.cpp`, per the runtime grounding)"
is true, but cite `vortex_vm.cpp:347`, `:1222-1224`, `:1260`. Add that `Choice` rejects a target
with no opcode kind (`:1135-1138`), while `Jump` and `Branch` do not check, so the no-op is silent
there.

VERDICT: NOT CONVERGED
