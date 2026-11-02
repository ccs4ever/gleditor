# Round 4 critique: System Architect (vagent-native-agent.md v0.4)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 5 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R3.M1 (§8.3 allowlist omissions): RESOLVED. All omitted syscalls identified in R3 (`access`,
  `faccessat`, `faccessat2`, `getcwd`, `getppid`, `pipe2`, `ftruncate`, and own-pid `kill`/`tgkill`
  for `abort`) are now explicitly listed in §8.3 (lines 528-537). The memfd seal probe in §13.4
  correctly exercises `ftruncate` under the allowlist and observes `EPERM`.
- R3.M2 (§8.3 `LOG`-mode check unreadable): RESOLVED. The allowlist is derived via `strace -f` over
  the test suite, and unprivileged enforcement is validated in §8.3 and §13.4 using a `TRAP`-mode
  check build whose `SIGSYS` handler writes trapped syscall numbers to the log pipe.

## Audit of v0.4 architectural changes

- **C++ wrapper (`vagent_native.hpp`, §6.4):** Header-only layer over `vn_api` depending only on the
  C ABI and standard C++23 `<expected>`. It avoids pulling any Xanadu/ZigZag engine headers into
  module builds. Op-log equivalence between the C++ wrapper and pure C ABI is enforced by the §13.2
  test asserting byte-identical logs against `tests/samples/vagent/native/curses_hello_c.c`.
  Architectural layering and clean separation between control plane and guest are preserved.
- **N13 two-scope grants (`hold_terminal`, §10.1, §12.4):** Structurally resolves intention-lifetime
  leaks on failed plans without requiring front-end effect `else` (Q2). The broker unlinks intention
  associations on `hold_terminal`, promoting grant lifetime to agent scope cleanly.
- **`deriveSchedule` diffing (P20.6, §12.4):** Derives `intentionEnded` by diffing live top-level
  intention IDs during the existing post-turn schedule derivation walk
  (`agent_executor.cpp:4038-4072`). Incurs zero additional graph walks and avoids C++ lifecycle
  coupling into the VM graph.
- **`none_if` modifier (§11.2):** Intercepts C return sentinels (`-1`, `EOF`) at the host runner
  boundary before frame serialization, delivering typed `Opt<T>` to the vagent runtime and upholding
  the house rule against sentinel values.

## BLOCKING

None.

## MAJOR

None.

## MINOR

1. (§6.4) **Naming consistency:** The review brief references the C++ wrapper as
   `native_builder.hpp`, while §6.4 and §14.1 name it `vagent_native.hpp`. The document's name
   alongside `vagent_native.h` is clean; maintain this header filename consistently across packaging
   rules (P20.5).
1. (§8.1) **Supervisor PID verification:** In step 1, the parent process verification should
   explicitly compare against the supervisor PID passed via an environment variable or argument
   rather than depending solely on `getppid()`, safeguarding against re-parenting in environments
   with nested PID namespaces.
1. (§7.3) **`noexec` mount detection on staging directory:** If `$XDG_RUNTIME_DIR` resides on a
   filesystem mounted with `noexec`, `dlopen` of `module.so` will fail with `EACCES`. Providing a
   typed refusal (`StagingNoexec`) and allowing a configurable staging root setting avoids opaque
   loader errors.
1. (§12.1) **Backoff reset hysteresis:** A crash-looping module that succeeds on a single trivial
   effect before terminating could continually reset the consecutive failure counter. Consider
   requiring a sustained uptime or multiple consecutive successful turns before resetting
   `max_failed_starts`.
1. (§10.5) **Sole writer for authority ledger:** Explicitly note that the broker's dedicated I/O
   thread is the sole writer to the private `authorities/` store and permascroll, preventing any
   potential multi-threaded contention on the unshared `Store` instance.

VERDICT: CONVERGED
