# Round 1 critique: System Architect (vagent-native-agent.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 11 MAJOR, 4 MINOR.

N2 (turns stay in the control plane, only native code is sandboxed) is the right call. The refused
alternative in §15 holds, and the dividend in §12.1 (a dead host loses effects, never state) is
real. The findings below concern what is built around it.

## BLOCKING

None.

## MAJOR

### 1. (§8.1, §8.3) The module and FFI libraries are unreadable once Landlock is on

**Problem:** Start-up step 4 applies Landlock, step 5 loads "the module and the manifest's FFI
libraries". §8.3 grants read-and-execute only on `vagent.native.library_paths` ("the dynamic
loader's paths and `/etc/ld.so.cache`") and read on terminfo. The module is
`build/tests/curses_hello.so` (§13.1), which is in neither. The first test cannot load its own
module as specified. The same gap covers a locale archive for `libncursesw` after `setlocale`,
`~/.terminfo` and `$TERMINFO_DIRS`, and any library outside the loader paths (Nix store, `/opt`).

**Evidence:** §8.1 steps 4 and 5; §8.3 "Landlock"; §13.1 build path. Landlock decides at open time,
so `dlopen` after lockdown opens the file under the ruleset.

**Fix:** Say where the module's read rule comes from: the supervisor adds the module's directory (or
the exact file via a path beneath rule) to the ruleset, derived from the manifest and the deployment
configuration, not from a setting the owner must remember. Or open the module and each resolved
library before step 4 and map them from the descriptors. If `dlopen("/proc/self/fd/N")` is the plan,
say so; I could not verify that Landlock allows it with no `/proc` rule. Add locale and terminfo
paths to the default list, and a probe that fails start-up by name when a declared soname is
unreadable.

### 2. (§14.1, §16) The "engine links no libseccomp" claim contradicts the build

**Problem:** §14.1 puts `sandbox_linux.cpp` (Landlock, seccomp, rlimits, namespaces) in
`apps/common/xanadu/vagent/native/` and then states "Nothing in `apps/common/xanadu/` links libffi
or libseccomp". `Makefile:633` compiles every `.cpp` under `apps/common/xanadu` into
`COMMON_XANADU_SRCS`, and `Makefile:649` makes those `XUDU_CORE_OBJS`, which `xuzz`, `vquery`,
`vagent` and the test binaries all link. A file that includes `<seccomp.h>` would force the
dependency on every one of them. §16's first property depends on this claim.

**Evidence:** `Makefile:633-649`; `Makefile:686` (`VAGENT_RUNTIME_CORE_OBJS` filters by directory,
so `native/` would need its own list).

**Fix:** Split by link unit and say it in §14.1. The engine tree gets only device-free,
dependency-free pieces: `op_log`, `manifest`, `native_program`, `scaffold`, `remote_skill`,
`supervisor`, `ipc`, `broker`. The host-only pieces (`sandbox_linux.cpp`, the libffi runner, the
loader) go under `apps/vagent-native-host/`. P20.5 states the Makefile rule that keeps the engine
glob from picking them up. Alternatively, `sandbox_linux.cpp` must use raw `seccomp(2)` and
`landlock_*` syscalls with no library, which also removes one dependency; say which.

### 3. (§14.1, §4) No component is justified as C++ rather than Vortex

**Problem:** The governance rule (`AGENTS.md` "Sovereign keymap and Vortex governance";
`.claude/rules/architectural_governance.md` §4) requires new C++ to justify why it is not Vortex.
The document names ten new C++ units and gives no per-component reason. Several have no OS reason:
the grant table and policy (a decision over `programRef:kind` pairs), the verifier allowlist walk,
the scaffold splice, the restart policy, and the terminal filter's escape-sequence classifier.

**Evidence:** §14.1 lists files only. §10.2 and §12.1 are policy logic.
`design/vagent-native-agent.md` contains no section applying the rule.

**Fix:** Add a table to §14.1 with one row per unit: OS interface (spawn, `SCM_RIGHTS`, Landlock,
seccomp, pty, shared-memory rings: valid C++ reasons), or the Vortex alternative and why it was
refused (cost, trust, boot-order). At minimum price the grant policy and the restart policy as
Vortex or as cells in an agent-owned slice (system settings and grants as cells follows the
cells-and-dims rule), and justify the verifier as C++ because it must run before any Vortex from the
artifact does.

### 4. (§1 item 5, §8.1, §12.1) "Leverages Vortex's multiprocess support" is not delivered

**Problem:** The request asked to leverage Vortex's multiprocess support. The document says the
supervisor works "in the manner of the Spool-Mesh worker lifecycle (`MeshHost`, `mesh_cli`,
incarnation epochs)", but nothing in `vortex/` spawns or supervises a process. Searching
`apps/common/xanadu` and `apps/vagent` for `posix_spawn`, `fork` and `exec` finds only
`provenance.cpp:245`. `MeshHost` workers are in-process threads and swarm peers. So the supervisor,
the incarnation counter, the IPC framing and the restart policy are all new, parallel to
`mesh_epoch` and `spool_mesh` (`vortex/spool_mesh.cpp:39-63` already bounds payloads at
`identity::kMaxPayloadBytes`). The owner's request is answered by analogy, and the reader is not
told that.

**Evidence:** `apps/common/xanadu/provenance.cpp:245-254` (the only spawn); `vortex/mesh_host.hpp`
(no process type); `vortex/mesh_epoch.{hpp,cpp}`.

**Fix:** State in §1 that no process-level multiprocess support exists today, and either (a) reuse
`mesh_epoch`'s epoch type and `spool_mesh`'s size-bounded codec, naming the symbols, or (b) record
that the incarnation counter is a separate concept and why. Extract the spawn code in
`provenance.cpp` into a shared helper used by both (the "posix_spawn precedent" is a function-local
pipe-and-actions sequence, not an API) as a numbered proposal, or say the supervisor will duplicate
it.

### 5. (§11.4, N10, N11, P20.1) Contention is per skill name, shared by all agents

**Problem:** §11.4 binds `one_thread` libraries as `Contention::Queue` "so the control plane's view
matches the host's". `HostBindings` keeps one `Binding` per skill name for the whole registry, with
a single `live` list and a single `queue` (`host_bindings.cpp:50-56`, `:195-225`). With N10 (one
host per agent) and a registry-wide `Curses` binding, `Queue` serialises agent A's `batch` behind
agent B's, although they run in different processes, and `preempting` methods cancel every live
effect of the binding across agents. The queue is an unbounded `std::deque`. P20.1 only adds the
agent's name to `EffectRequest`; it does not change the contention domain.

**Evidence:** `vagent/runtime/host_bindings.cpp:50-56`, `:195-225` (`busy` is computed from
`binding.live` regardless of agent); `host_bindings.hpp:96-103`.

**Fix:** Either P20.1 makes the contention domain `(skill, agent)` for bindings that declare it (a
`BindingOptions` field, priced, with its tests), or `RemoteNativeSkill` binds `Shared` and the host
queues per library thread. The second keeps contention out of existing code, but then state that the
queue depth is a setting and that a queued effect's `within` deadline runs from staging, not from
dequeue.

### 6. (§8.3, §13.4) The kill and signal guarantees need ABI 6 or a mechanism that exists

**Problem:** §8.3 has seccomp refusing "`kill`/`tgkill` outside the process". A seccomp-bpf filter
sees only integer arguments. It can compare `kill` or `tgkill`'s target with the host's own pid, but
`tkill`, `rt_sigqueueinfo`, `rt_tgsigqueueinfo` and `pidfd_send_signal` are not listed at all. The
host runs under the node's uid, so without a boundary it can signal the node (`SIGKILL`, `SIGSTOP`).
The real guard is Landlock ABI 6 signal scoping, but §8.3 and N7 make ABI 4 the refusal threshold
and ABI 6 "also" optional. The probe table says `kill(getppid())` is refused by "seccomp (Landlock
ABI 6)", which is both.

**Evidence:** §8.3 "Seccomp" refusal list; N7; §13.4 probe row 7. Landlock ABI 6 is Linux 6.12 (as
the document says).

**Fix:** Pick one. Either raise the required ABI to 6 (kernel 6.12; price the nodes lost), or run
the host in a PID namespace (unprivileged user namespaces are already "when permitted" in §8.3;
promote it to required for the signal boundary on ABI 4–5), or list every signalling call and give
the argument filter. Make the probe table state the expected refusal per ABI so the test is not red
on 6.7–6.11 kernels.

### 7. (§8.3) `clone3` is not handled, and the thread/fork rule depends on it

**Problem:** The allowlist refuses "`clone` without `CLONE_THREAD`". glibc 2.34 and later call
`clone3` first for `pthread_create` and fall back to `clone` only on `ENOSYS`. `clone3` takes a
struct, which seccomp cannot inspect. If it is allowed, a module can fork. If it is refused with
`EPERM` (the document's default action), `pthread_create` fails and the `one_thread` library thread
(§11.4) cannot start. Neither is the outcome the table intends.

**Evidence:** §8.3 names `clone` only; "Refused with `EPERM`" is the stated default.

**Fix:** State that `clone3` returns `ENOSYS`, so the loader's fallback gives the filterable
`clone`. Add the probe `fork()` through `clone3` raw, and a test that a thread can still be created.

### 8. (N6, §8.1 step 5) Nothing binds the loaded module to `module_digest`

**Problem:** N6 puts `sha256(module)` into `programRef`, and §10.2 relies on that to make approvals
per exact build. §8.1 has the host load the module from a path, and the `Ready` frame reports
library digests but not the module's. The host is untrusted from the moment foreign code loads, so a
digest it reports is not evidence, and a path can change between the control plane's hash and the
host's `dlopen`. An approval for `programRef:terminal` could then be spent on different code.

**Evidence:** §8.1 step 5; §8.4 (libraries only); N6; §10.2.

**Fix:** The control plane opens the module, hashes the bytes it opened, and passes that descriptor
(plus a sealed copy in a `memfd` if `dlopen` needs a path). The host's `Ready` check is for
diagnosis, not authority. Specify it in §8.1 and add a gate test that swaps the file after hashing.

### 9. (§10.4, §12.1) A killed host leaves the owner's real terminal in the agent's mode

**Problem:** The deadline kill (§12.1) and any crash skip `endwin`. When the relay is attached to
the owner's terminal (`vagent run`), the slave side may have switched the real terminal to the
alternate screen, enabled mouse reporting or bracketed paste (all "mode" sequences the `strict`
filter passes), hidden the cursor, and the broker has put the owner's terminal in raw mode to relay
keystrokes. Nothing in §10.4 saves or restores any of it. Revocation closes the master but does not
repair the display. This is the most visible failure a user would meet from the kill-on-deadline
policy.

**Evidence:** §10.4 (relay and filter), §12.1 (kill), N9 "Price".

**Fix:** The broker records the owner's `termios` and tracks which private modes the relay has
passed, and on revocation emits the inverse (`?1049l`, `?1000l`, `?2004l`, `?25h`, SGR reset) and
restores `termios`. Add a gate test with a pty standing in for the owner's terminal: kill the host
mid-screen, assert the restore sequences and termios.

### 10. (§10.3, §11.2, §14.3) Handle, grant and queue rows have no reaping rule

**Problem:** The document bounds handles per host (`max_handles` 4096) but never says when a handle
or a broker row is reclaimed.

- Handles are reclaimed only by a `consumed` call or retirement of the incarnation. An agent that
  drops a `NativeHandle` from its state (a plan that failed after `newterm`) leaks it until the host
  dies, and 4096 leaks later every `newterm` fails. There is no per-agent-turn or per-goal scope.
- Broker rows "keyed by agent name and grant id" (§10.3): the document does not say they are erased
  on `release`, revocation, `stop`, lease end, or agent exit, or when the name is reused.
- The percept path: `Readable` is posted per readiness; if the agent is slower than the terminal,
  percepts queue in `AgentExecutor`. I did not find a bound on that queue in `agent_executor.hpp`
  (not verified in depth).
- The log pipe and ring B `Log` frames are rate-limited but the limit is not a setting.

**Evidence:** §10.3, §11.2 (`consumed`), §12.1 (retirement), §14.3 (no percept or log limits).

**Fix:** State a lifecycle table: for each of handle, grant row, grant fd, queued effect, percept,
log line: created by, bounded by (setting), reaped on. Give handles an owner (the effect that made
them, or an explicit scope the agent releases) and let `stop` and checkpoint drop everything. Add
coalescing for `Readable` (at most one outstanding per grant).

### 11. (§12.1, §14.3) The restart limit cannot trigger as written

**Problem:** A restart waits `restart_backoff` (5 s) and "more than `max_restarts` (3) deaths within
one backoff window" fails further effects. Deaths are separated by at least one backoff, so at most
one death can fall inside any 5 s window, and 3 is unreachable. A module that crashes at start-up
restarts every 5 s indefinitely and the guard "so a crashing module cannot consume the node" never
fires. A restart is also driven by the agent's next native effect, so the loop is the agent's.

**Evidence:** §12.1 final bullet; §14.3 rows `restart_backoff` and `max_restarts`.

**Fix:** Define the window independently (`restart_window`, a setting, longer than
`max_restarts * restart_backoff`) or use exponential backoff to a cap with a hard failure after N
consecutive deaths without a successful effect. Say which counter resets and when.

## MINOR

### 12. (§9.1, §9.3, §8.3) Shared memory and socket hostility is incomplete

§9.3 covers copy-then-decode and index bounds. Not covered: (a) each ring's own producer and
consumer indices live in memory the host can write, so the control plane must keep private copies of
the indices it owns (say so); (b) the seccomp allowlist admits `sendmsg`/`recvmsg` on any descriptor
(§8.3), so the host can send descriptors to the control plane over `SCM_RIGHTS`. The I/O thread must
use `MSG_CMSG_CLOEXEC`, a per-message descriptor cap, and close any it did not expect; (c) the
doorbell eventfd can be flooded, so the drain must be bounded per wake.

### 13. (§8.1 step 2) `RLIMIT_NPROC` is per user and breaks threads

`RLIMIT_NPROC` is compared against the user's total process and thread count, not the host's. "As
low as the platform allows" will either do nothing (fork is already refused) or make
`pthread_create` fail for the library thread. Drop it from the list, or say it is set to the current
count plus the thread budget and justify.

### 14. (§9.1, §14.3) The 64 KiB frame is a ceiling users will work around

`frame_bytes` is bounded by the mesh's payload limit, which is the wrong reason: the sandbox link is
not a mesh link. An `ffi` effect carrying a 100 KiB buffer fails `too_large`, and the user chunks
it. Price it, or give large arguments a chunked frame or a per-effect side region sized by a grant
limit. The "eventual answer" in the specification's §18.3 is a different problem (events on the
wire).

### 15. (§8.3) "Allowlist" with a list of refusals

§8.3 calls the filter an allowlist, then lists the calls refused with `EPERM` and a list of allowed
ones. Say the default action (kill or `ERRNO(ENOSYS)`) for everything not listed, and that the
`EPERM` list is the subset the probes observe. As written an implementer could build a denylist,
which §15 refuses. Also, `EPERM` instead of kill is right for probes but the production default
should be recorded as a setting (`vagent.native.seccomp_action`) so the stricter one is available.

VERDICT: NOT CONVERGED
