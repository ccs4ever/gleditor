# Round 3 critique: System Architect (vagent-native-agent.md v0.3)

Tally: 0 BLOCKING, 2 MAJOR, 4 MINOR. VERDICT: NOT CONVERGED (two cheap fixes).

## Status of previous BLOCKING/MAJOR findings

- R2.1 (allowlist not enumerated): PARTLY. The list is now named, but see M1 (`access` missing) and
  M2 (the LOG check cannot be read).
- R2.2 (no architecture check): RESOLVED. BADARCH kills, the x32 bit kills, and there is an
  `int 0x80` probe.
- R2.3 (ledger permascroll): RESOLVED. Private `authorities/permascroll/`, typed refusal,
  fail-closed `approved`. See minor 4.

## New findings

### M1. (§8.3) The allowlist still omits calls ncurses and glibc make

- `access` / `faccessat` / `faccessat2`: ncurses' `_nc_access` calls `access()` on each terminfo
  path before opening it. With ENOSYS, `newterm` finds no terminfo and §13.2 fails at the first
  curses call.
- `getcwd` (ld.so and ncurses path handling) and legacy `readlink`, `stat`, `lstat`, `open`
  (non-`at`) may be used by a glibc or musl target. A glibc that issues them should be named.
- `pipe2`, `select`/`pselect6`, `getppid` and `tgkill` to the own pid (abort) are not listed.
  `abort()` is a §13.4 gate, so `tgkill` or `rt_sigprocmask` plus `tgkill` on the own pid must be
  allowed. The list names `tgkill` only under the refused rule, so the allow side is implicit.
- `ftruncate` is unlisted, so it gets ENOSYS. The §13.4 probe "`ftruncate` of the shared region,
  expected `EPERM` by memfd seals" can then never observe the seals.
- `getppid` is unlisted and returns -1. The `kill(getppid())`, `pidfd_open(getppid())` and
  `ptrace(..., getppid())` probes then pass for the wrong reason.

**Fix:** add `access`, `faccessat`, `faccessat2`, `getcwd`, `getppid`, `ftruncate` (or have the
probe use a call that is allowed), `tgkill` for the own tgid, and `pipe2`. State that the list is
the glibc x86-64 and aarch64 result of the strace-derived run (M2), not a hand-written list.

### M2. (§8.3) The `LOG`-mode check has no readable channel

`SECCOMP_RET_LOG` writes to the kernel audit subsystem (`dmesg` or `auditd`). An unprivileged test,
especially one inside CI or a container, cannot read it, and the logging may be disabled by
`kernel.seccomp.actions_logged`. As written, the test either skips silently or passes having
observed nothing.

**Fix:** derive the list from an `strace -f -c` run of the §13 test and the probe module, and commit
the result. Run the check in the test with a default action of `SCMP_ACT_TRAP` and a SIGSYS handler
installed in the host's test build, which writes the call number to the log pipe. That is readable
without privilege. Say the check fails, not skips, when the channel is unavailable.

## MINOR

1. (§8.1) The step 1 parent check should say what it compares: the supervisor's pid passed on the
   command line or as an environment variable, not `getppid()` alone. A pid namespace makes the two
   differ.
1. (§7.3, carried from R2) A `noexec` `$XDG_RUNTIME_DIR` is not yet named. Give it a typed failure
   and a setting for the staging root.
1. (§12.1) A module that completes one effect and then dies still resets the failure count each
   time. Reset only after a sustained uptime.
1. (§10.5) Add the gate promised in R2.3: after an approval and a revocation, the author's
   permascroll is byte-identical. Name the broker's I/O thread as the ledger store's only writer.
