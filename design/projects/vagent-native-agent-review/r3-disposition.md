# Round 3 disposition (vagent-native-agent.md v0.3 → v0.4)

Round 3 tallies (BLOCKING/MAJOR/MINOR):

| Critic    | Tally | Verdict       |
| --------- | ----- | ------------- |
| writer    | 0/2/3 | NOT CONVERGED |
| vortex    | 0/2/4 | NOT CONVERGED |
| codebase  | 0/0/4 | CONVERGED     |
| architect | 0/2/4 | NOT CONVERGED |
| ergo      | 0/2/5 | NOT CONVERGED |
| zigzag    | 0/0/4 | CONVERGED     |

Between rounds, the owner asked for a supported C++ wrapper over the C API: the C++ layer for ease
of use, and the C ABI kept for adoption by other languages. §6.4 was rewritten to specify it. A
C-only twin of the test module proves both produce byte-identical op logs.

## Rulings

| Finding                                                      | Ruling                                                                                                                                                                                                                                                            |
| ------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| writer M1 (line over 100 columns)                            | Already fixed by the reformat that followed the §6.4 edit; `mdformat` and `mdl` are clean                                                                                                                                                                         |
| writer M2 (`mvinnstr` arity)                                 | Accepted. Rule 1 of §11.4: a call omits its `cstring_out` parameters                                                                                                                                                                                              |
| vortex M1 (`intentionEnded` has no source)                   | Accepted. It is derived in `deriveSchedule` (`agent_executor.cpp:2187`, `:4038-4072`) by diffing live top-level intention ids after each turn. A subgoal's grant is mapped to its top-level ancestor by a negward `d.subgoal` walk at staging (N13, §12.4, P20.6) |
| vortex M2, ergo M2 (N13 contradicts `curses_keep` and §11.6) | Accepted. N13 now has two scopes. `hold_terminal(t)` moves a grant to the agent. `curses_keep` and the §11.6 example hold their terminal. Gate added                                                                                                              |
| ergo M1 (§11.6 does not type-check)                          | Accepted. A complete `Keyboard` agent with `predicate last_key`, `var screen: Opt<Terminal> := none`, `if key then revise …`, and a declared `read_key -> Opt<Int>` over `libc` `fgetc`. A new `none_if` modifier maps C sentinels to `Opt` at the boundary       |
| architect M1 (allowlist omissions)                           | Accepted. Adds `access`, `faccessat`, `faccessat2`, `getcwd`, `getppid`, `pipe2`, `ftruncate` (so the seal probe sees `EPERM`) and own-pid `kill`/`tgkill` (for `abort`)                                                                                          |
| architect M2 (`LOG` check unreadable)                        | Accepted. The list is derived with `strace -f`; a check build uses `TRAP`, and its `SIGSYS` handler reports over the log pipe                                                                                                                                     |

## MINOR

Applied: writer's `close_terminal` exemption from `stale`, and the `finally endwin()` guard rule.
Noted for the final polish: the zigzag, codebase, architect and vortex minors (outcome vocabulary,
staging `noexec`, restart-count reset, sole ledger writer, cross-scroll purpose quote, the Q1 gate's
extent). None blocks convergence.
