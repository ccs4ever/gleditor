# Round 2 critique: System Architect (vagent-native-agent.md v0.2)

Tally: 0 BLOCKING, 3 MAJOR, 5 MINOR. Verdict: sound, revise first (the three MAJORs are cheap).

## Status of previous BLOCKING/MAJOR findings

- R1.1 (module and libraries unreadable under Landlock): RESOLVED. Staged copy with a rule beneath
  it (§7.3), locale and terminfo defaults, `~/.terminfo` excluded on purpose.
- R1.2 (libseccomp claim vs build): RESOLVED. libffi and libseccomp are linked only into
  `apps/vagent-native-host/` (§14.1); see minor 5 for what the host links from the engine.
- R1.3 (C++ not justified): RESOLVED. The §14.1 table argues each unit; nothing is a layering
  violation, and the engine side stays device-free.
- R1.4 ("leverages multiprocess support" not delivered): RESOLVED as disclosure. §1 says what it
  does and does not supply, and NQ7 puts the call to the owner. The owner may still read it as a
  miss.
- R1.5 (contention per skill name): RESOLVED. Moved into the host (§11.5, P20.1).
- R1.6 (kill and signal guarantees): RESOLVED. pid-argument filters plus the ABI 4 threshold. See M2
  for the arch check they rely on.
- R1.7 (`clone3`): RESOLVED. ENOSYS with the `clone` fallback and probes. The glibc fallback is
  correct.
- R1.8 (module vs `module_digest`): RESOLVED. Hash, then stage from the same bytes. No TOCTOU is
  left: the host cannot write beneath the staging directory and starts after the copy.
- R1.9 (terminal left in the agent's mode): RESOLVED in substance (§10.4, gate). See minor 2.
- R1.10 (reaping): RESOLVED. The §12.5 table is complete.
- R1.11 (restart limit): RESOLVED. See minor 3 for a residual.

## New findings

### M1. (§8.3) The seccomp allowlist is not enumerated, and a default of `ENOSYS` breaks glibc and ncurses as written

**Problem:** The "Allowed" list names memory management, `clone` with `CLONE_THREAD`, futexes,
`read`/`write`/`readv`/`writev`/`pread64`/`pwrite64`/`sendmsg`/`recvmsg`, `openat`, `close`,
polling, clocks, `ioctl`, signals and exit. With default `ENOSYS`, everything else fails. That
includes:

- `newfstatat`, `fstat` and `statx`: `ld.so` calls them on every `dlopen`, and stdio and ncurses
  call them too. `dlopen` of the module or `libncursesw` fails.
- `lseek`, `fcntl` and `readlink(at)`: needed by stdio, terminfo reading and `ttyname`.
- `getpid`, `gettid`, `prlimit64`, `sigaltstack`, `sched_yield`, `getrandom`, `uname` and
  `set_tid_address`.
- `rseq` and `set_robust_list` degrade tolerably. `mmap` of the locale archive is covered by
  `openat` plus memory management, and `ioctl` for termios is covered. `memfd_create` refusal is
  safe here, because §11.3 refuses callbacks and so no libffi closure is ever allocated. `ffi_call`
  needs no `memfd_create`, but the document should say that.

**Fix:** Replace the prose list with a named list. Derive it by running the §13 test and the probe
module under `strace -f` on the glibc the project targets, then commit the result as a table in the
design. Add a probe that runs `dlopen`, `newfstatat`, `fcntl(F_GETFD)`, `lseek` and `getrandom`
sandboxed and expects success. Otherwise the first sandboxed run fails at step 6 for a reason the
document never predicted.

### M2. (§8.3) No architecture check on the filter

**Problem:** Filters keyed on syscall number are bypassed through the 32-bit compat entry
(`int 0x80`) on x86-64. There `execve` is 11, which is `munmap` on x86-64 and so allowed, and `kill`
is 37. The x32 bit (`nr & 0x40000000`) is a second route on x86-64. The document never mentions
`AUDIT_ARCH`. This defeats the R1.6 and R1.7 fixes, which are all number-and-argument rules.

**Fix:** The first filter instruction checks the architecture and kills the process on mismatch.
Reject x32 numbers. Add a probe: a 32-bit `int 0x80` `execve` stub in the escape module (x86-64
only). Also state the int-versus-64-bit register compare for the pid arguments, and that the rule is
"allow if equal, else `EPERM`".

### M3. (§10.5) The ledger store does not say which permascroll it writes, and the default is the wrong one

**Problem:** A `Store` is handed a `UserPermascroll`, and every Structure cell with content spools
primedia into it. The ledger holds a purpose text spooled "once per program", agent names, programs
and policy text. If the broker uses the author's permascroll, which is the default everywhere else,
then:

- approval history and agent purposes enter an append-only record that is torrented in Merkle pieces
  and can never be redacted;
- the broker writes to the author's scroll concurrently with the editor. The comparison to the epoch
  journal does not help: that journal is not a `Store`. §10.5 also does not say who owns the store's
  lock.

**Fix:** Name a private deployment scroll. `store.tables` v4 already carries "ordered private
deployment scroll descriptors", and the `system://activity` store is the precedent for a private
store. Declare it unpublished. Say the broker's I/O thread is the only writer, and that
`authorities/` is created at first use with its own scroll. State what NQ5 retention means for an
append-only scroll, namely that retention cannot truncate it. Add a gate: after an approval and a
revocation, the author's permascroll is byte-identical.

## MINOR

1. (§7.3) `$XDG_RUNTIME_DIR` can be mounted `noexec`, and then `dlopen` of the staged module fails
   on `mmap(PROT_EXEC)`. Name the failure (`NoExecStaging`) and a fallback setting for the staging
   root. The staging directory also leaks if the supervisor itself is killed. Sweep stale
   `vagent-native/*` directories whose epoch is dead at start-up. A module's `$ORIGIN` sibling
   libraries are not copied, and the document should say so.
1. (§10.4) If the host dies mid-escape-sequence, the restore bytes are absorbed into the
   unterminated CSI or OSC. The broker should track partial sequences from the relay, and begin the
   restore with `CAN` or `ESC \` when one is open. A control-plane crash while the relay is attached
   leaves the owner's terminal unrestored. That is accepted but should be stated.
1. (§12.1) A module that always completes one effect and then dies resets the failure count every
   time, so it restarts at the base backoff forever. Reset the backoff only after a sustained
   uptime, or count deaths in a window.
1. (§14.3, §10.5) `vagent.native.library_paths` is a list, but §10.5 says `system_docs.hpp` admits
   only scalar settings. Either make it a rank, or define a delimited scalar with an escape rule. It
   is the Landlock read allowlist, so it cannot be left ambiguous.
1. (§14.1, P20.5) The host needs the op-log encoder, the node-kind table and the frame codec from
   `apps/common/xanadu/vagent/native/`, but not the rest of `COMMON_XANADU_SRCS`. Say which objects
   the host links. The two ends of the IPC frame format must come from one header in the engine, not
   from two copies, with the fuzz target covering it.
