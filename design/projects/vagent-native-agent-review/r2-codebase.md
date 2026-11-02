# Round 2 critique: Codebase Expert (vagent-native-agent.md v0.2)

Paths below are relative to the repo root. Runtime files are under
`apps/common/xanadu/vagent/runtime/` unless stated. Tally: 1 BLOCKING, 3 MAJOR, 5 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (no scaffolding seam): RESOLVED in the design's terms. Native ops now build a `Unit`; P20.2
  is `fromUnit`. But the new route exposes a different compiler gap (B1 below).
- R1.2 (C12 / drain): RESOLVED. §3 states the skip truthfully (`agent_executor.cpp:2246-2252`,
  `:2379-2386` verified) and the drain proposal is withdrawn.
- R1.3 (no multiprocess supervisor): RESOLVED. §1 item 5 says Vortex offers distribution, not
  isolation; `--mesh-worker` exists in `mesh_cli.cpp:236,242`, and the supervisor is declared new.
- R1.4 (replay cost): RESOLVED. N1 states `instantiate` recompiles per agent and resume
  (`program.cpp:148-160` verified).
- R1.5 (`LocalHost`, settings, CLI): RESOLVED. `LocalHost` exposes `bindings()` and `executor()`
  (`program.hpp`), so a `CompiledProgram` from `fromUnit` needs no `LocalHost` change. P20.3 and
  P20.8 state the `vagent run` system-store gap (`apps/vagent/main.cpp:11-14` verified).
- R1.6 (prelude capacity): RESOLVED. Synthesized ordinary declarations need no prelude change, and
  the checker needs none for `capacity`, `record`, `event` and `native skill` forms
  (`checker.cpp:1082-1103`). Compilation is B1.
- R1.7 (C6 walk): RESOLVED by withdrawal; the program is a `CompiledProgram`.
- R1.8 (Makefile glob): PARTLY. Host-only code is out of the glob and `TEST_PROGRAMS` is listed, but
  see M3.

## Citations checked

All verified at the cited line: `program.cpp:18-27`, `:31-110`; `compiler.cpp:92` (anonymous
namespace), `:411` (`class Gen`); `agent_executor.cpp:3271`, `:3895`, `:4789-4791`;
`agent_executor.hpp:334-341`, `:347-351`, `:410-413`; `host_bindings.cpp:50-56`, `:195-225`;
`runtime_abi.hpp:70` (15 names); `mesh_epoch.hpp:17-24`; `mesh_host.hpp:13-44` (the
`MeshHostTransport` surface); `provenance.cpp:240-254`; `system_docs.hpp:760-765`; `Makefile:633`,
`:1334`. `program.cpp:148-160` is `instantiate`.

## P20.2 `fromUnit` audit

It is a clean factoring. `fromSource` uses the source text in exactly three places: `parseUnit`, the
diagnostic prefix `name`, and `refOf(source)` in the final constructor call. Everything between
(`check`, `makeUpgradeSchema`, per-agent `compileAgent` into a staging context, `promote`, the remap
loop) reads only `*unit` and `*checked`. `fromUnit(Unit, name, ref)` is therefore "everything after
parse, with `ref` passed in". Diagnostics use `loc.line`/`loc.column`, so an op ordinal can ride in
`loc.line`; the checker's only use of `loc` is sorting errors (`checker.cpp:343`). The one defect is
M1.

## BLOCKING

### 1. (N1, N4, §13.1, §14.2) The compiler refuses native skills and effect calls today

**Problem:** N1/N4 say a native program passes through "the existing checker and compiler ...
exactly as source does", and §13.1's agent does `install HostTerminals()` and calls
`open_terminal(24, 80)`. The compiler cannot compile either. No proposal covers it.

**Evidence:** `Gen` bails with `unsupported(...)` ("is not supported in this iteration",
`compiler.cpp:103-106`) at three sites. `compiler.cpp:6761-6763`:
`if (sd.native) unsupported("native skill " + name + " (its methods are effects)")` when an agent
installs a native skill. `:6822`: `capacityCall` does
`if (sig->isEffect) unsupported("effect methods", loc)`. `:6102`: so does `prepareMethodSet` for
effect `MethodDef`s. `Gen::effectCall` (`:3827`) is fully written (staging the outbox entry with
skill, method, id and args) but has no caller: `grep effectCall` finds only its declaration
(`:1097`) and definition. No compiler test uses `native skill`; the effect tests hand-build outbox
entries ("the compiler side comes later", `tests/xuzz/vagent_effects_test.cpp:5`).
`design/vagent-implementation.md` marks phase 6 "Effects and composition" Open.

**Fix:** Add a proposal for `compiler.cpp` (and its tests): admit a native skill as a bodiless
container, route `capacityCall` on an effect method to `effectCall` with the installed skill's
qualified name, and cover suspension positions and step paths for effect calls (C5). Price it. It is
a first-class compiler feature, not a splice. Or state it as a prerequisite owned by the vagent
implementation plan and re-scope §13 until it lands. Either way N1's "exactly as source" must say
which of the two it relies on.

## MAJOR

### 1. (N6) `programRef` omits `kProgramFormat`

**Problem:** N6 hashes `vagent-native/1`, the op log, the manifest and the module digest. A native
`programRef` then survives a compiler change that alters what the same `Unit` compiles to.

**Evidence:** `program.hpp:30-32` bumps `kProgramFormat` "so a ref made by an older compiler never
names a program this one would build differently", and `refOf` hashes it (`program.cpp:18-23`). The
ref is what `resume`, retained programs and P17's shared base compare. A native ref does not move
when the compiler does.

**Fix:** Include `kProgramFormat` as a fifth length-prefixed part.

### 2. (§10.5, §14.1) The authority ledger needs a permascroll the design never names

**Problem:** The ledger is "a private store in the node's deployment directory" of cells whose
content is a program ref, kind, purpose, outcome and so on. A cell's text is typed into a
permascroll, and a store holds no primedia.

**Evidence:** `Store::makeCell(parent, text)` is "typed into the author's permascroll now"
(`store.hpp:511`). The default `Store()` makes a fresh in-memory `UserPermascroll`
(`store.cpp:123-125`). `fromSource` can use that because a program store is ephemeral. A ledger
saved to `authorities/` and reloaded after a restart would have `ops.nodes` and no text, so
`approved` would read empty refs. That is the "document that is not the document" failure AGENTS.md
describes.

**Fix:** Say which permascroll holds ledger text. Either the author's, with approvals then
transcluding from it, or a dedicated deployment permascroll with its own lifecycle. Say how the
broker opens it and what `approved` compares. Alternatively mint approvals as scalar and structural
cells only, with the `programRef` hex as content. Add the load/refuse behaviour for a ledger whose
permascroll is missing.

### 3. (P20.5, §14.1) Control-side `native/*.cpp` is still globbed into every binary

**Problem:** `supervisor`, `ipc` and `broker` stay in `apps/common/xanadu/vagent/native/` and use
`memfd_create`, `eventfd`, `SCM_RIGHTS` and `posix_openpt`. `Makefile:633` globs them into
`XUDU_CORE_SRCS`, hence into `xuzz`, `vquery`, `vagent` and every test binary, on every platform in
`packaging/` (macOS, Android, wasm).

**Evidence:** `COMMON_XANADU_SRCS := $(shell find apps/common/xanadu -name '*.cpp')`
(`Makefile:633`). P20.5 names probes "under `GLEDITOR_ENABLE_NATIVE_AGENTS=1|0`" but not what `=0`
or a non-Linux build does with these files. §14.1 only argues the libffi/libseccomp half.

**Fix:** State the mechanism: a `filter-out` of the three Linux-only files when the flag is 0 or the
platform is not Linux, with `op_log`, `replay` and `remote_skill` (portable) staying in the glob. Or
guard the files with `#if` and a refusing stub. Say which.

## MINOR

- (§12.2) "Two incarnations never share a name across restarts" holds only where `reserveNodeId` is
  called (`mesh_cli.cpp:249`, `mesh_host.cpp:836`). `LocalHost` takes `Options::epoch` (default 1,
  `program.hpp`), and `apps/vagent/main.cpp` never reserves one. `AgentExecutor` keeps `localNode_`
  private (`agent_executor.hpp:682`), so the supervisor must take the name from `LocalHost::node()`.
  Tokens are safe anyway (CSPRNG, in-memory table); say so and drop the claim for `vagent run`.
- (§12.3 vs §12.4, C6/P20.6) §12.3 says the host "may stay alive across a checkpoint" and the agent
  "keeps its grants". §12.4 and the `agent_ended` cause revoke grants and shut the host down on
  `checkpointed(pid)`. Pick one.
- (§6.2) "One kind per `ast.hpp` struct" omits `PolicyArg` and `ArbitrationClause` (P19), so a
  native program cannot state arbitration policy. The X-macro table is hand-kept over 73 structs, so
  the round-trip test catches a missing entry only in one direction. §13.2 step 2 also needs a
  loc-ignoring structural `Unit` comparator, and `Unit` is move-only with no `operator==`
  (`ast.hpp:26`, `:493`). List the comparator as new test code.
- (P20.3) `NativeSettings` in `agent_executor.hpp` is unneeded. Nothing in the executor reads it;
  the supervisor and broker do. Put it in `LocalHost::Options` (or a native header) only.
- (N11) Two native programs on one node both synthesize unqualified `HostTerminals`. `bind` replaces
  by name (`host_bindings.hpp:117`). State that the `RemoteNativeSkill` is one node-global forwarder
  keyed by agent pid, or qualify the names.

VERDICT: NOT CONVERGED
