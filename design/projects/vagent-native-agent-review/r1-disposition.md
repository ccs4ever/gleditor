# Round 1 disposition (vagent-native-agent.md v0.1 → v0.2)

Round 1 tallies: writer 0/6/7, vortex 1/10/3, codebase 3/5/4, architect 0/11/4, ergo 2/6/4, zigzag
0/5/3 (BLOCKING/MAJOR/MINOR). All six critics: NOT CONVERGED.

The orchestrator verified the load-bearing claims against the code before ruling:

- `Gen` is a private class in `compiler.cpp`'s anonymous namespace (`:92`, `:411`), and it is
  AST-driven.
- The outbox drain skips malformed entries after commit (`agent_executor.cpp:2246-2252`,
  `:2379-2386`).
- `instantiate` recompiles per agent (`program.cpp:148-160`).
- `Makefile:633` globs all of `apps/common/xanadu`.
- `HostBindings` contention is per skill name (`host_bindings.cpp:50-56`).
- `LocalHost` takes `CompiledProgram`.
- `prelude.cpp` holds only events.
- Lifecycle signals are absent: only turn and intention observers exist.

## The central ruling

**Native ops now construct the actor front end's program tree (`ast.hpp`), not opcode fragments.**
Codebase B1 and vortex B1 showed that the compiler has no seam for splicing fragments. Vortex M2/M3,
zigzag M1 and writer M4 showed that no verifier proposed in v0.1 makes fragments safe. A native
program is now a `Unit`, checked and compiled through a new `CompiledProgram::fromUnit` (P20.2). It
is therefore a `CompiledProgram`, and every downstream feature works: `LocalHost`, live upgrade,
P19. The owner's phrase "constructing its vortex graph with native ops" is now met at the
program-tree level. Raw opcode construction is deferred to NQ1, with the critics' requirements
recorded. This is flagged to the owner as the most significant reinterpretation in the revision.

## Per-finding rulings

### writer

| #   | Ruling                                                                                                                 |
| --- | ---------------------------------------------------------------------------------------------------------------------- |
| M1  | Accepted. `Grant` is replaced by the synthesized `Terminal { token, term, rows, cols }` (§10.1); `scope` is gone       |
| M2  | Accepted. The generic batch is replaced by typed sequences (§11.4)                                                     |
| M3  | Accepted. Sequences and functions surface as `effect def … within` (§11.2, §11.4)                                      |
| M4  | Moot. Fragments are removed (N4)                                                                                       |
| M5  | Accepted. Binding ops are a kind family in §6.2; the example uses only defined ops through the C++ layer (§6.4, §13.1) |
| M6  | Accepted. §13.2 step 8 now asserts a stale token on a resumed copy                                                     |
| m1  | Accepted. NQ numbering is rebuilt and §15's promised question exists (NQ1)                                             |
| m2  | Accepted. Cause table §10.3; `stale` replaces `stale_grant`/`stale_handle`                                             |
| m3  | Accepted. All types used are declared (§10.1, §11.2)                                                                   |
| m4  | Accepted. A descriptor table in §8.1                                                                                   |
| m5  | Accepted. The header reads P20.1–P20.8                                                                                 |
| m6  | Accepted. §8.2 says only Linux is specified; Windows is "not planned (N7)"                                             |
| m7  | Accepted with the example rewrite                                                                                      |

### vortex

| #   | Ruling                                                                                                                                                                      |
| --- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| B1  | Accepted; the central ruling                                                                                                                                                |
| M2  | Moot (no fragments); recorded in NQ1                                                                                                                                        |
| M3  | Moot; recorded in NQ1 and §15                                                                                                                                               |
| M4  | Accepted. N1 now states that compilation per agent is exactly a compiled program's, with P17 as the remedy for both                                                         |
| M5  | Moot. C6 is now the compiler's; the checkpoint walk is withdrawn                                                                                                            |
| M6  | Accepted. N11: `RemoteNativeSkill` binds `Shared`; contention lives in the host                                                                                             |
| M7  | Accepted. Incarnation is `(author:device:epoch, serial)`, from `reserveNodeId`'s durable epoch (§12.2); tokens are bound to the pid, so a resume (new pid) is stale (§12.3) |
| M8  | Accepted. P20.6 lifecycle observer; §12.4                                                                                                                                   |
| M9  | Accepted. §1 item 5 states plainly that Vortex offers distribution, not isolation; the `MeshHostTransport` peer is refused in §15; NQ7 asks the owner                       |
| M10 | Accepted. Length-prefixed hash parts (N6); module staged from the hashed bytes (§7.3)                                                                                       |
| M11 | Accepted. The ABI is specified as generic node and slot setters over an X-macro table generated from `ast.hpp` (§6.1–6.2); §13.2 step 2 is a Unit-equality test             |
| m12 | Accepted. Edge-triggered arming (§11.6)                                                                                                                                     |
| m13 | Accepted. C10 cites `EffectInFlight`; no new check                                                                                                                          |
| m14 | Moot with fragments                                                                                                                                                         |

### codebase

| #   | Ruling                                                                                                                                                                                         |
| --- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| B1  | Accepted; the central ruling. P20.2 is now `fromUnit`, a factoring of `fromSource`, not a compiler refactor                                                                                    |
| B2  | Accepted. C12 states the existing skip truthfully (§3); P20.6 (drain) is withdrawn because a native program is compiled and cannot stage malformed entries                                     |
| B3  | Accepted as vortex M9; P20.7 extracts the spawn helper                                                                                                                                         |
| M4  | Accepted (= vortex M4)                                                                                                                                                                         |
| M5  | Accepted. `LocalHost` needs no change, because native programs are `CompiledProgram`s. P20.3 adds `NativeSettings` plumbing and states the `vagent run` system-store gap. P20.8 covers the CLI |
| M6  | Accepted with option (a). Authority capacities are synthesized declarations, so there is no prelude change                                                                                     |
| M7  | Moot (the C6 walk is withdrawn). The `promote` finding (base cells not expanded) is noted                                                                                                      |
| M8  | Accepted. Host-only code lives in `apps/vagent-native-host/`, outside the glob (§14.1); P20.5 adds `TEST_PROGRAMS`                                                                             |
| m9  | Accepted. `frame_bytes` is now argued from the 32 KiB effect value bound (§9.1)                                                                                                                |
| m10 | Accepted. Defaults are stated as unmeasured placeholders with what to measure (§14.3)                                                                                                          |
| m11 | Moot (no opcode allowlist)                                                                                                                                                                     |
| m12 | Accepted. Packaging is in P20.5                                                                                                                                                                |

### architect

| #   | Ruling                                                                                                                                                                                                                                         |
| --- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1  | Accepted. The module is staged into a control-plane-owned directory with a Landlock rule beneath it (§7.3); locale and terminfo paths are default; `~/.terminfo` is excluded on purpose                                                        |
| M2  | Accepted (= codebase M8)                                                                                                                                                                                                                       |
| M3  | Accepted. §14.1 justification table                                                                                                                                                                                                            |
| M4  | Accepted (= vortex M9); P20.7                                                                                                                                                                                                                  |
| M5  | Accepted (= vortex M6)                                                                                                                                                                                                                         |
| M6  | Accepted. Seccomp argument filters on `kill`, `tgkill` and `rt_sigqueueinfo` against the host's own pid; `tkill`, `pidfd_*` refused; ABI 4 is kept as the threshold, with ABI 6 as defence in depth; the probe table is uniform across ABI 4–6 |
| M7  | Accepted. `clone3` returns `ENOSYS`; probes for fork, raw `clone3` and `pthread_create`                                                                                                                                                        |
| M8  | Accepted (= vortex M10); §13.4 file-swap gate                                                                                                                                                                                                  |
| M9  | Accepted. The broker records `termios` and passed modes and restores both on revocation; §13.4 gate                                                                                                                                            |
| M10 | Accepted. §12.5 lifecycle table; handles cannot outlive their sequence; readiness is armed singly                                                                                                                                              |
| M11 | Accepted. Exponential backoff with a consecutive-failure count (§12.1)                                                                                                                                                                         |
| m12 | Accepted. §9.3 covers private indices, `MSG_CMSG_CLOEXEC` with descriptor refusal, and a per-wake drain bound                                                                                                                                  |
| m13 | Accepted. `RLIMIT_NPROC` is removed                                                                                                                                                                                                            |
| m14 | Accepted in substance. The frame bound is argued as no tighter than the agent's own; larger payloads are U-A3's                                                                                                                                |
| m15 | Accepted. Default action `ERRNO(ENOSYS)`, stated                                                                                                                                                                                               |

### ergo

| #   | Ruling                                                                                                                                                                                                      |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| B1  | Accepted. Complete ABI by rule (§6.1–6.2); binding kinds; the example uses a defined C++ layer                                                                                                              |
| B2  | Accepted (= writer M1, M2)                                                                                                                                                                                  |
| M3  | Accepted. Typed, named sequences, as the critic proposed; the generic batch is refused in §15                                                                                                               |
| M4  | Accepted. One failure rule (a step failure fails the effect, with the step named in the cause), with `finally` cleanup; grants are agent-scoped and released on stop; intention scoping is NQ8              |
| M5  | Accepted. `vagent_native_api_version`, `{version, size}` header, id 0 invalid, `error()`, no variadics                                                                                                      |
| M6  | Moot in part (no statement brackets: statements are `Stmt` nodes); `uses` names capacities in the example                                                                                                   |
| M7  | Accepted. `within` everywhere; out-params as result fields; `fail_if` grammar including `null`; `consumed` only on success; `in_` rename; one record type per handle type                                   |
| M8  | Accepted. Kinds become synthesized capacities, so undeclared kinds are refused at seal; purpose comes from the declaration only; cause table; opaque token without incarnation; `TerminalReadable` declared |
| m9  | Accepted. One modifier syntax                                                                                                                                                                               |
| m10 | Moot (no `Result` record)                                                                                                                                                                                   |
| m11 | Accepted. Synthesized names are unqualified program declarations and reserved against clashes (§6.3)                                                                                                        |
| m12 | Accepted. `c_bool` is `_Bool`; `c_int` for int flags                                                                                                                                                        |

### zigzag

| #   | Ruling                                                                                                                                                                                                                                                                              |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1  | Moot (no fragments; the compiler's emission is the existing one); recorded in NQ1                                                                                                                                                                                                   |
| M2  | Moot (the C6 walk is withdrawn)                                                                                                                                                                                                                                                     |
| M3  | Accepted. Approvals are cells in the authority ledger (§10.5); a string setting is refused in §15                                                                                                                                                                                   |
| M4  | **Partly refused.** The agent-visible half (capacities, records, events) is synthesized into the `Unit` and compiled into cells. The C half is consumed only by the host's libffi runner; putting it in cells would mean reading it straight back out for the host. Recorded in §15 |
| M5  | Accepted. Authority ledger of decisions and revocations as Structure operations in a private deployment store (§10.5)                                                                                                                                                               |
| m6  | Accepted (= writer M1)                                                                                                                                                                                                                                                              |
| m7  | Accepted. C9 states both counts and cites `runtime_abi.hpp:70`                                                                                                                                                                                                                      |
| m8  | Accepted. N5 states that constants go where `fromSource` puts them, and that spans are outside `programRef`                                                                                                                                                                         |

## Proposal register after round 1

- P20.1 effect routing.
- P20.2 is now `fromUnit`.
- P20.3 settings plus `NativeSettings`.
- P20.4 logging.
- P20.5 build and packaging.
- P20.6 is now the lifecycle observer. The conditional drain change is withdrawn.
- P20.7 spawn helper extraction (new).
- P20.8 CLI (new).
