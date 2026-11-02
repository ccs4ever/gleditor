# vagent native agents: sandboxed native code behind the agent contract

| field         | value                                                                                                                                                                                                                                                |
| ------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| version       | 0.6 (see [change history](#change-history))                                                                                                                                                                                                          |
| status        | Proposal, review converged ([record](projects/vagent-native-agent-review/README.md)); P20.1–P20.8 approved by owner (2026-10-31). Nothing is built                                                                                                   |
| prerequisites | Q1–Q2 (§1.1): effect and native-skill lowering in the actor compiler, owed by the vagent implementation plan's milestone 6. §13 cannot run before they land                                                                                          |
| companion to  | [vagent v3.13](vagent-agent-language.md) §§4–7, 10, 11; [runtime grounding](vagent-runtime-grounding.md); [shared program and live upgrade](vagent-shared-program-and-live-upgrade.md) (P17); [cluster packaging](vagent-cluster-packaging.md) (P21) |
| rests on      | the actor front end (`vagent/runtime/ast.hpp`, `checker`, `compiler`, `program`), `HostBindings` (`vagent/runtime/host_bindings.hpp`), `AgentExecutor`, `MeshActorRuntime`, `reserveNodeId`                                                          |
| first test    | an agent built by native code draws with ncurses through libffi on a terminal it had to request, headless, under a sandbox that refuses every other write                                                                                            |

A **native agent** is a vagent agent whose program is built by native code instead of being written
as vagent source, and whose native capacities are carried out by native code it ships itself. Its
program is built through recorded **native ops** that construct the actor compiler's typed program
tree. The node checks and compiles that tree with the same checker and compiler as source, so the
Vortex graph that runs is produced exactly as a pure vagent agent's is. To every peer, to the mesh
runtime and to the adapter, it is an agent like any other. Its native code never runs in the node
process. It runs in a separate, sandboxed **native host process** that can read its libraries, write
nothing on disk, reach no network, and talk only to the node over shared memory, one socket and a
log pipe. Anything more, such as the right to write to a terminal, it requests at run time, and the
owner's policy grants it as a descriptor, never as a path.

Paths below are under `apps/common/xanadu/` unless stated.

______________________________________________________________________

## Contents

- [1. The request, as this document reads it](#1-the-request-as-this-document-reads-it)
- [2. Glossary](#2-glossary)
- [3. The agent contract](#3-the-agent-contract)
- [4. Rulings](#4-rulings)
- [5. Architecture](#5-architecture)
- [6. Native ops: building the program](#6-native-ops-building-the-program)
- [7. Sealing, loading and `programRef`](#7-sealing-loading-and-programref)
- [8. The native host process and its sandbox](#8-the-native-host-process-and-its-sandbox)
- [9. IPC with the control plane](#9-ipc-with-the-control-plane)
- [10. Authorities](#10-authorities)
- [11. Foreign calls through libffi](#11-foreign-calls-through-libffi)
- [12. Lifecycle, failure, checkpoint and migration](#12-lifecycle-failure-checkpoint-and-migration)
- [13. The first test: ncurses through libffi](#13-the-first-test-ncurses-through-libffi)
- [14. New code and proposed changes](#14-new-code-and-proposed-changes)
- [15. Refused alternatives](#15-refused-alternatives)
- [16. Properties and costs](#16-properties-and-costs)
- [17. Open questions](#17-open-questions)
- [Change history](#change-history)

______________________________________________________________________

## 1. The request, as this document reads it

The request has five parts. Each is read here as follows, so that a reader who meant something else
can see where the readings part.

1. **"Fulfilling the agent contract just as a pure Vortex agent would."** The contract is the set of
   obligations §3 lists from the vagent specification. A native agent meets every one of them by the
   mechanisms a compiled agent meets them by. No obligation rests on native code behaving.
1. **"Constructing its Vortex graph with native ops."** *Native ops* are construction calls a native
   module makes through a versioned C ABI (§6). There is one node kind for each struct of the actor
   front end's program tree, `vagent/runtime/ast.hpp`, plus kinds for the native bindings. The ops
   are recorded, the node replays them into a `Unit`, and the existing checker and compiler lower it
   to the Vortex graph. The native code decides every declaration, statement and expression. The
   compiler decides the opcodes. Ops that place individual opcodes, a second tier, are deferred
   (NQ1) because §15 shows that the compiler has no seam for them and no verifier yet makes them
   safe.
1. **"Capable of accessing native libraries."** Through native capacities whose methods are effects
   (V6), carried out in the native host process by a generic libffi runner (§11) or by the module's
   own native skills.
1. **"Blocked from most writes, by likely platform-specific mechanisms, except for shared memory and
   whatever IPC needs."** The native host runs under Landlock and seccomp-bpf on Linux, and under
   the platform's equivalent elsewhere (§8). It may write its own memory, two shared rings, its
   socket, two eventfds and a log pipe, plus any descriptor a grant hands it.
1. **"Leverages Vortex's multiprocess support."** Vortex's multiprocess support is *distribution*:
   `MeshHost` workers run as separate processes (`vpl --mesh-worker`, `design/vortex-mesh-cli.md`),
   reach each other over an authenticated transport, and reserve durable incarnations with
   `reserveNodeId` (`vortex/mesh_epoch.hpp:17-24`). Vortex has no facility for *isolating* a child
   process. This design therefore reuses what fits. Native-host incarnations derive from the node's
   durable epoch (§12.2). Frames follow the mesh's bounded, named-error codec conventions, and the
   values inside them are the actor wire's `WireValue`s. It builds the isolation half new. §15
   refuses running the native host as a mesh peer over `MeshHostTransport`, with the reason. The
   owner may read this as meeting the request only in part, and §17 (NQ7) asks the question.

### 1.1 Prerequisites in the vagent implementation

A native program is compiled exactly as source is (N1). That inherits the actor compiler's present
limits, and two of them block every native capacity until they are lifted. Both are features the
vagent specification already defines and the [implementation plan](vagent-implementation.md) owes
under milestone 6 ("Effects and composition", open). They are therefore prerequisites, not proposals
of this document:

- **Q1. Effect calls and native skills lower.** Today the compiler refuses a native skill
  (`vagent/runtime/compiler.cpp:6761-6763`) and an effect method (`:6102`, `:6822`), and
  `tests/xuzz/vagent_skill_test.cpp` (`ANativeSkillIsRefusedByName`) asserts the refusal.
  `Gen::effectCall` (`:3827`) already lowers a plan's effect call as a suspension point with its
  step entry and `Waiting` record (`:3870-3900`), but it has no caller and writes no
  `kOutboxEffectCapacity` or `kOutboxEffectDeadline`. The drain counts and skips an entry that lacks
  either (`agent_executor.cpp:2379-2392`). Milestone 6 must admit a native skill as a bodiless
  container, route `capacityCall` on an effect method to `effectCall`, write capacity and deadline
  (from `MethodSig::withinMs`, leaving `kOutboxEffectPreempts` unset), and decode record results.
- **Q2. Effect recovery.** The specification's statement-level `else` on an effect call (§6.6 of the
  specification) has no AST slot: `ECall` carries no `orElse` (`ast.hpp:138-141`), and the parser
  reads `else` only after `achieve`, `ask` and `await`. This design does not depend on it: grants
  are released when their intention ends (§10.1), and N13 stays after Q2 lands as defence for
  non-local exits. Once Q2 lands, native modules gain the slot through the ABI like any other
  (§6.2).

**Gate for Q1, owned by the vagent plan as Q1's acceptance test:** the §13.1 program compiles, its
first turn stages an outbox entry that the drain accepts with capacity and deadline present, a
record result is decoded, the plan resumes at its step path, and `ANativeSkillIsRefusedByName` is
replaced in the same change.

## 2. Glossary

| Term                | Meaning                                                                                                                                                    |
| ------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| control plane       | The node process: `MeshActorRuntime`, `AgentExecutor`, the capability issuer, the broker. It holds every key and every arena                               |
| native module       | A shared object exporting `vagent_native_api_version` and `vagent_native_build`, and optionally native skills                                              |
| native ops          | The `vn_api` calls of §6, recorded into an op log                                                                                                          |
| op log              | The bencoded, ordered record of native ops a build produced. Replayed, it is a `Unit` plus a binding manifest                                              |
| binding manifest    | The binding-kind nodes of the op log (§6.2): FFI libraries, C signatures, sequences, handle types, authorities. Read by the host's runner                  |
| native host process | `vagent-native-host`: one sandboxed process per native agent that loads the module and the FFI libraries and carries out effects                           |
| sandbox             | The platform restrictions applied to the native host before any module or library code runs                                                                |
| incarnation         | `(node, serial)`: the node's `author:device:epoch` name and a per-node counter; names one native host for one agent pid                                    |
| authority           | A named kind of access beyond the sandbox, such as `terminal`; declared by the module, requested at run time, granted by policy                            |
| grant               | One authority granted to one intention (or, once held, to the agent): an opaque token in agent state, a descriptor in the native host, a row in the broker |
| handle              | A native pointer in the native host's handle table, seen by the agent only as an opaque token inside a typed record                                        |
| broker              | The control-plane component that decides requests, creates resources (a pty pair for `terminal`), records decisions and revokes                            |
| authority ledger    | A private store, with its own private permascroll, where the broker records approvals, decisions and revocations as cells (§10.5)                          |
| sequence            | A typed foreign-call effect declared by the module: named steps over its parameters, run on one library thread, with `finally` cleanup (§11.4)             |

## 3. The agent contract

The obligations are taken from the vagent specification. The right-hand column says what enforces
each one for a native agent. "Compiler" means the existing checker and compiler, which a native
program passes through exactly as source does (N1), once Q1 has landed.

| No. | Obligation                                                                               | Source       | Enforced for a native agent by                                                                                                                                |
| --- | ---------------------------------------------------------------------------------------- | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| C1  | One agent is one actor with one private arena and one cursor; single owner               | V1, V3, §4.1 | Unchanged: the arena lives in the control plane (N2), created by `MeshActorRuntime::spawn`                                                                    |
| C2  | A turn is atomic; nothing leaves the arena mid-turn                                      | V5, §4.2     | Unchanged adapter steps; compiler-emitted graph with no host-reaching opcode (N3)                                                                             |
| C3  | Device actions are asynchronous effects that suspend; every external wait has a deadline | V6, §6.6     | Checker (every synthesized native method is an `effect def … within`, §10.1, §11.2) and compiler (Q1); foreign calls exist only as effects                    |
| C4  | Speculation is bounded by the statement and the turn                                     | V2, V13      | Compiler (statement choice points); adapter `clearChoicePoints` and `discard`/`release`                                                                       |
| C5  | Intentions persist as data and resume at a step path                                     | V4, §6.6     | Checker (suspension positions) and compiler (step paths, Q1)                                                                                                  |
| C6  | No state cell links into the program                                                     | V12          | Compiler; no op places a link                                                                                                                                 |
| C7  | Agents are named; request ids, floors and redelivery follow §5.5 and §7.3                | V16, §7.3    | Unchanged adapter; native code never sees an envelope                                                                                                         |
| C8  | Each turn is bounded by the grant's cells, work, cycles and scratch bytes                | §4.2         | Unchanged                                                                                                                                                     |
| C9  | Genesis, the agent dimensions and `Initialize` run once                                  | V10, §4.1    | Compiler and adapter. The specification's V10 lists thirteen dimensions; `kAgentDimensionNames` registers fifteen today (`vagent/runtime/runtime_abi.hpp:70`) |
| C10 | Checkpointable at quiescence; nothing process-local in the checkpoint                    | §4.2, §4.4   | Unchanged: `checkpoint` refuses `EffectInFlight` and pending host deliveries (`agent_executor.cpp:4789-4791`). Tokens in state go stale on resume (§12.3)     |
| C11 | `answer` bodies are read-only                                                            | §7.1         | Checker purity rules                                                                                                                                          |
| C12 | Hard failures are classified and recovered; a failed turn drains nothing                 | §4.2         | Unchanged adapter                                                                                                                                             |

One observation from verifying C12 is not specific to native agents. The drain counts and skips an
outbox entry it cannot decode (`bumpStat(state, kStatMalformed); continue`,
`agent_executor.cpp:2246-2252`, `:2379-2386`). It does so after the turn has committed, so a
malformed entry is not a hard failure. A correct compiler produces none, and Q1's gate checks that
the effect entries it produces decode.

## 4. Rulings

Each ruling states what it decides, why, and what it costs. Refused alternatives are in §15.

**N1. A native program is a `CompiledProgram`.** The op log replays into a `Unit`, which P20.2's
`CompiledProgram::fromUnit` checks and compiles, exactly as `fromSource` does after parsing
(`vagent/runtime/program.cpp:31-110`). After the parse, `fromSource` reads only the `Unit`. Its text
is used for the diagnostic prefix and `refOf`. Launch, `LocalHost`, children, checkpoint, resume,
live upgrade (`AgentExecutor::upgrade` takes a `CompiledProgram`; the supervisor's part is §12.4)
and P19 arbitration (which `dynamic_cast`s to `CompiledProgram`, `agent_executor.cpp:3271`) then
work as for any compiled program. *Price:* one factoring of `fromSource` (P20.2), the compiler's
present limits (§1.1), and per-agent costs that are exactly a compiled program's. Today
`instantiate` recompiles the `Unit` for each agent and each resume (`program.cpp:148-160`); P17's
shared base is what changes that, for both kinds of program.

**N2. Turns run in the control plane; only native code runs in the sandbox.** The arena, cursor and
VM stay in the node process. The native host holds no arena and runs no turn. It carries out
effects, runs readiness watchers and holds native handles and descriptors. *Why:* a crashed or
compromised native host can do no worse than a misbehaving device. It can return wrong results,
which are typed `WireValue`s the control plane decodes and checks, or return nothing, which a
deadline catches. It cannot corrupt agent state, forge outbox entries or stall another agent.
*Price:* every native call is an effect, paying V6's turn per effect and an IPC round trip (§16).
Sequences (§11.4) are the mitigation.

**N3. No new opcode reaches native code.** The runtime grounding's conclusion stands: a VM
native-call hook would be a separately priced core proposal with its own security contract, and this
design makes none. *Price:* even a pure native function costs a turn. In return, no native code runs
inside a turn, so a failed turn never leaves the world changed, and single-turn determinism (§14 of
the specification) is unchanged.

**N4. Native ops construct the program tree, and the compiler constructs the graph.** There is one
node kind per struct of `ast.hpp`, and one slot per field (§6.2). The program the native code
describes is therefore anything vagent source can describe, with nothing more and nothing less.
*Why:* the compiler's emitter is a single AST-driven class, `Gen`, private to `compiler.cpp`'s
anonymous namespace (`vagent/runtime/compiler.cpp:92`, `:411`), and it derives the plan step-entry
and frame model from the tree. A lower-level op set would need that class refactored and a verifier
nobody has specified (§15). At this level, C3–C6, C9 and C11 are the checker's and compiler's job,
as they are for source. *Price:* a native module cannot express what vagent source cannot. That is
the contract, and raw graph construction is deferred (NQ1).

**N5. The op log is replayed by the control plane, not compiled by the sandbox.** The native host
records ops and returns the log. It builds no `Store`, holds no permascroll and no key, and could
not write a permascroll if it had one. The control plane replays, checks and compiles. String
constants go wherever `fromSource`'s promotion puts them today. The spans are not part of
`programRef` (N6), so a node-local placement does not change program identity.

**N6. `programRef` covers the compiler, the op log and the module, not the system's libraries.** It
is the SHA-256 of four length-prefixed parts: the actor compiler's `kProgramFormat`
(`vagent/runtime/program.hpp:36-39`, so a compiler change moves the ref as it does for source), the
format tag `vagent-native/1`, the canonical op-log bytes (the binding manifest is part of the log),
and the SHA-256 of the module bytes the control plane opened (§7.3). Length prefixes, not
separators, because bencoded parts may contain any byte (unlike source text, `program.cpp:18-27`).
The bytes of `libncursesw.so.6` are not included: system libraries differ per machine, and including
them would name a different program on every node. *Price:* two nodes running one `programRef` can
load different builds of a library, and the soname's major version is the only ABI guard. The
resolved library digests are logged per incarnation (§8.4).

**N7. Default refuse, with no unsandboxed fallback.** Every supported platform (Linux, FreeBSD,
OpenBSD, macOS and Windows) has a specified sandbox and IPC backend (§8.2, §9.1, §10.4). A node
whose build disabled native agents, or whose kernel lacks the platform's required mechanism (Linux:
Landlock ABI 4, Linux 6.7; FreeBSD: Capsicum and `shm_open2`; OpenBSD: `unveil` and `pledge`; macOS:
Seatbelt; Windows: AppContainer and ConPTY), refuses to launch a native agent with `NoSandbox` and
names the missing mechanism. *Price:* such nodes cannot host native agents, and a migration to one
is refused.

**N8. Authorities are descriptors or handles handed over, never paths opened up.** The sandbox never
widens after lockdown. A granted authority arrives as a descriptor over `SCM_RIGHTS` on POSIX (or a
handle duplicated with `DuplicateHandle` on Windows), created by the broker and acknowledged before
the granting effect completes (§9.1). Path-based access checks happen at open time, so a descriptor
or handle the unsandboxed broker created remains usable, while the host can still open nothing new.
*Price:* only authorities expressible as a descriptor or handle are offered (NQ3).

**N9. The terminal authority is a brokered pseudo-terminal, never the owner's terminal.** The broker
creates a pty pair (`posix_openpt` on POSIX, `CreatePseudoConsole` ConPTY on Windows), keeps the
master and passes the slave. *Why:* revocation (close the master), sizing, headless testing,
restoration of the owner's terminal (§10.4), and safety. A descriptor for the owner's real terminal
would allow `TIOCSTI` input injection into the owner's shell, line-discipline changes, and writes
after revocation. *Price:* to show output to the owner, the broker relays and filters through a
closed CSI allowlist (§10.4), one copy per byte.

**N10. One native host per agent pid.** Each native agent has its own process, handle table and
grants. *Why:* an ncurses fault takes down one agent's native host and no other, and grants never
leak between agents. *Price:* a process per native agent, bounded by `vagent.native.max_hosts`.

**N11. Effects are routed per agent, and contention lives in the host.** Every native capacity is
bound in the node's `HostBindings` to one `RemoteNativeSkill` with `Contention::Shared`, which
forwards to the requesting agent's native host. Serialisation for a `one_thread` library happens on
that host's library thread. *Why:* `HostBindings` keeps one `live` list and one `queue` per skill
name for the whole registry (`host_bindings.cpp:50-56`, `:195-225`). Binding with `Queue` would
serialise every agent's ncurses calls behind each other, across processes that share nothing.
`EffectRequest` names no agent today, and P20.1 adds the agent pid, its name and the staging
intention so the binding can route. One registry entry per skill name suffices even when two
programs both declare `CursesFfi`, because each agent's effects reach the host that loaded that
agent's own module. *Price:* one field and one argument in existing code, and preemption across a
library is the host's job (§11.5).

**N12. Nothing the native host reports is trusted beyond its type.** Ring A (`shm_a`) is mapped
read-only in the host so the host cannot corrupt control-plane writes, and frames from Ring B
(`shm_b`) are copied out of shared memory with atomic byte loads before they are decoded (§9.1,
§9.3). A result must decode to the method's declared result type, or the effect fails
`native_protocol` and the host is killed. *Price:* two shared-memory objects per host, one copy per
frame, and a kill-and-restart path to test.

**N13. A grant belongs to the intention that requested it, unless the agent holds it.** By default a
grant is released, with cause `intention_ended`, when the top-level intention that staged
`open_terminal` ends, by success, failure or drop. A staging subgoal's grant belongs to its
top-level ancestor, which P20.1 finds by walking the `d.subgoal` chain negward when the effect is
staged. `hold_terminal(t)` moves a grant to agent scope. A held grant lasts until `close_terminal`,
revocation by the owner, or the agent's end, and it is what an agent keeps in a field for later
intentions (§11.6, §13.1). *Why:* a plan that fails between `open_terminal` and `close_terminal`
would otherwise keep a terminal until the agent stops. Statement-level `else` on an effect is not
yet in the front end (Q2), and a token held only in a failed plan's frame is unreachable by any
later plan. Holding is explicit, so a terminal outlives its intention only when the program says so.
*Price:* P20.6's `intentionEnded` signal, and one more effect method.

## 5. Architecture

```text
 control plane (node process)                                         native host process (sandboxed)
 ┌──────────────────────────────────────────────────────┐            ┌──────────────────────────────────┐
 │ MeshActorRuntime ─ AgentExecutor ─ turns over arenas  │            │ vagent-native-host               │
 │      │         outbox drain → HostBindings            │            │   recorder for vn_api (seal mode)│
 │      │             └ RemoteNativeSkill (Shared) ──────┼─ shm_a ───►│   loader: module, FFI libraries  │
 │      │                                                │  (RO host) │   per-library threads, libffi    │
 │ CompiledProgram::fromUnit ◄─ op log replay ◄──────────┼◄─ shm_b ───┤   sequences, handle table        │
 │ NativeHostSupervisor: stage, spawn, watch, kill       │  (RW host) │   readiness watchers             │
 │ Broker: policy, ledger, pty/ConPTY, relay, revoke ────┼─ ctrl ch ─►│   grant table (sync GrantAck)    │
 │ I/O thread: copy, decode, post completions/percepts  │◄─ log pipe ┤ stderr (O_NONBLOCK)              │
 └──────────────────────────────────────────────────────┘            └──────────────────────────────────┘
```

Three phases use these parts.

- **Seal**, once per module version and usually at build time.
  `vagent native seal <module.so> [-o <out.vnprog>]` (P20.8) runs `vagent-native-host --seal` with
  no authorities, which calls `vagent_native_build` twice. The two op logs must be byte-identical,
  or the seal fails `NondeterministicBuild`. The control plane replays and checks the log, so a
  module that builds an ill-typed program fails at seal with the checker's diagnostics. It writes a
  `.vnprog` file recording `api_version`, `op_log`, `module_digest` and `module_filename` (§7.1).
- **Load**, per node. The control plane decodes the `.vnprog`, locates and hashes the module file
  (§7.3), replays the log into a `Unit`, calls `CompiledProgram::fromUnit`, and retains the program
  with `retainProgram`.
- **Run**, per agent. `launch` spawns the actor. At its first native effect (or at launch, with
  `vagent.native.start = eager`), the supervisor starts its native host. Turns run in the control
  plane. Native effects go to the host over Ring A (`shm_a`, mapped read-only in the host), and
  completions, Ring A's consumer cursor and percepts come back over Ring B (`shm_b`).

### 5.1 Building and running a native agent: five steps from source to deployment

A developer creating a native agent from scratch follows five steps:

1. **Write the module (`greeter.cpp`).** Include `<vagent/vagent_native.hpp>` and export
   `vagent_native_api_version` and `vagent_native_build`:

   ```cpp
   #include <vagent/vagent_native.hpp>

   extern "C" uint32_t vagent_native_api_version(void) { return VN_API_VERSION; }

   extern "C" int vagent_native_build(const vn_api *api, vn_builder *raw) {
     using namespace vagent::native;
     Builder b(api, raw);

     b.authority("terminal", "draw a greeting on a dedicated terminal");

     auto curses = b.library("ncurses", "libncursesw.so.6", Affinity::OneThread,
                             "Curses", "CursesFfi");
     curses.handle("Screen", "SCREEN", "delscreen");
     curses.handle("Stream", "FILE", "stdio.close");
     curses.fn("newterm", {{"type", "cstring_in"}, {"outf", "Stream"}, {"inf", "Stream"}},
               "Screen", FailIf::Null());
     curses.fn("delscreen", {{"sp", "Screen", ParamMod::Consumed}}, "void");
     curses.fn("mvaddstr", {{"y", "c_int"}, {"x", "c_int"}, {"s", "cstring_in"}},
               "c_int", FailIf::Eq(-1));
     curses.fn("refresh", {}, "c_int", FailIf::Eq(-1));
     curses.fn("mvinnstr",
               {{"y", "c_int"}, {"x", "c_int"}, {"s", "cstring_out(n)"}, {"n", "c_int"}},
               "c_int", FailIf::Eq(-1));
     curses.fn("endwin", {}, "c_int", FailIf::Eq(-1));

     auto seq = curses.sequence("draw_greeting",
                                {{"t", "Terminal"}, {"y", "Int"}, {"x", "Int"}, {"text", "Str"}},
                                "Str", /*withinMs=*/5000);
     seq.let("outf", seq.call("stdio.stream", {seq.arg("t"), seq.lit("w")}));
     seq.let("inf", seq.call("stdio.stream", {seq.arg("t"), seq.lit("r")}));
     seq.let("sp", seq.call("newterm", {seq.field("t", "term"), seq.arg("outf"), seq.arg("inf")}));
     seq.step(seq.call("mvaddstr", {seq.arg("y"), seq.arg("x"), seq.arg("text")}));
     seq.step(seq.call("refresh", {}));
     seq.let("back", seq.call("mvinnstr", {seq.arg("y"), seq.arg("x"), seq.len("text")}));
     seq.result(seq.field("back", "s"));
     seq.finally_call(seq.call("endwin", {}));
     seq.finally_call(seq.call("delscreen", {seq.arg("sp")}));
     seq.finally_call(seq.call("stdio.close", {seq.arg("inf")}));
     seq.finally_call(seq.call("stdio.close", {seq.arg("outf")}));

     b.event("Greeted", {{"text", "Str"}});
     b.goal("Greet", GoalMode::Perform);

     auto ag = b.agent("Greeter");
     ag.uses({"Terminals", "Curses"});
     ag.install("HostTerminals");
     ag.install("CursesFfi");
     ag.on("Initialize").adopt("Greet");

     auto plan = ag.plan("greet").forGoal("Greet");
     plan.let("t", b.call("open_terminal", {b.lit(24), b.lit(80)}));
     plan.let("line", b.call("draw_greeting",
                             {b.name("t"), b.lit(2), b.lit(4),
                              b.lit("hello from a native agent")}));
     plan.stmt(b.emit(b.record("Greeted", {{"text", b.name("line")}})));
     plan.stmt(b.call("close_terminal", {b.name("t")}));

     return b.finish();
   }
   ```

1. **Compile the shared object:**

   ```sh
   c++ -std=c++23 -shared -fPIC -fvisibility=hidden -O2 \
     $(pkg-config --cflags vagent-native) greeter.cpp -o greeter.so
   ```

1. **Seal the program into a `.vnprog` artifact:**

   ```sh
   vagent native seal greeter.so -o greeter.vnprog
   ```

1. **Approve the `terminal` authority (or leave policy at `ask` for an interactive prompt):**

   ```sh
   vagent native approve greeter.vnprog --kind terminal --ledger ./authorities
   ```

1. **Run the native agent:**

   ```sh
   vagent run greeter.vnprog --grant-policy approved --ledger ./authorities
   ```

## 6. Native ops: building the program

### 6.1 The ABI

A native module exports `vagent_native_api_version` and `vagent_native_build`, and optionally
`vagent_native_skills` (§11.1). `vagent_native.h` is the whole ABI. It is plain C with no variadic
function, so a Rust or Zig module can bind it directly. Its `VN_KIND_*`, `VN_SLOT_*` and enum-value
constants are generated from `node_kinds.def` (§6.2) by the build, so module authors never write raw
numbers.

```c
/* vagent_native.h (hand-written part; the generated part defines VN_KIND_*, VN_SLOT_*, VN_ENUM_*) */
#include <stddef.h>
#include <stdint.h>

#define VN_API_VERSION 1u

typedef struct vn_builder vn_builder;            /* opaque recorder */
typedef struct { uint32_t id; } vn_node;         /* id 0 is never a node */

typedef enum vn_status {
  VN_OK = 0,
  VN_E_KIND,        /* unknown kind */
  VN_E_SLOT,        /* the kind has no such slot */
  VN_E_CLASS,       /* wrong value class for the slot */
  VN_E_CHILD_KIND,  /* the child's kind is not allowed in this slot */
  VN_E_ID,          /* unknown node id */
  VN_E_REUSED,      /* the node is already a child somewhere */
  VN_E_SET_TWICE,   /* a single-valued slot was already set */
  VN_E_LIMIT,       /* a seal limit was reached */
  VN_E_AFTER_ERROR  /* an earlier call failed; nothing more is recorded */
} vn_status;

typedef struct vn_api {
  uint32_t version;                              /* the host's ABI version */
  uint32_t size;                                 /* sizeof(vn_api) on the host */
  vn_node (*node)(vn_builder *, uint32_t kind);  /* {0} on failure; see error() */
  vn_status (*set_str)(vn_builder *, vn_node, uint32_t slot, const char *, size_t);
  vn_status (*set_int)(vn_builder *, vn_node, uint32_t slot, int64_t);
  vn_status (*set_float)(vn_builder *, vn_node, uint32_t slot, double);
  vn_status (*set_bool)(vn_builder *, vn_node, uint32_t slot, int);
  vn_status (*set_child)(vn_builder *, vn_node, uint32_t slot, vn_node);
  vn_status (*append)(vn_builder *, vn_node, uint32_t slot, vn_node);
  vn_status (*append_str)(vn_builder *, vn_node, uint32_t slot, const char *, size_t);
  vn_status (*set_module)(vn_builder *, vn_node module_decl);
  vn_status (*add_import)(vn_builder *, vn_node import_decl);
  vn_status (*add_decl)(vn_builder *, vn_node top_decl);
  const char *(*error)(const vn_builder *);      /* the first error, or NULL */
  /* version 1 ends here; later versions only append members */
} vn_api;

typedef struct vn_effect_req {
  uint64_t token;           /* effect correlation token */
  const char *skill;        /* declared native skill name */
  const char *method;       /* declared effect method name */
  const uint8_t *args_wire; /* bencoded WireRecord of arguments */
  size_t args_len;
} vn_effect_req;

/* ok != 0: payload is bencoded result WireValue; ok == 0: payload is UTF-8 failure cause */
typedef void (*vn_complete_fn)(void *host_ctx, uint64_t token, int ok,
                               const uint8_t *payload, size_t len);

typedef struct vn_skill_entry {
  const char *skill_name;
  void (*start)(const vn_effect_req *req, vn_complete_fn complete, void *host_ctx);
  void (*cancel)(uint64_t token);
} vn_skill_entry;

uint32_t vagent_native_api_version(void);        /* returns VN_API_VERSION */
int vagent_native_build(const vn_api *api, vn_builder *b);  /* 0 on success */
const vn_skill_entry *vagent_native_skills(void); /* optional; {NULL, NULL, NULL}-terminated */
```

The host refuses a module whose `vagent_native_api_version` exceeds its own. A module checks
`api->size` before calling a member added after the version it was built for. After the first
failure the builder records nothing more, every later call returns `VN_E_AFTER_ERROR`, and `error`
returns the message with the failing op's ordinal. A module that ignores a status and returns 0
still fails the seal, because the host checks `error` itself. The ABI is a wire boundary, and id 0
is its one reserved value. Everything on the control-plane side is `std::optional`/`std::expected`.

### 6.2 Node kinds and slots

`node_kinds.def` is one X-macro table that both the host's recorder and the control plane's replayer
include. It lists every struct of `vagent/runtime/ast.hpp` as a kind, and for each field one slot
with its class:

| Class        | `ast.hpp` field shape                                 | Set by                     | Rule                                                                                         |
| ------------ | ----------------------------------------------------- | -------------------------- | -------------------------------------------------------------------------------------------- |
| `str`        | `std::string`                                         | `set_str`                  | Once                                                                                         |
| `int`        | integer fields                                        | `set_int`                  | Once                                                                                         |
| `float`      | `double`                                              | `set_float`                | Once                                                                                         |
| `bool`       | `bool`, including `hasParens`, `hasParams`, `hasBody` | `set_bool`                 | Once; unset is `false`                                                                       |
| `enum`       | a nested `enum class`                                 | `set_int` with `VN_ENUM_*` | Once; out-of-range values fail `VN_E_CLASS`                                                  |
| `opt-*`      | `std::optional<…>`                                    | the setter of its class    | Unset is absent; set once is present                                                         |
| `child`      | `ExprPtr`, a nested struct, or a variant              | `set_child`                | Once; the table lists the kinds allowed (a variant slot allows its alternatives)             |
| `child-list` | `std::vector<…>` of nodes                             | `append`                   | In order; allowed kinds as for `child`                                                       |
| `str-list`   | `std::vector<std::string>`                            | `append_str`               | In order (`UsesDecl::capacities`, `SkillDecl::implements`, `ImportDecl::path`, and the rest) |

Every struct is a kind, including `PolicyArg` and `ArbitrationClause` (P19). `Unit::module` and
`Unit::imports` are set with `set_module` and `add_import`, and every `TopDecl` alternative with
`add_decl`. A node's `SourceLocation` is synthesized from its op ordinal, so the checker's
diagnostics point at the op that made the faulty node. A test constructs one node of every kind,
sets every slot, and round-trips it, so the ABI cannot drift from the tree. Numbers are append-only:
a new AST field is a new slot, and a removed one is retired, never reused. Names in the tree are
strings, as in source, and the checker resolves them.

The seven binding kinds are recorded in the same log through `add_decl`:

| Kind constant                | Slots (`VN_SLOT_*`)                                                                                           |
| ---------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `VN_KIND_NATIVE_AUTHORITY`   | `NAME` (`str`), `PURPOSE` (`str`)                                                                             |
| `VN_KIND_NATIVE_LIBRARY`     | `NAME`, `SONAME`, `CAPACITY`, `SKILL` (`str`), `AFFINITY` (`enum`), `HANDLES`, `FUNCTIONS`, `SEQUENCES`       |
| `VN_KIND_NATIVE_HANDLE_TYPE` | `NAME` (`str`), `C_TYPE` (`str`), `DESTROY` (`str`)                                                           |
| `VN_KIND_NATIVE_FUNCTION`    | `NAME` (`str`), `PARAMS` (`child-list`), `RETURN_TYPE` (`str`), `FAIL_IF` (`opt-enum`), `NONE_IF` (`opt-int`) |
| `VN_KIND_NATIVE_PARAM`       | `NAME` (`str`), `TYPE` (`str`), `SIZE_PARAM` (`opt-str`), `CONSUMED` (`bool`)                                 |
| `VN_KIND_NATIVE_SEQUENCE`    | `NAME` (`str`), `PARAMS` (`child-list`), `RETURN_TYPE` (`str`), `WITHIN_MS` (`int`), `STEPS`, `CLEANUPS`      |
| `VN_KIND_NATIVE_STEP`        | `BIND_NAME` (`opt-str`), `FN_NAME` (`str`), `ARGS` (`child-list`), `IS_CLEANUP` (`bool`)                      |

The replay turns them into two things. One is the declarations they synthesize into the `Unit`
(§10.1, §11.2). The other is the binding manifest the host's runner reads.

### 6.3 What the replay checks before the checker runs

1. **Decode under limits.** Ops at most `vagent.native.op_log_ops`. Strings, nesting and node counts
   are bounded as `wire.hpp`'s decoder bounds values (`kMaxWireNodes`, depth 32).
1. **Shape.** Every id was made by an earlier op and is the child of exactly one parent, so the tree
   is a tree. Required slots are set (`EAsk::within`, `MethodSig::withinMs` when `isEffect`).
1. **Reserved names.** No declaration uses the `std:` prefix or a name the binding manifest
   synthesizes.

Then `check` runs over the `Unit`, and `compileAgent` lowers it. Everything after that is the actor
compiler's existing behaviour.

### 6.4 Two interfaces: the C ABI and the C++ wrapper

The project ships both, with different jobs.

- **The C ABI, `vagent_native.h`, is for adoption.** Every language with a C foreign-function
  interface (Rust, Zig, Go, Python through `ctypes`, and so on) can bind a plain C header with no
  variadic functions and no C++ name mangling. It is complete, deliberately low level, and the only
  interface the host relies on. A binding for another language is written against it, not against
  the wrapper.
- **The C++ wrapper, `vagent_native.hpp`, is for ease of use.** It is a header-only layer in
  namespace `vagent::native` over `vn_api`, shipped beside the C header (`<vagent/vagent_native.h>`
  and `<vagent/vagent_native.hpp>`) and supported as part of this feature, not left as an example.
  `Builder b(api, raw)` wraps the C recorder and `b.finish()` returns `0` when `api->error(raw)` is
  null and `-1` otherwise (`b.status()` returns `std::expected<void, Error>` carrying the op ordinal
  and `error()` text). It gives value-style builders on `b` that mirror the surface language (§5.1):
  - **Declarations:** `b.module(name)`, `b.import_path({...})`, `b.predicate(name, params)`,
    `b.event(name, fields)`, `b.record_type(name, fields)`, `b.goal(name, mode, params)`,
    `b.capacity(name)`, `b.native_skill(name, capacity)` and `b.agent(name)` (with `ag.uses(...)`,
    `ag.install(...)`, `ag.var(...)`, `ag.on(...)`, `ag.plan(...)`).
  - **Bindings and sequences:** `b.authority(kind, purpose)` and
    `b.library(name, soname, affinity, capacity, skill)` returning a `LibraryBuilder` with
    `lib.handle(name, c_type, destroy)`, `lib.fn(name, params, ret, fail_if, none_if)` and
    `lib.sequence(name, params, ret, within_ms)`. On a `SequenceBuilder`, `seq.let(name, call)`
    records a binding step, `seq.step(call)` records an unlabelled `do` step (named `step` in C++
    because `do` is a keyword), `seq.result(arg)` sets the sequence result, and
    `seq.finally_call(call)` appends a cleanup step.
  - **Presentation flags:** It sets `hasParens`, `hasParams` and `hasBody` exactly as the parser
    would, which the C caller must otherwise do by hand.

The wrapper calls only `vn_api` and adds no op. A module written with it therefore produces the same
op log as one written against the C header directly. Because of that, the equality test of §13.2
checks the ABI, not the wrapper. The first test's module uses the wrapper (§5.1, §13.1). A small C
module, `tests/samples/vagent/native/curses_hello_c.c`, builds the same program against the C header
alone, and §13.2 step 1 asserts that the two op logs are byte-identical. That keeps the C ABI honest
as the adoption path. The wrapper follows the C ABI's version. A wrapper built for version N uses
only members present in version N, so a module compiled with it runs on any host that offers N.

## 7. Sealing, loading and `programRef`

### 7.1 The artifact

A `.vnprog` file is `\x89VAGNATV`, format version 1, then a bencoded dictionary with `api_version`,
`op_log` (list of op records, binding kinds included), `module_digest` (the SHA-256 of the module
shared library), and `module_filename` (the basename of the module file recorded at seal time, such
as `greeter.so`, `greeter.dylib` or `greeter.dll`). `module_filename` is deployment metadata outside
`programRef` (N6), so moving or renaming a directory never alters program identity. As AGENTS.md's
on-disk rules require, the reader checks its signature and version and throws a typed refusal naming
what was wrong. The next format bumps the version and deletes the old reader. It is deployment
input, outside every document store. Multi-target `.vnpkg` cluster packages, bundled non-system
shared libraries (`--bundle-lib`), and content-addressed distribution over `MeshSwarmTransport` are
specified in [vagent-cluster-packaging.md](vagent-cluster-packaging.md) (§§5–8).

### 7.2 Canonical form

The op log's canonical form is the recorded sequence, not the tree. Two builds that make the same
tree in a different order are different programs. That keeps `programRef` a hash of bytes the module
produced, rather than of a normalisation the node performs. The double build at seal time (§5)
catches a module that is not deterministic.

### 7.3 Locating and staging the module bytes

At load, and again each time a native host starts, the control plane locates the module file from an
explicit `--module <path>` override (`NativeProgramLoadOptions::modulePath`), or otherwise by
resolving the `.vnprog`'s `module_filename` in the directory containing the `.vnprog` file. If the
file cannot be opened, load refuses with `ModuleNotFound`. The control plane reads the file and
computes its SHA-256 from the bytes read. If the digest is not the `.vnprog`'s `module_digest`, it
refuses with `ModuleDigestMismatch`. It then writes those same bytes into a fresh staging directory
it owns under `vagent.native.staging_dir` (defaulting to `$XDG_RUNTIME_DIR/vagent-native` on
Linux/BSD, `$TMPDIR/vagent-native-<uid>` on macOS, and `%LOCALAPPDATA%\vagent-native` on Windows),
`<staging_dir>/<author>-<device>-<epoch>/<serial>/module.<ext>`, with directory mode 0700 and file
mode 0500 (or owner-only read-execute ACL on Windows). The sandbox grants read and execute beneath
that directory, and nothing writable. The file the host loads is therefore a copy written from the
hashed bytes, in a directory the host cannot write. Replacing the original file after it is hashed
changes nothing. The control plane removes a host's directory when the host exits. At start-up, the
supervisor removes every directory under `<staging_dir>` belonging to an earlier epoch of the same
node, which cleans up after a control plane that died. If the staging root is unset, or on POSIX its
filesystem is mounted `noexec` (`statvfs` `ST_NOEXEC`), launch refuses with `NoStagingDirectory` or
`StagingNoexec`. The test fixture sets it under `build/xdg/run`.

## 8. The native host process and its sandbox

### 8.1 Start-up order and two-phase lockdown

The supervisor starts `vagent-native-host` with the shared spawn helper of P20.7, extracted from
`provenance.cpp:240-254` (POSIX) and `CreateProcessAsUserW` / `UpdateProcThreadAttribute` (Windows).
On Linux the supervisor spawns from one long-lived thread of its own (`PR_SET_PDEATHSIG` tracks the
spawning thread) and also passes a `pidfd` when available. It passes an explicit environment
including `VAGENT_SUPERVISOR_PID` and these eight descriptors (or inherited handles on Windows):

| fd  | What                                                                                                                                   |
| --- | -------------------------------------------------------------------------------------------------------------------------------------- |
| 0   | `/dev/null` (`NUL` on Windows), read                                                                                                   |
| 1   | `/dev/null` (`NUL` on Windows), write                                                                                                  |
| 2   | the log pipe, write (`O_NONBLOCK`, drained continuously by the control plane so foreign `stderr` writes never block)                   |
| 3   | the control channel (`AF_UNIX` `SOCK_SEQPACKET` on Linux/BSD; `AF_UNIX` `SOCK_STREAM` with 4-byte length prefix on macOS; pipe on Win) |
| 4   | Ring A shared region (`shm_a`, control → host), opened **read-only** for the host (§9.1)                                               |
| 5   | Ring B shared region (`shm_b`, host → control plus Ring A's consumer cursor `tail_a`), opened read-write for the host (§9.1)           |
| 6   | doorbell, control → host (`eventfd` on Linux/FreeBSD; non-blocking pipe on macOS/OpenBSD; auto-reset `Event` handle on Windows)        |
| 7   | doorbell, host → control (same per-OS primitive)                                                                                       |

The host's own code, ours and with no foreign code loaded yet, then runs in nine steps:

1. **Parent-death binding.** Bind termination to the supervisor's lifetime: on Linux, set
   `prctl(PR_SET_PDEATHSIG, SIGKILL)` (and register a `pidfd_open` watch on `VAGENT_SUPERVISOR_PID`
   in the I/O `epoll` set when permitted); on FreeBSD, set
   `procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &sig)`; on macOS and OpenBSD, register `EVFILT_PROC`
   (`NOTE_EXIT`) on `VAGENT_SUPERVISOR_PID` in the host's `kqueue`; on Windows, verify membership in
   the supervisor's Job Object (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`). Then verify
   `getppid() == VAGENT_SUPERVISOR_PID` so a parent death before registration is caught immediately.
1. **Descriptor hygiene.** P20.7 sets `POSIX_SPAWN_CLOEXEC_DEFAULT` where supported (and on Windows,
   `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` restricts inheritance to the explicit startup handles). The
   host calls `close_range(8, ~0U, 0)` (or `closefrom(8)`), verifies that fds 0–7 are open, and
   exits if any is missing.
1. **Resource limits.** Set `RLIMIT_AS` to `vagent.native.memory_limit`, `RLIMIT_NOFILE` to
   `vagent.native.max_fds`, `RLIMIT_CORE` 0 and `RLIMIT_FSIZE` 0 (on Windows, enforced by the Job
   Object's `ExtendedLimitInformation`).
1. **Shared memory mapping.** Map `shm_a` (`fd 4`) as `PROT_READ` (`FILE_MAP_READ`) and `shm_b`
   (`fd 5`) as `PROT_READ | PROT_WRITE` (`FILE_MAP_ALL_ACCESS`). Both sizes were fixed and locked
   before spawn (§9.1).
1. **Privilege lock and Capsicum pre-open.** Set `PR_SET_NO_NEW_PRIVS` and `PR_SET_DUMPABLE` 0 on
   Linux (`PROC_NO_NEW_PRIVS_CTL` and `PROC_TRACE_CTL` on FreeBSD). If unprivileged user namespaces
   are permitted on Linux, unshare the network, IPC and mount namespaces (§8.3). On FreeBSD, before
   entering capability mode, pre-open `O_RDONLY | O_EXEC` descriptors for the staged `module.so`,
   the binding manifest's libraries, and `/usr/share/terminfo`, and restrict every open descriptor
   with `cap_rights_limit` and `cap_ioctls_limit` (`shm_a` gets `CAP_READ | CAP_MMAP_R | CAP_FSTAT`,
   never `CAP_WRITE` or `CAP_FTRUNCATE`).
1. **Phase 1 sandbox (pre-load confinement).** Apply filesystem, network and initial syscall
   confinement before any foreign ELF, Mach-O or PE constructor executes (§8.2, §8.3): Landlock and
   the phase-1 seccomp filter on Linux; `cap_enter()` on FreeBSD; `unveil` of library and terminfo
   paths read/exec followed by `unveil(nullptr, nullptr)` and
   `pledge("stdio rpath prot_exec recvfd")` on OpenBSD; `sandbox_init` with the deny-default
   Seatbelt profile on macOS; AppContainer and restricted token activation on Windows.
1. **Module and library load.** Load the staged module (`fdlopen` on FreeBSD, `dlopen` on
   Linux/OpenBSD/macOS, `LoadLibraryExW` on Windows) and the binding manifest's libraries. Resolve
   every declared FFI symbol and optional `vagent_native_skills`. A missing library or symbol fails
   start-up by name (`NativeLoadError`).
1. **Phase 2 lockdown (post-load W^X enforcement).** Once all libraries and symbols are loaded,
   revoke the ability to create or transition new executable memory (`PROT_EXEC`) so a memory safety
   bug in foreign code during effect execution cannot JIT shellcode:
   - *Linux:* call `prctl(PR_SET_MDWE, PR_MDWE_REFUSE_EXEC_GAIN, 0, 0, 0)` (Linux 6.3+) and install
     a second stacked seccomp filter that denies `mmap` and `mprotect` with `PROT_EXEC`
     (`SCMP_CMP_MASKED_EQ` on `prot`, `PROT_EXEC`, `PROT_EXEC` returning `EPERM`) and denies
     `mremap` with `MREMAP_MAYMOVE` on executable mappings.
   - *FreeBSD:* close the pre-opened library descriptors; no remaining descriptor holds
     `CAP_MMAP_X`.
   - *OpenBSD:* tighten `pledge` to `pledge("stdio rpath recvfd")` (or `"stdio rpath recvfd tty"`
     when the manifest declares `terminal`, with `TIOCSTI` disabled by the kernel), dropping
     `prot_exec`.
   - *macOS:* the Seatbelt profile denies `MAP_JIT` and any `file-map-executable` outside the
     read-only system/staging library subpaths.
   - *Windows:* call `SetProcessMitigationPolicy(ProcessDynamicCodePolicy, ...)` with
     `ProhibitDynamicCode = 1` and `ProcessSystemCallDisablePolicy` with
     `DisallowWin32kSystemCalls = 1`.
1. **Serve.** Report resolved library paths and digests in a `Ready` frame on Ring B. The host's I/O
   thread calls `_exit` on end-of-file on `fd 3`, supervisor-exit notification, or a failed doorbell
   read, even while a library thread is blocked in a foreign call.

### 8.2 Cross-platform sandbox and IPC parity

Every supported platform enforces the same five invariants: no filesystem writes anywhere, no
network access, read-only Ring A (`shm_a`) and size-locked Ring B (`shm_b`), post-load W^X
enforcement, and immediate termination when the control plane exits.

| Platform | Process & parent-death                              | No filesystem writes                                                    | No network                             | Call & W^X filter (Phase 1 → Phase 2)                                     | Shared memory, doorbells & grants                                                                              |
| -------- | --------------------------------------------------- | ----------------------------------------------------------------------- | -------------------------------------- | ------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| Linux    | `posix_spawn`; `PR_SET_PDEATHSIG` + `pidfd`; userns | Landlock (ABI 4+): all write rights handled, none granted; RX on libs   | Landlock TCP + seccomp `socket`        | seccomp-bpf phase 1 → phase 2 (`PR_SET_MDWE`, no `PROT_EXEC`, §8.3)       | 2 `memfd_create` (`shm_a` RO via `/proc/self/fd` + seals); 2 `eventfd`; `SOCK_SEQPACKET` + `SCM_RIGHTS`        |
| FreeBSD  | `posix_spawn`; `PROC_PDEATHSIG_CTL`; `cap_enter`    | Capsicum: no global namespace; libs pre-opened RO/RX before `cap_enter` | no socket descriptors or syscalls      | `cap_rights_limit` + `cap_ioctls_limit`; close RX lib fds in phase 2      | 2 `shm_open2(SHM_ANON)` (`shm_a` `CAP_MMAP_R` only); 2 `eventfd`; `SOCK_SEQPACKET` + `SCM_RIGHTS`              |
| OpenBSD  | `posix_spawn`; `kqueue` `EVFILT_PROC` `NOTE_EXIT`   | `unveil` staging/libs/terminfo `"rx"`/`"r"`, then `unveil(NULL, NULL)`  | no `inet`/`unix`/`dns` in `pledge`     | `pledge` drops `prot_exec` in phase 2; kernel `tk_can_tiocsti=0`          | 2 `shm_open` (`shm_a` re-opened `O_RDONLY`) + `shm_unlink`; 2 `pipe2`; `SOCK_SEQPACKET` + `SCM_RIGHTS`         |
| macOS    | `posix_spawn`; `kqueue` `EVFILT_PROC` `NOTE_EXIT`   | Seatbelt `(deny default)` `(deny file-write*)`; `file-read*` on libs    | `(deny network*)` `(deny mach-lookup)` | Seatbelt profile; no `MAP_JIT`                                            | 2 `shm_open` (`shm_a` re-opened `O_RDONLY`) + `shm_unlink`; 2 `pipe`; `SOCK_STREAM` (4B len) + `SCM_RIGHTS`    |
| Windows  | AppContainer + Restricted Token + Job Object        | AppContainer SID; staging/system DLLs read-execute ACL only             | no network capabilities in token       | Job limits; `ProhibitDynamicCode` + `DisallowWin32kSystemCalls` (phase 2) | 2 `CreateFileMappingW` (`shm_a` duplicated `FILE_MAP_READ`); 2 `Event`; named pipe + `DuplicateHandle`; ConPTY |

### 8.3 Linux in detail

**Landlock.** ABI 4 (Linux 6.7) is required, for its network rules. The ruleset handles every
filesystem access right the running ABI knows. It adds read-and-execute rules beneath the staging
directory (§7.3) and `vagent.native.library_paths`. That defaults to the dynamic loader's
directories and `/etc/ld.so.cache`, plus read-only rules for `/usr/lib/locale`, `/usr/share/locale`,
`/usr/share/terminfo`, `/etc/terminfo` and `/lib/terminfo`. Neither `/proc`, `/sys`, `/dev/shm` nor
the owner's home directory is in the ruleset, so `/proc/self/mem` and `~/.terminfo` are unreadable.
A terminal type the system terminfo database lacks fails `newterm`, and the failure says so. No
write rule exists anywhere. On ABI 6 (Linux 6.12) the ruleset also scopes signals and abstract Unix
sockets. That is defence in depth, because seccomp already covers both, below.

**Seccomp (two-phase).** Both filters are built with libseccomp. Their first rule checks the
architecture: `SCMP_FLTATR_ACT_BADARCH` is `KILL_PROCESS`, so a system call through a non-native ABI
(the 32-bit `int 0x80` path on x86-64) kills the host. On x86-64, any call number with
`__X32_SYSCALL_BIT` set also kills it. Only then do number-based rules apply. Any call not listed
below returns `ERRNO(ENOSYS)`, so libc probes for new calls take their fallbacks.

- **Allowed in Phase 1 (step 6), tightened in Phase 2 (step 8).** `read`, `write`, `readv`,
  `writev`, `pread64`, `pwrite64`, `sendmsg`, `recvmsg` on any descriptor, because the descriptors
  are the authority; `openat` (Landlock decides); `close`, `close_range`, `dup`, `dup3`, `lseek`;
  `fcntl` restricted by `SCMP_CMP_MASKED_EQ(1, 0xFFFFFFFFULL, cmd)` to `F_GETFL`, `F_SETFL` (masked
  to `O_NONBLOCK`), `F_GETFD`, `F_SETFD`, `F_DUPFD` and `F_DUPFD_CLOEXEC`; `fstat`, `newfstatat`,
  `statx`, `readlinkat`, `getdents64`, `access`, `faccessat`, `faccessat2` (ncurses' `_nc_access`
  checks terminfo files with them), `getcwd`, `pipe2`, `ftruncate` (the memfd seals refuse it on the
  shared regions with `EPERM`, which the probe observes); `mmap`, `munmap`, `mremap`, `mprotect`
  (Phase 1 allows `PROT_EXEC` for `dlopen` in step 7; Phase 2 in step 8 calls
  `prctl(PR_SET_MDWE, PR_MDWE_REFUSE_EXEC_GAIN, 0, 0, 0)` and stacks a filter returning `EPERM`
  whenever `prot & PROT_EXEC` is non-zero in `mmap` or `mprotect`, or when `mremap` is called on an
  executable mapping), `madvise`, `brk`; `clone` with `CLONE_THREAD`, `set_robust_list`, `rseq`,
  `set_tid_address`, `futex`, `sched_yield`, `sched_getaffinity`; `poll`, `ppoll`, `epoll_create1`,
  `epoll_ctl`, `epoll_wait`, `epoll_pwait`; `clock_gettime`, `clock_nanosleep`, `nanosleep`;
  `getpid`, `getppid`, `gettid`, `getuid`, `geteuid`, `getgid`, `getegid`, `uname`, `getrandom`;
  `prlimit64` with a null new limit only; `ioctl` restricted by
  `SCMP_CMP_MASKED_EQ(1, 0xFFFFFFFFULL, cmd)` to the terminal allowlist `TCGETS`, `TCSETS`,
  `TCSETSW`, `TCSETSF`, `TIOCGWINSZ`, `TIOCSWINSZ` and `FIONREAD` (masking the low 32 bits of
  `args[1]` prevents 64-bit register high-bit or sign-extension bypasses of `sys_ioctl`'s 32-bit
  `unsigned int cmd`); `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `sigaltstack`; `tgkill` and
  `kill` aimed at the host's own pid (so `abort()` can raise `SIGABRT`); `exit`, `exit_group`. The
  list was derived with `strace -f` over the first test, and it is checked, not only written down. A
  check build of the host sets the default action to `TRAP`. Its `SIGSYS` handler writes the trapped
  call's number to the log pipe with an async-signal-safe `write`, then exits. The first test's
  suite runs once against the check build and fails if any call was trapped. That needs no audit-log
  access, so an unprivileged test can run it.
- **`ENOSYS`: `clone3` and `openat2`.** glibc 2.34+ tries `clone3` for `pthread_create` and falls
  back to `clone` on `ENOSYS`; glibc also probes `openat2` and falls back to `openat`. Seccomp
  cannot inspect pointer arguments of `clone3` or `openat2`, and can inspect `clone`'s flags while
  Landlock governs `openat`.
- **`EPERM`, so a probe can observe the refusal.** `socket`, `socketpair`, `connect`, `bind`,
  `listen`, `accept`, `accept4`; `execve`, `execveat`; `fork`, `vfork`, and `clone` without
  `CLONE_THREAD`; `ptrace`, `process_vm_readv`, `process_vm_writev`; `kill`, `tgkill` and
  `rt_sigqueueinfo` whose target pid or tgid is not the host's own pid (the filter is built after
  `getpid`, so it compares with a constant); `tkill`, `rt_tgsigqueueinfo` for other tgids,
  `pidfd_open`, `pidfd_send_signal`; `mount`, `umount2`, `pivot_root`, `chroot`, `unshare`, `setns`;
  `keyctl`, `add_key`, `request_key`; `bpf`, `perf_event_open`, `userfaultfd`; `io_uring_setup`,
  `io_uring_enter`, `io_uring_register` (io_uring submissions are not seen by seccomp);
  `memfd_create` (no feature needs it: libffi closures are refused, §11.3); any `ioctl` outside the
  masked terminal allowlist above (including `TIOCSTI`, `TIOCLINUX` and `TIOCSETD`, even with upper
  32 bits set in the 64-bit register); and in Phase 2, `mmap` or `mprotect` with `PROT_EXEC`.

**Namespaces.** If unprivileged user namespaces are permitted, the host unshares network, IPC and
mount namespaces before Landlock. Many distributions restrict them, and Landlock plus seccomp stand
without them.

**Why both.** Landlock decides by object (what may be opened). Seccomp decides by operation (what
may be called) and by integer argument. Landlock cannot refuse `TIOCSTI` on a granted descriptor.
Seccomp cannot tell `/usr/lib` from `/home`.

### 8.4 What the host reports

The `Ready` frame lists each library's resolved path and SHA-256. The control plane logs them under
`vagent.native` at debug level. If a later incarnation resolves a different digest, it logs that at
info level, because a system update has changed a library under a running program (N6). These are
diagnostics, never authority: the host is untrusted once foreign code has loaded.

## 9. IPC with the control plane

### 9.1 Channels, asymmetric shared memory and SPSC ring layout

| Channel          | Direction      | Carries                                                                                         |
| ---------------- | -------------- | ----------------------------------------------------------------------------------------------- |
| Ring A (`shm_a`) | control → host | `StartEffect`, `CancelEffect`, `Revoke`, `ArmReadable`, `Shutdown` (mapped **RO** in host)      |
| Ring B (`shm_b`) | host → control | Ring A consumer cursor `tail_a`; `Ready`, `EffectDone`, `EffectFailed`, `Readable`, `Log`       |
| control channel  | both           | `GrantInstall` + descriptor/handle (control → host) and `GrantAck` (host → control); EOF = dead |
| doorbells        | both           | one wakeup channel per direction (`fd 6` control → host, `fd 7` host → control)                 |
| log pipe         | host → control | the host's `stderr` (`O_NONBLOCK`), drained continuously and rate-limited in memory             |

**Asymmetric shared memory (`shm_a` vs. `shm_b`).** Placing both rings in one writable shared-memory
object would let a compromised host `mmap` or `mprotect` Ring A as writable and overwrite Ring A's
frames or producer cursor while the control plane writes, causing C++23 data races. The control
plane therefore creates **two** independent shared-memory objects of size
`sizeof(RingHeader) + vagent.native.ring_bytes` (`ring_bytes` a power of two, default 1 MiB), seals
their sizes before spawn, and gives the host **read-only** access to `shm_a` (`fd 4`, opened
`O_RDONLY` via `/proc/self/fd` on Linux, restricted to `CAP_MMAP_R` on FreeBSD, re-opened `O_RDONLY`
before `shm_unlink` on macOS/OpenBSD, and duplicated with `FILE_MAP_READ` on Windows):

```cpp
struct alignas(128) ShmARegion {
  alignas(128) uint64_t head_a;          // written by control (release), read by host (acquire)
  uint64_t capacity;                     // ring_bytes (power of 2)
  uint32_t max_frame_bytes;              // vagent.native.frame_bytes (default 64 KiB)
  alignas(128) uint8_t data_a[];         // ring_bytes payload (read-only in host)
};

struct alignas(128) ShmBRegion {
  alignas(128) uint64_t tail_a;          // Ring A consumer cursor: written by host (release)
  alignas(128) uint64_t head_b;          // Ring B producer cursor: written by host (release)
  alignas(128) uint64_t tail_b_published;// Ring B consumer cursor: written by control (release)
  uint64_t capacity;                     // ring_bytes (power of 2)
  uint32_t max_frame_bytes;
  alignas(128) uint8_t data_b[];         // ring_bytes payload
};
```

Each cursor occupies its own 128-byte cache line (covering 64-byte x86-64 and 128-byte Apple Silicon
lines) so producer and consumer never false-share. Cursors are monotonic 64-bit byte offsets; the
byte offset in the ring is `cursor & (ring_bytes - 1)`.

**Contiguous framing and wrap-around.** Every ring entry begins with a 32-byte aligned fixed binary
header (`FrameHeader`, §9.2). If the bytes remaining from `head & (ring_bytes - 1)` to `ring_bytes`
are less than `sizeof(FrameHeader) + payload_len`, the producer writes a wrap marker
(`frame_len = 0xFFFFFFFFU`) at `head & (ring_bytes - 1)`, advances `head` to the next multiple of
`ring_bytes`, and writes the 32-byte `FrameHeader` and `payload_len`-byte payload contiguously
starting at offset `0` (padded up to an 8-byte boundary). Frame totals are at most
`vagent.native.frame_bytes` (default 64 KiB), exceeding the 32 KiB `maxOutputBytes` turn scratch
bound (§4.2 of the specification).

**C++23 atomic ordering and doorbell wakeup.** A producer writes the frame bytes, executes
`std::atomic_ref<uint64_t>(head).store(new_head, std::memory_order_release)`, issues
`std::atomic_thread_fence(std::memory_order_seq_cst)`, and signals the peer's doorbell. A consumer
wakes, reads `head` with `std::atomic_ref<uint64_t>(head).load(std::memory_order_acquire)`, copies
each frame into a pre-allocated thread-local 64 KiB scratch buffer, and publishes `tail` with
`std::memory_order_release`.

**Synchronous grant installation (`GrantInstall` / `GrantAck`).** Passing a grant descriptor over
`fd 3` out-of-band from Ring B would race if `open_terminal` completed before the host read `fd 3`.
When the broker grants an authority, it sends a fixed-size binary `GrantInstall{seq, token, kind}`
struct with the attached descriptor (`SCM_RIGHTS` on POSIX; duplicated handle value on Windows) on
`fd 3`. The host's I/O thread receives `GrantInstall`, inserts `token -> fd` into its grant table,
and replies on `fd 3` with `GrantAck{seq, token}`. The broker invokes `EffectCompletion` for
`open_terminal` **only after** receiving `GrantAck` (bounded by `open_terminal`'s deadline),
guaranteeing the descriptor is registered in the host before the agent's next turn can stage a
foreign call using `token`.

**Backpressure.** Staging into Ring A is never allowed to block the control plane: if Ring A lacks
space for a frame (`new_head - tail_a > ring_bytes`), `RemoteNativeSkill::start` fails the effect at
once with cause `busy`. On Ring B, if the host finds insufficient space, the host's writer waits on
the control→host doorbell (which the control plane signals whenever advancing `tail_b_published`
frees space from a full ring) for up to `vagent.native.call_grace`; if Ring B is still full after
the grace period, the host exits so a wedged ring never deadlocks.

### 9.2 Zero-allocation binary frames and `WireValueView`

Whereas `.vnprog` artifacts and the op log (§7.1) use canonical bencode for deterministic SHA-256
hashing (`programRef`), the hot shared-memory rings use a **fixed 32-byte native little-endian
binary header** followed by flat Tag-Length-Value (`wire.hpp`) payload bytes, parsed without heap
allocations:

```cpp
enum class FrameType : uint16_t {
  StartEffect = 1, CancelEffect = 2, Revoke = 3, ArmReadable = 4, Shutdown = 5,
  Ready = 100, EffectDone = 101, EffectFailed = 102, Readable = 103, Log = 104,
};

struct alignas(8) FrameHeader {
  uint32_t frame_len;    // total bytes (sizeof(FrameHeader) + payload_len), or 0xFFFFFFFFU wrap marker
  uint16_t magic;        // 0x564E ("VN")
  FrameType type;        // discriminated message type
  uint64_t seq;          // correlating sequence / EffectRequest id
  uint8_t token[16];     // 128-bit raw CSPRNG grant/effect token (zero if unused)
};
static_assert(sizeof(FrameHeader) == 32);
```

- **Zero-allocation `WireValueView`.** Effect arguments and results use the binary TLV encoding of
  `vagent/runtime/wire.hpp`. On the IPC hot path, `decodeValueView(std::span<const uint8_t>)`
  validates tags, lengths and UTF-8 in a single linear pass and exposes strings, identifiers and
  record fields as `std::string_view` / `std::span` slices pointing directly into the caller's 64
  KiB scratch buffer (`thread_local alignas(64) std::array<uint8_t, 65536>`). Decoders return
  `std::expected<WireValueView, NativeIpcError>` with zero dynamic allocations and zero external
  dependencies.
- **Identity.** Effect frames carry the `EffectRequest` sequence id and 128-bit token in
  `FrameHeader`; the agent pid (P20.1) and incarnation are implied by the dedicated per-agent host
  the ring belongs to.

### 9.3 The host is hostile

The control plane's I/O thread treats `shm_b`, `fd 3` and the doorbell as attacker-controlled:

- It keeps authoritative private copies of `head_a` and `tail_b` in control-plane memory, and reads
  `tail_a` and `head_b` from `shm_b` once per drain with `std::memory_order_acquire`, validating
  `tail_a <= head_a`, `head_b >= tail_b`, and `head_b - tail_b <= ring_bytes`. Any cursor regression
  or out-of-range distance is a protocol violation.
- **Private scratch copy defeats shared-memory TOCTOU.** Because `shm_b` remains mapped `PROT_WRITE`
  in the untrusted host, validating or traversing offsets in place inside `data_b` (as zero-copy
  vtable formats such as FlatBuffers or Cap'n Proto do) would expose a double-fetch / TOCTOU race if
  a second host thread mutated `data_b` between verification and dereference. Instead, the I/O
  thread copies each frame (`<= 64 KiB`) once out of `data_b` into its pre-allocated thread-local 64
  KiB scratch buffer before inspecting `FrameHeader` or constructing a `WireValueView`, achieving
  both **zero heap allocation** and complete immunity to concurrent guest mutation.
- It decodes under limits. A bad `magic`, an unknown `FrameType`, a `frame_len` below
  `sizeof(FrameHeader)` or exceeding `frame_bytes`, a `decodeValueView` failure, a result whose type
  is not the method's declared result type, a completion for an unknown `seq`/`token`, or a
  `Readable` for a grant that is not armed is a protocol violation.
- It receives `GrantAck` on `fd 3` with `MSG_CMSG_CLOEXEC`, expects no descriptors from the host,
  and closes any that arrive (`MSG_CTRUNC` or non-empty ancillary data is a protocol violation).
- It drains at most `vagent.native.drain_per_wake` frames per doorbell so a flooding host cannot
  monopolise the thread. If unread frames remain in Ring B after reaching `drain_per_wake`
  (`head_b != tail_b`), the I/O thread immediately re-arms its own doorbell before returning to the
  poll loop so remaining burst frames are never stranded.
- It drains the non-blocking log pipe (`fd 2`) continuously at full speed, forwarding up to
  `vagent.native.log_bytes_per_second` to `vagent.native` and dropping excess bytes in memory with a
  dropped-byte count so foreign `stderr` writes never block.
- On a protocol violation it kills the host (§12.1). It never tries to resynchronise.

The I/O thread only enqueues. It reports through `EffectCompletion`, which may be called from any
thread, with only the first call counting (`host_bindings.hpp`), and through
`AgentExecutor::percept`, which is callable from any thread (`agent_executor.hpp:334-341`). It never
runs agent code.

## 10. Authorities

### 10.1 Declared, synthesized, requested

A module declares each authority kind it may request with a `NativeAuthority` node. The node carries
the kind and a purpose string, which is shown to the owner and is the only source of that text. Each
declared kind synthesizes into the `Unit` a typed capacity, a native skill implementing it, a record
and its events, all named by a fixed rule from the kind. The checker therefore types every use, and
an undeclared kind is a name the checker does not know, so it is refused at seal, not at run time.
For `terminal`:

```vagent
record Terminal { token: Str, term: Str, rows: Int, cols: Int }
capacity Terminals {
  effect def open_terminal(rows: Int, cols: Int): Terminal within 60s
  effect def hold_terminal(t: Terminal) within 1s
  effect def close_terminal(t: Terminal) within 1s
  effect def watch_terminal(t: Terminal) within 1s
}
native skill HostTerminals implements Terminals
event TerminalRevoked { token: Str, cause: Str }
event TerminalReadable { token: Str }
event NativeCallFailed { method: Str, step: Int, ret: Int, errno: Int }
```

These are ordinary declarations in the program, emitted by the replay (`NativeCallFailed` is emitted
once whenever the manifest declares a library), so no prelude change is needed (`prelude.cpp` holds
events only). Their effect methods compile once Q1 has landed.

- **The deadline** on `open_terminal` is the time a human has to decide under `ask`. If it passes
  with the prompt still open, the broker withdraws the prompt. A decision that arrives later creates
  no grant, and the ledger records `expired`.
- **The token** is opaque: 128 random bits from the control plane's CSPRNG, hex-encoded. The broker
  accepts it only from the pid it was granted to, so a fabricated or copied token fails `stale`.
- **Scope.** The grant belongs to the top-level intention that staged `open_terminal` (N13), which
  the broker learns from P20.1's request field. It is released with cause `intention_ended` when
  that intention ends, unless `hold_terminal` has moved it to the agent first.
- **Closing or re-holding a token.** `close_terminal` is exempt from `stale`: on a token already
  revoked it succeeds and does nothing. `hold_terminal` on a token already held by this agent
  succeeds and does nothing; on a revoked token it fails `stale` like any other use.

### 10.2 Policy

The broker decides by `vagent.native.grant_policy`:

| Value      | Decision                                                                                                         |
| ---------- | ---------------------------------------------------------------------------------------------------------------- |
| `deny`     | Every request is refused                                                                                         |
| `ask`      | The owner is asked through the host's approval hook (`vagent run`'s prompt, P20.8; a test's callback). Default   |
| `approved` | Granted if the authority ledger's latest approval event for this kind and this exact `programRef` is an approval |

An approval is per `programRef`, so a new build of the module needs a new approval. That is why the
module digest is part of the ref (N6, §7.3). If the ledger cannot be read, `approved` grants nothing
(§10.5).

### 10.3 Failure causes

The causes are a fixed vocabulary, compared with `==`. Detail that would not fit a fixed word goes
to the log and, for foreign calls, to a `NativeCallFailed` percept (§11.4).

| Cause                | Meaning                                                                                    |
| -------------------- | ------------------------------------------------------------------------------------------ |
| `refused`            | Policy or the owner said no                                                                |
| `unavailable`        | The broker could not create the resource (no pty available)                                |
| `stale`              | The token is unknown for this pid: revoked, from another incarnation, or never granted     |
| `host_lost`          | Revoked because the agent's native host died                                               |
| `released`           | Revoked by `close_terminal`                                                                |
| `intention_ended`    | Revoked because the intention that requested it ended (N13)                                |
| `owner`              | Revoked by the owner                                                                       |
| `agent_ended`        | Revoked because the agent stopped, terminated, or its lease ended                          |
| `program_changed`    | Revoked by a live upgrade to a program with a different module or binding manifest (§12.4) |
| `ffi_error`          | A step's `fail_if` matched                                                                 |
| `ffi_range`          | A value did not fit its C or vagent type                                                   |
| `ffi_argument`       | An argument was invalid before the call (an embedded NUL in a string)                      |
| `ffi_encoding`       | A returned string was not valid UTF-8                                                      |
| `busy`               | The library's queue or Ring A was full                                                     |
| `native_protocol`    | The host broke the protocol, and was killed                                                |
| `native_crashed`     | The host died                                                                              |
| `native_killed`      | The host was killed for overrunning a deadline                                             |
| `native_unavailable` | The host is backing off after repeated failed starts                                       |

`open_terminal` fails with `refused`, `unavailable` or the deadline. `TerminalRevoked.cause` is one
of `host_lost`, `released`, `intention_ended`, `owner`, `agent_ended` and `program_changed`.

### 10.4 `terminal`

On POSIX (Linux, FreeBSD, OpenBSD, macOS), the broker calls `posix_openpt(O_RDWR | O_NOCTTY)`,
`grantpt` and `unlockpt`, sets the window size (`TIOCSWINSZ`) from the request, and installs the
slave descriptor in the host via `GrantInstall` / `GrantAck` (§9.1). On Windows, the broker creates
a ConPTY (`CreatePseudoConsole({cols, rows}, hInRead, hOutWrite, 0, &hPC)`), keeps `hPC` and the
master pipe ends, and duplicates the slave pipe handles into the host via `DuplicateHandle` inside
`GrantInstall` / `GrantAck`. `Terminal.term` is the terminal type: `xterm-256color` by default, or
the owner's `TERM` when relaying. The host never learns a filesystem path for the slave.

The master goes where the owner says. In a test harness the harness reads it. In `vagent run`, with
the owner's consent, the broker relays master to its own terminal and keystrokes back. The relay is
a second security gate, because terminal-write authority is also authority over the owner's terminal
emulator: unvetted CSI queries (`CSI 6 n` cursor position report, `CSI c` device attributes,
`CSI 21 t` window title report) cause terminal emulators to inject synthetic response bytes into the
owner's standard input. `vagent.native.terminal_filter = strict` (the default) therefore runs a
state-machine parser with a **closed allowlist**:

- **Printable text and C0 controls:** valid UTF-8 code points (`U+0020` and above, excluding `DEL`
  `0x7F` and C1 `0x80`–`0x9F`) and the five C0 controls `BEL` (`0x07`), `BS` (`0x08`), `HT`
  (`0x09`), `LF` (`0x0A`) and `CR` (`0x0D`).
- **Two- and three-byte escapes:** `ESC 7` and `ESC 8` (`DECSC`/`DECRC`), `ESC =` and `ESC >`
  (`DECKPAM`/`DECKPNM`), and `ESC ( B` and `ESC ( 0` (G0 US-ASCII and DEC Special Graphics line
  drawing used by ncurses).
- **Standard CSI sequences (`ESC [` `[0-9;]*` `<final>`):** restricted to the closed final-byte set
  `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H`, `J`, `K`, `L`, `M`, `P`, `S`, `T`, `X`, `@`, `d`, `f`, `m`
  (SGR attributes) and `r` (DECSTBM scroll region), with at most 16 numeric parameters each
  $\le 9999$.
- **DEC private modes (`ESC [ ?` `<mode>` `h` | `l`):** restricted exclusively to `?25` (cursor
  visibility) and `?1049` (alternate screen buffer).
- **Dropped:** every other escape or control sequence—including all OSC, DCS, APC, PM and SOS
  strings, status and position queries (`n`), device attributes (`c`), window manipulation (`t` /
  XTWINOPS), key remapping (`p`), and input-generating private modes (`?1000`, `?1002`, `?1003`,
  `?1004`, `?1006`, `?2004`)—is silently dropped by `strict`. `none` passes everything. The test
  harness reads the raw stream.

When the relay attaches, the broker records the owner terminal's `termios` (or console mode on
Windows), and tracks the private modes the relay has passed (`?25` and `?1049` under `strict`, plus
mouse/paste/focus modes under `none`). On revocation for any cause, including a host killed
mid-screen, it emits the inverse of every mode still set, then an SGR reset (`ESC [ 0 m`), then
restores `termios`. The owner's terminal is left as it was, whether or not the agent's `endwin` ever
ran.

Revocation closes the master (and `hPC` on Windows). From then on the slave reads end-of-file and
its writes fail, whether or not the host cooperates. The broker also sends `Revoke`, so the host
closes its copy and marks the token dead, and it posts `TerminalRevoked{token, cause}` to the agent.

### 10.5 The authority ledger

Approvals and the history of every decision are structure, not C++ rows and not packed strings in a
setting (`system_docs.hpp:760-765` admits only scalar settings). The broker's I/O thread is the sole
writer to the ledger, recording Structure operations into a private store, `authorities/`, in the
node's deployment directory. It is deployment metadata that no reader visits, so R8 is untouched.

**Its permascroll.** A store holds no primedia, and the default `Store()` makes an in-memory
permascroll (`store.cpp:123-125`). The ledger therefore has its own permascroll,
`authorities/permascroll/`, created with the store, bound to the node's device identity, and never
published. Its text (program refs, kind names, purposes, agent names) stays out of the author's
permascroll, which is the one that gets published. When the store is opened, a missing or unreadable
permascroll is a typed refusal (`LedgerPermascrollMissing`). The broker then runs with no ledger:
`approved` grants nothing, and `ask` still works but records nothing, which the log says.

**Topology.** Every relation is a chain or a quotation. No two cells link to one target on the same
dimension and direction:

- **Approvals.** One head cell per approved `programRef`, its content the ref. Its `d.authority`
  rank holds one cell per kind, each with the kind as content. A second approval of the same pair
  finds the existing cells. Each kind cell heads a `d.approval` rank of events in time order. An
  event cell's content is `approve` or `withdraw`, with its time and author chained along `d.vars`
  with `d.values` to each value. The current state is the rank's tail. `vagent native approve` and
  `vagent native withdraw` append events (P20.8).
- **Decisions.** One global chronological `d.decision` rank, appended at its tail. A decision does
  not link to its approval. Its program, kind and purpose attributes are values whose content spans
  *quote* the approval's spans and the purpose copy spooled once per program into
  `authorities/permascroll/`, so they are transclusions inside the private permascroll and not
  links. Its agent name, policy, outcome (`granted`, `refused` or `expired`) and time are chained
  along `d.vars` with `d.values` to each value.
- **Revocations.** At most one per decision, linked from it along `d.revocation`, which is
  one-to-one (a racing second revocation is ignored). Its cause and time are chained along `d.vars`
  with `d.values`.

`d.authority`, `d.approval`, `d.decision` and `d.revocation` are minted by `ensureDimension` (R12).
They are dimensions of this private store only, not of any system xanadoc, so they need no schema or
notes pages. Each `SetLink`'s subject follows R7.

The broker holds the live table (token, pid, intention, incarnation, descriptor, pty master) in
memory, because those are process-local and must never persist. The ledger records what was decided
and why.

*Price:* a decision is one cell and seven attribute pairs. Each pair is a name cell and a value
cell, two `MakeCell` and two `SetLink`, so the pairs come to 28 operations. One more `MakeCell` and
a `SetLink` to append the decision bring it to about 30. A revocation is about 14. An approval event
is about 12. Purpose text is spooled once per program. Retention is the store's own segment growth
(NQ5).

## 11. Foreign calls through libffi

### 11.1 Two kinds of native binding

A native skill is bound either to the generic libffi runner (`NativeFunction` and `NativeSequence`
nodes) or to the module's own C function table (`vagent_native_skills` returning a `vn_skill_entry`
array of `start`/`cancel` pairs, §6.1, or the C++ `SkillRegistrar` helper in `vagent_native.hpp`,
§6.4). The first test uses libffi, because the point is that an agent reaches a stock library it did
not write, with no glue compiled for it; `escape_probe.cpp` (§13.4) uses `vagent_native_skills`.

### 11.2 Declarations

```text
library  ncurses  soname "libncursesw.so.6"  affinity one_thread  capacity Curses  skill CursesFfi
handle   Screen   c_type SCREEN  destroy delscreen
handle   Stream   c_type FILE    destroy stdio.close
fn  newterm(type: cstring_in, outf: Stream, inf: Stream) -> Screen        fail_if null
fn  delscreen(sp: Screen consumed) -> void
fn  mvaddstr(y: c_int, x: c_int, s: cstring_in) -> c_int                 fail_if eq -1
fn  refresh() -> c_int                                                    fail_if eq -1
fn  mvinnstr(y: c_int, x: c_int, s: cstring_out(n), n: c_int) -> c_int   fail_if eq -1
fn  endwin() -> c_int                                                     fail_if eq -1
library  libc  soname "libc.so.6"  affinity any  capacity Keys  skill KeysFfi
fn  fgetc(f: Stream) -> c_int                                             none_if eq -1
```

This is a reading form (constructed via `b.library`, `lib.handle`, `lib.fn` in §6.4, or raw
`VN_KIND_NATIVE_*` ops in §6.2). The nodes are `NativeLibrary`, `NativeHandleType`, `NativeFunction`
and `NativeParam`, and on disk the log is bencode. On non-ELF platforms the loader maps standard
`.so` sonames (`libncursesw.so.6`, `libc.so.6`) to the platform's shared-library naming convention
(`.dylib` on macOS, `.dll` on Windows) when resolving `soname`. Each parameter is
`name: type [modifier]*`, and each result is `-> type [modifier]*`. The modifiers are `consumed` (on
a handle parameter) and `fail_if <cond>` and `none_if <cond>` (on a result), where `<cond>` is
`null | eq N | ne N | lt N | le N | gt N | ge N`. `none_if` makes the agent-side result `Opt<T>`:
the C sentinel becomes `none` at the boundary, and nothing past it ever sees `-1`. The `library`
line names the capacity and the native skill it synthesizes. Its functions are not effects
themselves: only its sequences are (§11.4).

| Type                   | C side                                 | Agent side                           | Notes                                                                                                                                                                                       |
| ---------------------- | -------------------------------------- | ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `i8`…`i64`, `u8`…`u64` | that integer                           | `Int`                                | Range-checked; out of range fails `ffi_range`                                                                                                                                               |
| `c_int`, `c_long`      | the platform's `int`, `long`           | `Int`                                | Range-checked                                                                                                                                                                               |
| `f32`, `f64`           | `float`, `double`                      | `Float`                              | A non-finite result fails `ffi_range`, as the wire refuses it                                                                                                                               |
| `c_bool`               | `_Bool`                                | `Bool`                               | An `int` used as a flag is `c_int`                                                                                                                                                          |
| `cstring_in`           | `const char *`, a NUL-terminated copy  | `Str`                                | An embedded NUL fails `ffi_argument`                                                                                                                                                        |
| `cstring_out(n)`       | a runner-owned buffer of `n + 1` bytes | a `Str` field of the step result     | `n` is another `c_int` parameter requiring `0 <= n <= vagent.native.max_out_bytes` (`n < 0` or `n > max_out_bytes` fails `ffi_range` before allocation); invalid UTF-8 fails `ffi_encoding` |
| a declared handle      | the pointer, from the handle table     | a record `{token: Str}` of that name | 128-bit CSPRNG hex token bound to its `NativeHandleType` and host incarnation; unknown or wrong-type token fails `stale`                                                                    |
| an authority record    | `int`, the grant's descriptor          | that record (`Terminal`)             | Never a raw descriptor number from the agent                                                                                                                                                |
| `void`                 | —                                      | `()`                                 |                                                                                                                                                                                             |

`errno` is captured immediately after each call. A `consumed` handle is removed from the table only
if the call succeeded. The runner supplies `stdio.stream(t: Terminal, mode: Str) -> Stream` (`dup`
then `fdopen`, so closing the stream does not close the grant) and
`stdio.close(f: Stream consumed) -> void`, because a C stream cannot be made through a signature
alone.

### 11.3 What is refused, and why

- **Variadic functions** (`printw`, `mvprintw`). A format string from agent data is the classic
  format-string vulnerability, and the declarations cannot type varargs.
- **Callbacks into agent code.** They would run agent code synchronously, outside a turn, which V6
  and C2 forbid. libffi closures are therefore never created, which is also why `memfd_create` is
  refused (§8.3). A library that needs a callback needs a module-native skill whose callback
  enqueues a percept.
- **Structs by value, and raw pointers.** Deferred. Handles cover the opaque case.
- **Writing data symbols.** Reading a global (`global stdscr: Screen`) is allowed. Writing one is
  not.

### 11.4 Sequences

Under N2 and N3 every foreign call is an effect, and each costs a turn. A screen is a dozen calls.
The module therefore declares **sequences**: typed effects whose body is a list of named steps run
on one library thread. A sequence is the agent-visible unit of foreign work. Each sequence of a
library becomes an `effect def … within` of that library's capacity, so the checker types every call
site.

```text
sequence draw_greeting(t: Terminal, y: Int, x: Int, text: Str) -> Str  within 5s
  let outf := stdio.stream(t, "w")
  let inf  := stdio.stream(t, "r")
  let sp   := newterm(t.term, outf, inf)
  do mvaddstr(y, x, text)
  do refresh()
  let back := mvinnstr(y, x, len(text))
  result back.s
  finally endwin()
  finally delscreen(sp)
  finally stdio.close(inf)
  finally stdio.close(outf)
```

The grammar (in C++ `vagent_native.hpp`, the unlabelled step builder is `seq.step(call)` because
`do` is a C++ keyword, §6.4):

```text
sequence  ::= 'sequence' name '(' params ')' '->' type 'within' duration step* result? cleanup*
step      ::= 'let' name ':=' call | 'do' call
call      ::= fn_name '(' [arg {',' arg}] ')'
arg       ::= literal | name | name '.' field | 'len' '(' name ')'
result    ::= 'result' arg
cleanup   ::= 'finally' call
```

The rules:

1. **Arguments are positional, a call omits its `cstring_out` parameters, and `consumed` handles are
   linear.** Out-parameters are skipped during positional argument-to-parameter matching and the
   runner supplies their buffers, so `mvinnstr(y, x, len(text))` binds `y`, `x` and `n` in the
   four-parameter declaration of §11.2. A `name` is a sequence parameter or an earlier `let`.
   `len(x)` is the byte length of a `Str` and is the only computed expression. Every call is
   type-checked against its declaration when the sequence is sealed. Seal-time linearity validation
   also enforces that a handle variable passed to a `consumed` parameter never appears more than
   once in the same call's argument list and is never referenced in any subsequent `let`, `do`, or
   `result` step (failing seal with `ConsumedHandleReused`).
1. **Step results.** A `let` binds the function's return value, or, when the function has
   `cstring_out` parameters, a record of the return value as `ret` plus each out-parameter by name
   (`back.s`). A function may not name an out-parameter `ret`.
1. **Failure.** A step whose `fail_if` matches, or whose marshalling fails, ends the steps. The
   effect fails with the matching cause of §10.3. The runner posts
   `NativeCallFailed{method, step: Int, ret: Int, errno: Int}` as a percept, where `step` is the
   `let`/`do` step's ordinal from 1, so an agent that cares can handle it.
1. **`result`** is evaluated after the last step and before any cleanup, so it may read a handle
   that a cleanup will destroy. Its type must be the sequence's declared result.
1. **Cleanup.** `finally` calls run in written order, after `result` or after a failure. A cleanup
   runs only if every `let` name it mentions was bound and has not already been consumed by an
   earlier succeeded step. A cleanup that mentions none, such as `endwin()`, runs whenever cleanup
   runs; `endwin` before `newterm` fails harmlessly and is logged. A failing cleanup is logged and
   does not change the effect's outcome. A handle the sequence created, and that neither a cleanup
   consumed nor `result` returned, is then destroyed by its handle type's `destroy`, so a sequence
   cannot leak one.
1. **Atomicity.** A sequence is not atomic: the world keeps what the earlier steps did. A sequence
   of separate effects would be no different.

### 11.5 Thread affinity and contention

Each library declares `affinity`. `one_thread`, which covers ncurses and most terminal and GUI
toolkits, runs every call for that library on one host thread, in arrival order. Its queue is
bounded by `vagent.native.queue_depth`, and an effect beyond the bound fails `busy` at once. `any`
runs calls on a pool of `vagent.native.pool_threads`. An effect's `within` runs from staging, so
time spent queued counts against it. Contention is per host, which means per agent (N11).

### 11.6 Readiness

Input belongs on the library's own thread under `one_thread`, yet a keystroke should reach the agent
without polling. The host therefore watches, and never reads. `watch_terminal(t)` arms the
descriptor. The host then `poll`s it when the library thread is idle, and when it becomes readable
posts one `TerminalReadable{token}` percept and disarms.

A handler cannot use the reading effect's result: an effect staged from a handler has no intention
to resume, so its value is dropped (`agent_executor.cpp` `finishEffect`). The handler therefore
adopts a goal, and that goal's plan reads and re-arms:

```text
sequence read_key(t: Terminal) -> Opt<Int>  within 1s
  let inf := stdio.stream(t, "r")
  let key := fgetc(inf)
  result key
  finally stdio.close(inf)
```

```vagent
predicate last_key(code: Int)
goal Listen() perform
goal ReadKey() perform

agent Keyboard {
  uses Terminals
  uses Keys
  install HostTerminals()
  install KeysFfi()
  var screen: Opt<Terminal> := none
  on Initialize { adopt Listen() }
  plan listen for Listen {
    let t := open_terminal(24, 80)
    hold_terminal(t)
    screen := t
    watch_terminal(t)
  }
  on TerminalReadable { adopt ReadKey() }
  plan read for ReadKey {
    let t := screen
    let key := read_key(t)
    if key then revise last_key(key)
    watch_terminal(t)
  }
}
```

`Keys` is the capacity of the `libc` library line of §11.2. The descriptor was readable, so `fgetc`
returns at once, and `none_if eq -1` turns end-of-file into `none`. `hold_terminal` keeps the grant
after `Listen`'s intention ends (N13), so each `ReadKey` intention finds it in `screen`. Reading
`screen` fails the plan if it is `none`, as reading any empty `Opt` does. One percept per arming
means a slow agent cannot be flooded.

## 12. Lifecycle, failure, checkpoint and migration

### 12.1 Native host death

The control plane learns of a death through end-of-file on the control socket or a child status. It
may also cause one: by a protocol violation (§9.3), or because a foreign call ran past its effect's
deadline plus `vagent.native.call_grace`. A blocked C call cannot be cancelled from outside, so
killing the process is the only way to bound it. Then:

- Every in-flight and queued effect of that host fails `native_crashed` or `native_killed`.
- Every grant of that agent is revoked with cause `host_lost`, and the owner's terminal is restored
  (§10.4).
- The incarnation is retired. Its tokens are stale from then on.
- The agent's arena is untouched, because it lives in the control plane (N2).
- The next native effect starts a new host. Restarts back off exponentially from
  `vagent.native.restart_backoff`, doubling up to `vagent.native.restart_backoff_max`. After
  `vagent.native.max_failed_starts` consecutive deaths, native effects fail `native_unavailable` at
  once until the maximum backoff has elapsed, and then one more start is allowed. A successful
  effect after a sustained uptime of `vagent.native.restart_backoff_max` resets the count.

If the control plane itself dies, each host dies too, through its platform's parent-death mechanism
(`PR_SET_PDEATHSIG` on Linux, `PROC_PDEATHSIG_CTL` on FreeBSD, `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`
on Windows, or `_exit(1)` on `fd 3` EOF / `kqueue` `EVFILT_PROC` on OpenBSD and macOS, §§8.1–8.2)
even if a library thread is blocked. The ptys close with the control plane. The next start of the
node sweeps the staging directories of earlier epochs (§7.3).

### 12.2 Incarnations

An incarnation is `(node, serial)`. `node` is the executor's `author:device:epoch` name, and
`serial` is a counter within it. Where the host reserves the epoch with `reserveNodeId`
(`vortex/mesh_epoch.hpp:17-24`), which P20.8 makes `vagent run` do, two incarnations never share a
name across restarts or across nodes. Where it does not, as in a test's `LocalHost` with a fixed
epoch, incarnations are unique only within the process. Tokens stay unforgeable either way, because
they are 128 random bits checked against the pid they were issued to.

### 12.3 Checkpoint, resume and migration

A checkpoint changes nothing for the live agent. `AgentExecutor::checkpoint`
(`agent_executor.cpp:4770-4836`) is a snapshot that leaves the agent running, with its host and its
grants. It already refuses while an effect is in flight or a host delivery is pending (C10).

A resumed copy has a new pid (`resume` mints one), and a resume on the same node needs a newer epoch
(`WrongEpoch`, `mesh_actor.cpp:241-243`, `:322-324`). Any `Terminal` the copy finds in its state
therefore names a token issued to another pid, and its first use fails `stale`. No revocation is
needed for that. A migration is a checkpoint, a resume elsewhere, and a stop of the original. The
stop ends the original's grants with `agent_ended`.

Migration moves the agent's state, not its native host. The target node must hold the same
`programRef`, which for a single-target `.vnprog` means the same module bytes and so the same target
triple, and it must have a sandbox backend (N7). Otherwise `resume` refuses by name. Companion
specification [vagent-cluster-packaging.md](vagent-cluster-packaging.md) (§§4–8) extends this to
multi-target `.vnpkg` bundles (where one logical `programRef` binds per-triple module and bundled
library digests) and non-blocking on-demand fetch over `MeshSwarmTransport`.

### 12.4 What tells the supervisor

`AgentExecutor` offers turn and intention observers only (`agent_executor.hpp:347-351`, `:410-413`).
P20.6 adds a lifecycle observer with three signals. Intentions end inside the compiled graph, not in
C++, so `intentionEnded` has to be derived. After every turn the adapter already walks every
intention cell to rebuild the ready list and deadline heap (step 11, `deriveSchedule`,
`agent_executor.cpp:2187`, `:4038-4073`). P20.6 caches the set of live top-level intention ids on
`State` and reports each id that has disappeared since the previous walk (making between-turn
inspect calls to `deriveSchedule` idempotent). The cost is a set difference over live intentions per
turn, and no new walk.

| Signal                           | The supervisor and broker                                                                                                                                                                                                                                                                 |
| -------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `stopped(pid, cause)`            | Revoke every grant (`agent_ended`), shut the host down, remove its staging directory and broker rows. Covers stop, terminate, lease end and fatal initialisation                                                                                                                          |
| `intentionEnded(pid, intention)` | Revoke the grants that intention requested and did not hold (`intention_ended`, N13)                                                                                                                                                                                                      |
| `upgraded(pid, oldRef, newRef)`  | Fired after `AgentExecutor::upgrade`'s quiescence check succeeds. Compare module digest and binding manifest. If both are equal, keep host and grants. Otherwise revoke every grant (`program_changed`), restart the host from the new module, and decide later requests against `newRef` |

### 12.5 What is created, what bounds it, what reaps it

| Thing          | Created by                     | Bounded by                              | Reaped on                                                                          |
| -------------- | ------------------------------ | --------------------------------------- | ---------------------------------------------------------------------------------- |
| native host    | first native effect, or launch | `vagent.native.max_hosts`               | `stopped`, death, control-plane death, node shutdown                               |
| staging dir    | host start                     | one per host                            | host exit; sweep of earlier epochs at start                                        |
| grant          | `open_terminal`                | `vagent.native.max_grants` per agent    | `close_terminal`, intention end unless held, owner, `stopped`, host death, upgrade |
| handle         | a sequence step                | `vagent.native.max_handles` per host    | the sequence's cleanup or the type's `destroy`; host death                         |
| queued effect  | staging                        | `vagent.native.queue_depth` per library | completion, cancel, deadline, host death                                           |
| readiness      | `watch_terminal`               | one armed per grant                     | the percept, revocation (which wakes and disarms any blocked `poll`)               |
| log output     | the host's stderr              | `vagent.native.log_bytes_per_second`    | dropped beyond the rate, with a count                                              |
| ledger records | each event                     | the ledger store's segment growth       | not reaped; it is history (NQ5)                                                    |

## 13. The first test: ncurses through libffi

§13 cannot run until Q1 has landed (§1.1).

### 13.1 The module

`tests/samples/vagent/native/curses_hello.cpp`, built by the Makefile into
`build/tests/curses_hello.so`, uses `vagent_native.hpp`. It declares the authority, the bindings of
§11.2, the sequence of §11.4, and this agent. Its program, written here as the equivalent vagent
source without the declarations the replay synthesizes, is:

```vagent
event Greeted { text: Str }
goal Greet() perform

agent Greeter {
  uses Terminals
  uses Curses
  install HostTerminals()
  install CursesFfi()
  on Initialize { adopt Greet() }
  plan greet for Greet {
    let t := open_terminal(24, 80)
    let line := draw_greeting(t, 2, 4, "hello from a native agent")
    emit Greeted{text: line}
    close_terminal(t)
  }
}
```

`Curses` and `CursesFfi` are the capacity and skill the `ncurses` library line names, and `Curses`
has `effect def draw_greeting(t: Terminal, y: Int, x: Int, text: Str): Str within 5s`. If
`draw_greeting` fails, the plan fails and `GoalFailed` is raised. The sequence's cleanups have
already run, and the grant is revoked when the `Greet` intention ends (N13). The part of the module
that builds the plan on `ag` (`b.agent("Greeter")`), through the C++ layer:

```cpp
auto plan = ag.plan("greet").forGoal("Greet");
plan.let("t", b.call("open_terminal", {b.lit(24), b.lit(80)}));
plan.let("line", b.call("draw_greeting",
                        {b.name("t"), b.lit(2), b.lit(4),
                         b.lit("hello from a native agent")}));
plan.stmt(b.emit(b.record("Greeted", {{"text", b.name("line")}})));
plan.stmt(b.call("close_terminal", {b.name("t")}));
```

A second variant, `curses_keep.cpp`, has a plan that opens a terminal, calls `hold_terminal(t)`,
stores it in `var screen: Opt<Terminal> := none` and does not close it, as `Listen` does in §11.6.
That gives §13.2 step 8 a token that is still valid on the original and stale on a copy.

### 13.2 What the test does

`VagentNativeAgentTest`, under `tests/xuzz/`, runs from a new `make test/vagent-native` target and
from `make test`. Everything is headless. There is no window and no real terminal: the only terminal
is a pty whose master the harness holds.

1. **Seal.** `vagent native seal build/tests/curses_hello.so`, twice. Both succeed, the two
   `.vnprog` files are byte-identical, and the refs agree. Sealing `curses_hello_c.so`, the same
   program written against the C header alone (§6.4), yields a byte-identical op log.
1. **Same program as source.** Replay the op log into a `Unit` and compare it with `parseUnit` of
   the equivalent source in §13.1 plus the synthesized declarations, using a test-only structural
   comparator over `ast.hpp` that ignores source locations. The two are equal. This is the test that
   the native ops build what the language builds.
1. **Load.** `CompiledProgram::fromUnit`, then a `LocalHost` with a manual clock and `HostBindings`
   holding a `RemoteNativeSkill` for `HostTerminals` and `CursesFfi`.
1. **Policy.** `approved`, with the ledger holding an approval of `terminal` for this `programRef`.
   The harness's broker hook creates the pty pair and keeps the master.
1. **Run.** Launch `Greeter` and pump until idle, under a turn bound.
1. **The effect.** The harness reads the master. The stream contains `hello from a native agent` and
   a cursor-addressing sequence for row 2, column 4 under `xterm-256color`.
1. **The agent.** The `HostSink` received `Greeted{text: "hello from a native agent"}`. That text
   was read back from ncurses' own screen by `mvinnstr` for exactly `len(text)` characters, so the
   read path is tested as well as the write.
1. **Stale after resume** (`curses_keep`). The agent holds `screen` and is quiescent. Checkpoint it,
   resume it in a second `LocalHost` with a higher epoch, and have the copy call `draw_greeting`
   with `screen`. It fails `stale`. The original's terminal still works.
1. **The ledger.** For `curses_hello`, one decision (`granted`) and one revocation (`released`). For
   `curses_keep` after the original is stopped, one decision (`granted`) and one revocation
   (`agent_ended`).

### 13.3 The terminal had to be requested

With `grant_policy = deny`, `open_terminal` fails `refused`, the plan fails, and the master receives
no byte. A module variant that builds `draw_greeting(Terminal{token: "00…", …}, …)` from a literal
record fails `stale`, because no such token exists for that pid. A variant that opens `/dev/tty`
itself is the first escape probe below.

### 13.4 Escape probes and gates

`tests/samples/vagent/native/escape_probe.cpp` is a module-native skill (`vagent_native_skills`,
§6.1). Each method attempts one forbidden act and returns `errno` (or the OS status on Windows).
Every supported backend (§8.2) runs the invariant suite below:

| Probe                                                                                           | Expected                     | Linux enforcement                         | FreeBSD / OpenBSD / macOS / Windows enforcement                                           |
| ----------------------------------------------------------------------------------------------- | ---------------------------- | ----------------------------------------- | ----------------------------------------------------------------------------------------- |
| `open("/dev/tty", O_RDWR)` (or `CONIN$`)                                                        | `EACCES` / `ECAPMODE` / deny | Landlock                                  | `cap_enter` / `unveil` / Seatbelt `file-read*` / AppContainer                             |
| `open` of a new file under `$XDG_DATA_HOME` to write                                            | `EACCES` / `ECAPMODE` / deny | Landlock                                  | `cap_enter` / `unveil` / Seatbelt `file-write*` / AppContainer                            |
| `open` of the staged module to write                                                            | `EACCES` / `ECAPMODE` / deny | Landlock                                  | `cap_enter` / `unveil` (`"rx"`) / Seatbelt / read-only staging ACL                        |
| `open(getenv("HOME") + "/.profile")` to read                                                    | `EACCES` / `ECAPMODE` / deny | Landlock                                  | `cap_enter` / `unveil` / Seatbelt / AppContainer                                          |
| `socket(AF_INET, SOCK_STREAM, 0)`                                                               | `EPERM` / `ECAPMODE` / kill  | seccomp                                   | `cap_enter` / `pledge` (`SIGABRT`) / Seatbelt `network*` / AppContainer zero capabilities |
| `socket(AF_UNIX, SOCK_STREAM, 0)`                                                               | `EPERM` / `ECAPMODE` / kill  | seccomp                                   | `cap_enter` / `pledge` (`SIGABRT`) / Seatbelt `network*` / AppContainer                   |
| `execve("/bin/sh", …)` (or `CreateProcessW`)                                                    | `EPERM` / `ECAPMODE` / kill  | seccomp                                   | `cap_enter` / `pledge` / Seatbelt `process-exec*` / `JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 1` |
| `fork()`                                                                                        | `EPERM` / `ECAPMODE` / kill  | seccomp (`clone` without `CLONE_THREAD`)  | `cap_enter` / `pledge` / Seatbelt `process-fork` / `JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 1`  |
| raw `clone3` without `CLONE_THREAD`                                                             | `ENOSYS`                     | seccomp                                   | Linux-specific                                                                            |
| `pthread_create`                                                                                | succeeds                     | (fallback to `clone`)                     | allowed on all platforms                                                                  |
| `kill(getppid(), SIGTERM)`, `kill(0, …)`, `kill(-1, …)`                                         | `EPERM` / `ECAPMODE` / kill  | seccomp                                   | `cap_enter` / `pledge` / Seatbelt `signal` / UIPI + Job Object                            |
| `pidfd_open(getppid(), 0)`                                                                      | `EPERM`                      | seccomp                                   | Linux-specific                                                                            |
| `ptrace(PTRACE_ATTACH, getppid())` / `OpenProcess`                                              | `EPERM` / `ECAPMODE` / deny  | seccomp                                   | `cap_enter` / `pledge` / Seatbelt / Restricted Token + AppContainer                       |
| `ioctl(grant, TIOCSTI, "x")`                                                                    | `EPERM` / `ENOTCAPABLE`      | seccomp (`SCMP_CMP_MASKED_EQ` 32-bit cmd) | `cap_ioctls_limit` / OpenBSD kernel / Seatbelt / ConPTY                                   |
| `syscall(SYS_ioctl, grant, (1ULL << 32) \| TIOCSTI, "x")`                                       | `EPERM`                      | seccomp 32-bit masked `args[1]` (§8.3)    | 64-bit high-bit truncation bypass check                                                   |
| `mprotect(page, 4096, PROT_READ \| PROT_EXEC)` after step 8                                     | `EACCES` / `EPERM` / deny    | `PR_SET_MDWE` + phase-2 seccomp (§8.1)    | Phase-2 W^X enforcement / `pledge` / `ACG` `ProhibitDynamicCode`                          |
| `mmap(nullptr, 4096, PROT_READ \| PROT_EXEC, MAP_PRIVATE \| MAP_ANONYMOUS, -1, 0)` after step 8 | `EACCES` / `EPERM` / deny    | `PR_SET_MDWE` + phase-2 seccomp (§8.1)    | Phase-2 W^X enforcement / `pledge` / `ACG` `ProhibitDynamicCode`                          |
| `mprotect` / `mmap` with `PROT_WRITE` on `fd 4` (`shm_a`)                                       | `EACCES` / `EPERM` / deny    | `F_SEAL_FUTURE_WRITE` / RO mapping (§9.1) | `F_SEAL_WRITE` / RO dup fd / `FILE_MAP_READ`-only section handle                          |
| `ftruncate` of `fd 4` or `fd 5`                                                                 | `EPERM` / `EINVAL`           | memfd `F_SEAL_SHRINK \| F_SEAL_GROW`      | `SHM_ANON` seal / fixed section size                                                      |
| `io_uring_setup(…)`                                                                             | `EPERM`                      | seccomp                                   | Linux-specific                                                                            |
| `memfd_create`                                                                                  | `EPERM`                      | seccomp                                   | Linux/FreeBSD-specific                                                                    |
| `getpid` through `int 0x80` (x86-64)                                                            | host killed                  | seccomp architecture check                | Linux x86-64-specific                                                                     |

Each probe except the architecture kill probe is also run unsandboxed in a child of the test, where
it must succeed or fail with a different error. Otherwise the test would pass on a machine where the
act was impossible anyway, and prove nothing.

Gates:

- **N12.** A module-native skill completes with a value of the wrong type. The effect fails
  `native_protocol`, the host is killed, and the agent survives.
- **§9.1–9.3.** A module-native skill writes a frame with an out-of-range ring index, sends an
  unexpected descriptor on `fd 3`, or sends data on `fd 3` other than a valid `GrantAck`. The host
  is killed. A burst of 600 frames (`> 2 * drain_per_wake`) written into Ring B drains completely
  via doorbell re-arming without stranding frames.
- **§11.2, §11.4.** Calling a function with `cstring_out(n)` where `n = -1` or `n = INT_MIN` fails
  `ffi_range` before allocation. Sealing a sequence that passes the same handle variable twice to a
  call with a `consumed` parameter, or uses a `consumed` handle in a subsequent step, fails at seal
  time with `ConsumedHandleReused`.
- **§12.1.** A skill calls `abort()`. In-flight effects fail `native_crashed`, the grant is revoked
  with `host_lost`, the next effect starts a new incarnation, and old tokens are stale. A skill that
  sleeps past its deadline plus grace is killed (`native_killed`). A module that crashes at start-up
  reaches `native_unavailable` after `max_failed_starts`.
- **Control-plane death.** The test runs a control plane in a child process and kills it with
  `SIGKILL` while a host is blocked in a call. The host is gone, and a restarted control plane with
  a higher epoch sweeps the old staging directory.
- **§10.4.** A stand-in owner terminal (a second pty) is attached through the relay, the host writes
  `CSI 6 n`, `CSI c`, `CSI 21 t` and `OSC 52;c;... BEL` under `strict`, and the test asserts that
  zero query or clipboard bytes reach the stand-in. Then the host is killed after entering the
  alternate screen, and the stand-in receives the restore sequences and has its `termios` back.
- **§10.5.** After an approval, two decisions against it and a revocation, both decisions still
  resolve their quoted spans and the author's permascroll is byte-identical.
- **§7.3.** The original module file is replaced after hashing and before the host starts. The host
  loads the staged copy, and the test asserts that by a symbol only the original exports.
- **N13.** A plan whose `draw_greeting` fails leaves no grant behind: the ledger records
  `intention_ended`, and the master reads end-of-file. A held grant (`curses_keep`) survives the end
  of the intention that opened it.
- **§11.6.** The harness writes a key into the master. The agent's `last_key` belief holds it.
- **V5.** A plan statement after `draw_greeting` is made to fail hard. `draw_greeting` was staged in
  the same turn, so it never reaches the host, and the master reads nothing.

### 13.5 Where the sandbox is unavailable

On a kernel without its platform's required sandbox primitives (e.g., Linux without Landlock ABI 4,
or with Landlock disabled at boot), the same test asserts the refusal: launch fails with
`NativeAgentError::NoSandbox`, naming the backend and the missing primitive or ABI version. It does
not skip. A skipped sandbox test would be a passing check of nothing, and N7 makes the refusal the
specified behaviour there. On Linux ABI 4 and 5 kernels every probe in the table holds, because none
depends on ABI 6.

## 14. New code and proposed changes

### 14.1 New code, and why it is C++

| Unit                                                                                                                            | Where                               | Why C++ rather than Vortex                                                                                                                                                                      |
| ------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `node_kinds.def`, `op_log.{hpp,cpp}`                                                                                            | `apps/common/xanadu/vagent/native/` | It is the boundary format between untrusted bytes and the compiler, decoded before any program exists to run Vortex in                                                                          |
| `replay.{hpp,cpp}` (op log → `Unit` and manifest)                                                                               | same                                | It produces the compiler's C++ input; the compiler is C++                                                                                                                                       |
| `remote_skill.{hpp,cpp}`                                                                                                        | same                                | It implements `NativeSkill`, a C++ interface the adapter calls between turns                                                                                                                    |
| `supervisor.{hpp,cpp}`, `ipc_{posix,windows}.cpp`                                                                               | same, sandbox-gated (P20.5)         | Process spawning, child status, staging files, shared-memory rings and doorbells are OS interfacing                                                                                             |
| `broker.{hpp,cpp}`, `pty_{posix,windows}.cpp`                                                                                   | same, sandbox-gated                 | pty/ConPTY creation, `termios` and handle passing are OS interfacing. Its records are cells (§10.5). Its policy is three cases, decided outside any agent's turn, with authority no agent holds |
| `native_unavailable.cpp`                                                                                                        | same, unsupported builds            | A refusing implementation of the supervisor interface, so launch fails `NoSandbox` by name                                                                                                      |
| `vagent_native.h` (C ABI), `vagent_native.hpp` (C++ wrapper), `vagent-native.pc`                                                | `apps/vagent-native-host/`          | The interface foreign modules build against: C for adoption by other languages, C++ for ease of use (§6.4), installed under `<prefix>/include/vagent/` with a `pkg-config` file                 |
| `main.cpp`, `recorder.cpp`, `loader.cpp`, `ffi_runner.cpp`, `sequence.cpp`, `sandbox_{linux,freebsd,openbsd,macos,windows}.cpp` | `apps/vagent-native-host/`          | OS sandboxing, dynamic loading and libffi: OS and ABI interfacing in the sandboxed process, where no Vortex runtime exists by design (N2)                                                       |

`op_log`, `replay` and `remote_skill` are portable and dependency-free, and stay in the
`apps/common/xanadu` glob (`Makefile:633`) on every platform. `supervisor`, `ipc_*`, `broker` and
`pty_*` are built when `GLEDITOR_ENABLE_NATIVE_AGENTS=1` (default on supported platforms when
`libffi`—and `libseccomp` on Linux—are found); otherwise P20.5 filters them out of
`COMMON_XANADU_SRCS` and builds `native_unavailable.cpp` in their place. `libffi` (and `libseccomp`
on Linux) are linked only into `apps/vagent-native-host/`, which the Makefile builds as its own
program. A pty is an OS object, not a display, so the engine stays free of graphics devices.

### 14.2 Proposed changes to existing code

All eight proposals below (**P20.1–P20.8**) are **approved by the owner (2026-10-31)**.

| No.   | Status   | Change                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        | Where                                                                       | Why it cannot live in new code                                                               | Without it                                                                                                                                           |
| ----- | -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| P20.1 | Approved | `EffectRequest` gains `agent` (pid and name) and `intention` (the staging intention's resolved top-level ancestor id, absent for a handler), and `HostBindings::start` takes them. `AgentExecutor` passes them at `agent_executor.cpp:3895`                                                                                                                                                                                                                                                                   | `vagent/runtime/host_bindings.{hpp,cpp}`, `agent_executor.cpp`, their tests | Only the executor knows which agent and intention staged an effect                           | One `AgentExecutor` and `HostBindings` per native agent, or skill names mangled per agent, which change the program; and no intention-scoped grants  |
| P20.2 | Approved | `CompiledProgram::fromUnit(Unit, name, ref)` (taking `Unit` by value, `ast.hpp:26`): the check, compile, promote and remap half of `fromSource`, which becomes parse plus `fromUnit`                                                                                                                                                                                                                                                                                                                          | `vagent/runtime/program.{hpp,cpp}`                                          | `fromSource` is the only constructor and takes text                                          | Native programs would be pretty-printed to source and reparsed: a second printer to keep exact, and no op ordinals in diagnostics                    |
| P20.3 | Approved | `defaultSettingSpecs()` entries for §14.3, with schema-page and notes-page entries; a `NativeSettings` struct in `LocalHost::Options` that the host fills, as it fills `AgentSettings`                                                                                                                                                                                                                                                                                                                        | `system_docs.cpp`, `vagent/runtime/program.hpp`                             | Configuration lives in system xanadocs (AGENTS.md); the host is what reads them today        | Constants in the supervisor and broker                                                                                                               |
| P20.4 | Approved | Record the `vagent.native` logging category                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | `design/logging-migration.md`                                               | Categories are named in the migration plan, as P10 did for `vortex.agent`                    | Logging under an unrecorded category                                                                                                                 |
| P20.5 | Approved | Build: the `vagent-native-host` program; per-OS sandbox and IPC backends (§14.1); probes for `libffi` (and `libseccomp` on Linux) setting `GLEDITOR_ENABLE_NATIVE_AGENTS`; the `filter-out` of the gated files and `native_unavailable.cpp` substitute; generation of `vagent_native.h`'s constants from `node_kinds.def`; installation of `vagent_native.h`, `vagent_native.hpp` and `vagent-native.pc`; the test modules; `test/vagent-native`; the host in `TEST_PROGRAMS` (`Makefile:1334`); fuzz targets | `Makefile`, `debian/`, `packaging/`                                         | Programs, tests, optional dependencies and per-platform source selection need explicit rules | No host, no tests, a build break on platforms without the sandbox dependencies, and no fuzzing of the new input surfaces                             |
| P20.6 | Approved | A lifecycle observer on `AgentExecutor`: `stopped(pid, cause)`, `intentionEnded(pid, intention)`, derived in `deriveSchedule` by diffing live top-level intention ids, and `upgraded(pid, oldRef, newRef)` (§12.4); and a `localNode()` accessor for the node name the incarnation uses                                                                                                                                                                                                                       | `vagent/runtime/agent_executor.{hpp,cpp}`                                   | Only the executor sees these transitions; it exposes turn and intention observers only       | Hosts, ptys and staging directories leak after every stop, grants outlive their intentions and agents, and an upgrade runs new sequences on old code |
| P20.7 | Approved | Extract the `posix_spawn`, pipe and descriptor-action sequence of `provenance.cpp:240-254` into a shared helper, used by GnuPG invocation and the POSIX supervisor                                                                                                                                                                                                                                                                                                                                            | `apps/common/xanadu/provenance.cpp`, a new `process_spawn.{hpp,cpp}`        | The sequence is function-local today                                                         | A second copy of the descriptor-hygiene code, which is exactly the code a sandbox must get right                                                     |
| P20.8 | Approved | Add CLI subcommands and flags in `apps/vagent/main.cpp` (syntax below), an interactive approval prompt on `/dev/tty` for `ask`, `reserveNodeId` for the node's epoch in `vagent run`, and automatic `nativeSettings.enabled = true` when `vagent run` is invoked on a `.vnprog` file                                                                                                                                                                                                                          | `apps/vagent/main.cpp`                                                      | It is the existing command's surface                                                         | Native agents only through the test harness, and incarnations unique only within one process                                                         |

**CLI surface added by P20.8:**

```text
vagent native seal <module-path> [-o <output.vnprog>]
vagent native approve <file.vnprog | programRef> --kind <kind> [--ledger <dir>]
vagent native withdraw <file.vnprog | programRef> --kind <kind> [--ledger <dir>]
vagent run <file.vagent | file.vnprog> [--module <path>]
           [--grant-policy <deny|ask|approved>] [--ledger <dir>]
```

- `vagent native seal` runs `vagent-native-host --seal` twice (§5), checks that the two op logs are
  byte-identical, replays the op log into a `Unit`, type-checks and compiles it, and writes the
  bencoded `.vnprog` (§7.1). If `-o` is omitted, it replaces the `.so` / `.dylib` / `.dll` suffix of
  `<module-path>` with `.vnprog`. It prints `<programRef>  <output.vnprog>` on stdout.
- `vagent native approve` and `vagent native withdraw` accept either a `.vnprog` file path (from
  which `programRef` is computed) or a literal `programRef` string, and append an `approve` or
  `withdraw` event to the authority ledger (§10.5) at `--ledger` (defaulting to `./authorities` in
  the deployment directory, or `$XDG_DATA_HOME/vagent/authorities` when run standalone).
- When `vagent run` is given a `.vnprog` file, it sets `options.nativeSettings.enabled = true` for
  that invocation (whereas multi-tenant daemon nodes continue to default `vagent.native.enabled` to
  `false` unless enabled in system settings), resolves the shared library via `--module <path>` or
  `<vnprog-dir>/<module_filename>` (§7.1), reserves a node epoch via `reserveNodeId`, and applies
  `--grant-policy` and `--ledger` overrides if supplied.

Q1 and Q2 (§1.1) also change existing code: `compiler.cpp`, `ast.hpp`, the parser, the checker and
`vagent_skill_test.cpp`. They are listed there rather than here because the vagent specification
already defines them and the vagent implementation plan owes them.

### 14.3 Settings

| Setting                              | Default                                           | Bounds what                                                                                 |
| ------------------------------------ | ------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| `vagent.native.enabled`              | false (`true` for `vagent run <file.vnprog>`)     | whether the node launches native agents at all                                              |
| `vagent.native.start`                | `lazy`                                            | start the host at the first native effect, or `eager`                                       |
| `vagent.native.staging_dir`          | `$XDG_RUNTIME_DIR/vagent-native` (or OS fallback) | root directory for staged modules (§7.3)                                                    |
| `vagent.native.max_hosts`            | 16                                                | concurrent native hosts per node                                                            |
| `vagent.native.max_grants`           | 8                                                 | live grants per agent                                                                       |
| `vagent.native.grant_policy`         | `ask`                                             | §10.2                                                                                       |
| `vagent.native.terminal_filter`      | `strict`                                          | §10.4                                                                                       |
| `vagent.native.library_paths`        | the loader's paths, locale, terminfo              | sandbox read-and-execute rules (§§8.2–8.3)                                                  |
| `vagent.native.memory_limit`         | 512 MiB                                           | address-space / job memory limit of each host                                               |
| `vagent.native.max_fds`              | 64                                                | descriptor limit of each host                                                               |
| `vagent.native.ring_bytes`           | 1 MiB                                             | each shared ring (`data_a` and `data_b`, §9.1)                                              |
| `vagent.native.frame_bytes`          | 64 KiB                                            | one frame; above the 32 KiB an effect value can reach (§9.1)                                |
| `vagent.native.drain_per_wake`       | 256                                               | frames handled per doorbell before re-arming if `head_b != tail_b` (§9.3)                   |
| `vagent.native.queue_depth`          | 64                                                | queued effects per `one_thread` library                                                     |
| `vagent.native.pool_threads`         | 4                                                 | threads for `any` libraries                                                                 |
| `vagent.native.max_handles`          | 4096                                              | live handles per host                                                                       |
| `vagent.native.max_out_bytes`        | 4096                                              | one `cstring_out` buffer                                                                    |
| `vagent.native.call_grace`           | 2s                                                | past an effect's deadline before its host is killed                                         |
| `vagent.native.restart_backoff`      | 1s                                                | first restart delay; doubles                                                                |
| `vagent.native.restart_backoff_max`  | 60s                                               | the doubling's cap                                                                          |
| `vagent.native.max_failed_starts`    | 5                                                 | consecutive deaths without a successful effect                                              |
| `vagent.native.log_bytes_per_second` | 16 KiB                                            | host stderr forwarded to the log (drained continuously thereafter so the pipe never blocks) |
| `vagent.native.op_log_ops`           | 1,000,000                                         | ops in one artifact                                                                         |

None of these defaults is measured. Each is a starting point to replace with a measured value before
release: a compiled program's AST size for `op_log_ops`, host resident memory with ncurses loaded
for `memory_limit` and `max_hosts`, and the probe of §16 for the rings.

## 15. Refused alternatives

| Alternative                                                                     | Refused because                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| ------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Native ops that place opcodes and splice them into compiled scaffolding (v0.1)  | The compiler's emitter is one AST-driven class private to `compiler.cpp` (`:92`, `:411`), with no seam to splice into. An opcode-kind allowlist does not establish that a fragment can do nothing a vagent program cannot: computed jump targets, `Halt`, frame and `#Call`/`#Return` misuse, and fan-in on `d.spin`/`d.grab` all pass it. Deferred to NQ1 with those requirements                                                                                                                                                                                                          |
| Native ops as VM opcodes that call native code during a turn                    | A device act inside a turn breaks V6 and C2. It is also the generic native-call hook the runtime grounding says needs its own proposal and security contract                                                                                                                                                                                                                                                                                                                                                                                                                                |
| Pretty-print the op log to vagent source and parse it                           | A second printer to keep exact. Diagnostics would point at synthetic source instead of the op that made the fault (P20.2 is cheaper)                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| Run the agent's turns in the sandboxed process                                  | The arena would live beside untrusted code: a library bug could corrupt agent state or forge staged messages. Checkpoints would cross the boundary unsigned, since the sandbox holds no key. A crash would lose the agent, not only its effects                                                                                                                                                                                                                                                                                                                                             |
| The native host as a mesh peer over a socketpair `MeshHostTransport`            | `MeshHostTransport`'s surface is job announcement, content preparation, seeding and authenticated actor delivery (`vortex/mesh_host.hpp:13-44`). `receive` admits sends only from the owner identity, and a sandbox holds no device key. Effects are host-binding calls, not actor messages. The price of refusing is a second, much smaller process protocol                                                                                                                                                                                                                               |
| The sandbox compiles and promotes its own program                               | Promotion writes into a permascroll, which a write-blocked sandbox cannot do and should not hold (N5)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| One native host per program, shared by its agents                               | One agent's crash would take every agent's effects and handles, and grants would have to be partitioned inside one shared process                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| Bind `one_thread` libraries with `Contention::Queue` in `HostBindings`          | The queue is per skill name across the node, so it would serialise unrelated agents' processes behind each other (N11)                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| Pass the owner's real terminal descriptor                                       | No revocation, `TIOCSTI` into the owner's shell, unfiltered sequences to the emulator, and nothing to test headlessly (N9)                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| A single read-write shared-memory region for both rings                         | A compromised native host with `PROT_WRITE` over Ring A could overwrite in-flight `StartEffect` payloads or corrupt Ring A's header while the control plane writes. Separating `shm_a` (read-only in the host) from `shm_b` (read-write) eliminates the hazard at the page table (§9.1)                                                                                                                                                                                                                                                                                                     |
| Apache Avro, in-place FlatBuffers on `shm_b`, or bencode envelopes on the rings | `avro-cpp` pulls Boost, JSON schema parsers and compression codecs into both the control plane and sandbox, lacks BEP-3 canonical dict hashing for `.vnprog`, and heap-allocates on decode. Reading FlatBuffers in place from host-writable `shm_b` after `Verifier` exposes a shared-memory double-fetch / TOCTOU race if a second host thread mutates offsets before dereference. A 32-byte `FrameHeader` + zero-allocation `WireValueView` over a pre-allocated 64 KiB thread-local scratch copy gives zero heap allocations, zero external dependencies, and TOCTOU immunity (§9.2–9.3) |
| Widen the sandbox at run time                                                   | OS sandboxes only restrict further. Re-sandboxing means a new process and the loss of every handle                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| An unsandboxed fallback                                                         | A sandbox that silently is not there is worse than a refusal (N7)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| System library bytes in `programRef`                                            | Every node would name a different program (N6)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| A denylist seccomp filter                                                       | New calls arrive every kernel release, and a denylist admits each by default. io_uring is the standing example                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| Load the module by its original path                                            | The bytes that run could differ from the bytes hashed, so an approval for one build could be spent on another (§7.3)                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| A generic `batch([Step])` effect                                                | `Step`, `Arg` and a dynamic `Value` have no place in vagent's static types, and positional `$n` references break silently when steps move. Typed, named sequences are checked at seal (§11.4)                                                                                                                                                                                                                                                                                                                                                                                               |
| Approvals as a `programRef:kind` string setting                                 | Settings are scalars, so the pair would be a packed string parsed on every request. Approvals and decisions are structure in the ledger (§10.5)                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| Ledger text in the author's permascroll                                         | The author's permascroll is the published one; approval history and purposes would be published with it (§10.5)                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| A decision linked to its approval                                               | Many decisions per approval would be fan-in on one cell, evicting earlier links. A decision quotes the approval's spans instead (§10.5)                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| The binding manifest as cells in the program store                              | Its agent-visible half is already cells: the synthesized capacities, records and events are compiled like any declaration. Its C half (sonames, C types, `fail_if`) is consumed only by the host's libffi runner, outside any manifold, and would be read straight back out of cells to be sent there                                                                                                                                                                                                                                                                                       |
| Revoking grants on every checkpoint                                             | A checkpoint is a snapshot of a running agent; the live agent would lose its terminal for a backup. A resumed copy's tokens are already stale by the pid binding (§12.3)                                                                                                                                                                                                                                                                                                                                                                                                                    |
| A handler that reads a key through an effect                                    | An effect staged from a handler has no intention to deliver its result to, so the key is dropped. The handler adopts a goal instead (§11.6)                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| Variadic and callback-taking foreign functions in the first version             | Format strings from agent data, and synchronous re-entry into agent code outside a turn (§11.3)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |

## 16. Properties and costs

**Properties, with their conditions:**

- Everything §14 of the specification claims holds for a native agent: its program is checked and
  compiled by the same code, and its turns are the same adapter's, given Q1.
- A native program can express exactly what vagent source can (N4), shown per program by the §13.2
  equality test.
- Native code never runs in the node process, given that `libffi` and `libseccomp` are linked only
  into the host.
- Native code cannot write files, open sockets, execute programs, fork, signal other processes, or
  map new executable pages after module load, given the platform's §8.2/§8.3 sandbox and the §13.4
  probes passing on that host.
- A native host's death loses effects in flight and grants, never agent state, and leaves the
  owner's terminal restored. The control plane's death takes its hosts with it.
- A revoked terminal stays revoked whether or not the host cooperates. A grant outlives neither its
  intention nor its agent.

**Costs:**

- **A turn per foreign effect,** as for every effect, and an IPC round trip. Sequences amortise
  both, but not the world's lack of atomicity.
- **The round trip itself is unmeasured.** Before quoting a figure, a probe times 10,000 no-op
  sequences against 10,000 no-op in-process native effects through the same `AgentExecutor`.
- **A process per native agent,** with resident memory unmeasured.
- **A double build at seal** and replay at load. Compilation per agent is exactly a compiled
  program's.
- **Module staging:** one file copy per host start.
- **Ledger writes:** about 30 Structure operations per decision, 14 per revocation, 12 per approval
  event, in a private store and permascroll.
- **Relay copying and filtering** for a terminal shown to the owner.
- **Platform reach:** five OS backends specified with full feature parity (§8.2); each platform is
  enabled as its §13.4 probe suite lands.
- **Four new input surfaces** (op log, IPC frames, relay byte stream, module bytes), the first three
  with fuzz targets (P20.5).

## 17. Open questions

| No. | Question                                                                                     | Leaning                                                                                                                                                                                                                                                                        |
| --- | -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| NQ1 | Raw graph construction: ops that place opcodes inside bodies                                 | Needs a seam in `Gen` (a priced refactor) and a fragment verifier proven sufficient: no computed or out-of-fragment targets, no `Halt`, the `#Call`/`#Return` frame protocol, no fan-in on `d.spin`/`d.grab`, statement brackets enforced by construction. A separate proposal |
| NQ2 | Should a vagent source program carry a sandboxed native module for its `native skill`s?      | Yes. It is §§8–12 without §6; specified as the `"source+native"` package kind in [vagent-cluster-packaging.md](vagent-cluster-packaging.md) §5.1                                                                                                                               |
| NQ3 | Directory authorities (`files.read`, `files.write` beneath a directory descriptor)           | Landlock and Capsicum refuse opens beneath a passed descriptor unless the ruleset/capability rights allow them; design separately                                                                                                                                              |
| NQ4 | Module distribution by content address over the swarm, for migration                         | Reuse the mesh's program-content path, keyed by module digest and target triple; specified in [vagent-cluster-packaging.md](vagent-cluster-packaging.md) §§5–8 (P21.1–P21.8)                                                                                                   |
| NQ5 | Retention of the authority ledger                                                            | Keep everything; the bound is the store's own segment growth                                                                                                                                                                                                                   |
| NQ6 | Should `strict` admit additional terminfo sequences beyond the §10.4 closed allowlist?       | Expand only by audited additions verified against `xterm-256color` and `tmux`/`screen` terminfo entries and covered by the §13.4 filter gate                                                                                                                                   |
| NQ7 | Does reusing epochs and codec conventions satisfy "leverages Vortex's multiprocess support"? | The owner's call. The alternative is the refused `MeshHostTransport` peer, at the price §15 states                                                                                                                                                                             |

## Change history

**0.6 (2026-10-31).** Revised after specialist review round 5 (`security`, `ipc`, `user`); owner
approved `P20.1`–`P20.8`.

- **P20.1–P20.8 approved:** recorded in metadata header and §14.2.
- **§5.1, §§6.1–6.4, §7.1, §14.2 P20.8 (User journey & ABI completeness):** added a five-step
  end-to-end developer walkthrough (§5.1); specified `VN_API_VERSION`, `vagent_native_skills`
  (`vn_skill_entry`, `vn_effect_req`, `vn_complete_fn`) for module-native skills, the complete
  `VN_KIND_NATIVE_*` / `VN_SLOT_NATIVE_*` op-code table (§6.2), and C++ `Builder` methods for
  authority/library/handle/function/sequence construction (`seq.step`, §6.4); added
  `module_filename` to `.vnprog` (§7.1) and `--module` / automatic `nativeSettings.enabled = true`
  for `vagent run <file.vnprog>` (P20.8).
- **§§8.1–8.3, §10.4, §§11.2–11.4, §13.4 (Security hardening):** split sandbox lockdown into two
  phases so `PR_SET_MDWE` and phase-2 seccomp revoke `PROT_EXEC` on `mmap`/`mprotect` and deny
  `openat` after libraries load (§8.1); masked 64-bit `ioctl` `args[1]` with
  `SCMP_CMP_MASKED_EQ(1, 0xFFFFFFFFULL, cmd)` over a 7-command tty allowlist and restricted `fcntl`
  (§8.3); replaced the `strict` terminal filter with a closed CSI allowlist that drops status,
  device-attribute and XTWINOPS queries (§10.4); enforced `0 <= n <= max_out_bytes` on
  `cstring_out(n)` and seal-time linearity (`ConsumedHandleReused`) on `consumed` handles
  (§§11.2–11.4); added escape probes and gates for high-bit `ioctl`, post-load `PROT_EXEC`,
  read-only `shm_a`, `cstring_out` bounds, handle linearity, and query-escape stripping (§13.4).
- **§§8.1–8.2, §§9.1–9.3, §10.4, §14.1, §15 (IPC, zero-allocation framing & cross-platform
  parity):** split shared memory into read-only `shm_a` (`fd 4`) and read-write `shm_b` (`fd 5`)
  with `alignas(128)` cache-line-isolated atomics, a fixed 32-byte binary `FrameHeader`,
  `0xFFFFFFFFU` wrap-around markers, zero-allocation `WireValueView` (`decodeValueView`) over a
  pre-allocated 64 KiB thread-local scratch copy (defeating shared-memory double-fetch TOCTOU), and
  `std::atomic_ref` acquire/release + `seq_cst` doorbell ordering (§§9.1–9.3, §15); added
  synchronous `GrantInstall` / `GrantAck` on `fd 3` before `open_terminal` completes (§9.1);
  specified non-blocking `busy` backpressure on Ring A, bounded grace wait on Ring B, doorbell
  re-arming when `head_b != tail_b` after `drain_per_wake`, and continuous `O_NONBLOCK` log pipe
  draining (§9.3); specified full cross-platform IPC, shared-memory, doorbell, handle-passing,
  parent-death, ConPTY, and sandboxing parity across Linux, FreeBSD, OpenBSD, macOS and Windows
  (§§8.2, 9.1, 10.4, 14.1).

**0.5 (2026-10-31).** Final minor polish after review round 4 (all six critics converged).

- **§1.1:** Q1's existing suspension-point lowering noted; Q1 gate expanded to cover record decoding
  and step-path resumption.
- **§6.4, §13.1:** Builder calls consistently qualified with `b.` / `ag.`; header installation paths
  named.
- **§7.3, §14.3:** `vagent.native.staging_dir` setting and `StagingNoexec` refusal added.
- **§8.1:** Parent check compares against `VAGENT_SUPERVISOR_PID`.
- **§10.1, §10.5:** `NativeCallFailed` listed among synthesized declarations; `hold_terminal`
  idempotency, closed decision outcomes (`granted`, `refused`, `expired`), private permascroll
  purpose quotation, sole I/O-thread writer and §13.4 ledger gate stated.
- **§11.4, §11.6:** Positional out-parameter skipping clarified; `read_key` sequence shown in full.
- **§12.1–12.5, §14.2:** Sustained-uptime reset for `max_failed_starts`; P20.1/P20.2 table wording
  and line ranges refined.

**0.4 (2026-10-30).** Revised after review round 3, and at the owner's request.

- **N13** now has two scopes: a grant is intention-scoped by default, and `hold_terminal` moves it
  to the agent. `intentionEnded` is derived in `deriveSchedule` by diffing live top-level intention
  ids.
- **§11.6** has a complete `Keyboard` example that type-checks, using `Opt<Terminal>`, a declared
  predicate, and a `libc` `read_key` sequence.
- **`none_if`** maps a C sentinel to `Opt` at the boundary.
- A call **omits its `cstring_out` parameters**.
- **The seccomp allowlist** gains `access`, `faccessat`, `faccessat2`, `getcwd`, `getppid`, `pipe2`,
  `ftruncate` and own-pid `tgkill`. It is derived with `strace` and checked with a `TRAP`-mode build
  that reports over the log pipe.
- **§6.4:** the C++ wrapper is a supported deliverable for ease of use. The C ABI is kept for
  adoption by other languages, and a C-only twin module proves the two produce the same op log.

**0.3 (2026-10-30).** Revised after review round 2.

- **Q1–Q2 (§1.1)** name the actor compiler's present limits as prerequisites owed by the vagent
  plan's milestone 6: effects, native skills, and effect `else`.
- **N13:** grants are scoped to the requesting intention, through P20.6's `intentionEnded`.
- **ABI (§6.1–6.2):** status codes, string lists, module and imports, slot classes with allowed
  child kinds, and generated constants.
- **`programRef`** now includes `kProgramFormat`.
- **Sequences (§11.4)** get a grammar: `:=`, cleanup order, result timing, `len`. A fixed cause
  vocabulary and the `NativeCallFailed` percept are added (§10.3).
- **Checkpoints** no longer revoke grants (§12.3).
- **Control-plane death** is handled: parent-death signal, `_exit` on end-of-file, and a sweep of
  earlier epochs.
- **Upgrades** notify the supervisor (§12.4).
- **§11.6** adopts a goal to read input.
- **Seccomp (§8.3)** gains an enumerated, log-checked allowlist and the architecture/x32 kill rule.
- **The authority ledger** gets a private permascroll, approval withdrawal, and quotation instead of
  fan-in (§10.5).
- **Build:** Linux-gated control-side files and a refusing substitute (§14.1, P20.5).
- **Test (§13.2):** a stale-token variant, exact ledger rows, a structural comparator, and
  `mvinnstr` bounded by `len(text)`.

**0.2 (2026-10-30).** Revised after review round 1. Native ops construct the `ast.hpp` tree,
compiled through P20.2 `fromUnit`. Native programs are `CompiledProgram`s.

**0.1 (2026-10-30).** First draft, written against `b5ecfaf7`.
