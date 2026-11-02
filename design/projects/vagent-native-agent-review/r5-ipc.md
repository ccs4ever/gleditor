# Round 5 critique: IPC & Cross-Platform Shared Memory Expert (vagent-native-agent.md v0.5)

Paths below are relative to the repo root. Tally: 4 BLOCKING, 3 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§8.1, §9.1, §9.3) Single shared RW memfd allows guest corruption of Ring A

**Problem:** Colocating Ring A (control → host) and Ring B (host → control) within a single `memfd`
mapped `PROT_READ | PROT_WRITE` into the untrusted native host process completely undermines the
control plane's isolation model and induces undefined behavior under C++23.

**Evidence:** In §8.1 descriptor table (`design/vagent-native-agent.md:474`) and step 4 (`:485`), fd
4 is passed as "the shared region, a sealed `memfd`" and mapped by the host. In §9.1 (`:588-590`),
the control plane "creates the region with `memfd_create`, sizes it to two rings of
`vagent.native.ring_bytes`, and seals it with `F_SEAL_GROW`, `F_SEAL_SHRINK` and `F_SEAL_SEAL`
before spawn." Because the native host must write completions and percepts to Ring B, the host
requires write permission to fd 4. However, because both Ring A and Ring B reside within this single
`memfd`, the untrusted guest has read-write access to the entire backing store.

Even if the host process initially maps Ring A's segment as `PROT_READ`, a compromised guest module
or malicious FFI library holding the writable fd 4 can call `mprotect(..., PROT_WRITE)` or issue a
new `mmap(..., PROT_READ | PROT_WRITE, MAP_SHARED, 4, ...)` to write directly into Ring A.
Consequently:

1. The guest can overwrite Ring A's data buffer while the control plane is actively writing or
   formatting a frame (`StartEffect`, `CancelEffect`, `Revoke`, `Shutdown`). Under C++23
   [intro.races], unsynchronized concurrent writes to non-atomic memory by two execution agents
   constitute a data race with undefined behavior, allowing torn reads, compiler optimization
   invalidation, and memory corruption in the control plane process.
1. The guest can overwrite Ring A's producer index (`head`), forging the appearance of new control
   frames or wrapping the buffer arbitrarily.
1. Placing producer and consumer indices of both rings in a shared header on the same page or cache
   line allows the host to corrupt control plane private tracking or inflict atomic cache-line
   bouncing and bus locking on the control plane's I/O thread.

**Fix:** Decouple Ring A and Ring B into two independent shared-memory descriptors and page regions
with strictly partitioned access privileges:

1. **Ring A (control → host):** Allocated via a dedicated `memfd_create("vagent_ring_a", ...)`. The
   control plane maps it `PROT_READ | PROT_WRITE`. Before spawning the host (or before passing the
   fd), the control plane applies `F_SEAL_FUTURE_WRITE` (Linux 5.1+), or creates a read-only
   duplicate descriptor via `/proc/self/fd/N` with `O_RDONLY` while sealing the original. The host
   receives Ring A opened strictly read-only and maps it `PROT_READ`. Any attempt by the guest to
   map or protect Ring A as `PROT_WRITE` fails with `EPERM` or `EACCES`.
1. **Ring B (host → control):** Allocated via a separate `memfd_create("vagent_ring_b", ...)`.
   Mapped `PROT_READ | PROT_WRITE` in the host, and `PROT_READ` (with private consumer index
   tracking) in the control plane.
1. **Index Placement:** Ring A's consumer index (written by host, read by control) must NOT reside
   on a Ring A page. It must reside in Ring B's header or on a dedicated page aligned to
   `PAGE_SIZE`. Producer and consumer indices must be isolated on independent cache lines via
   `alignas(hardware_destructive_interference_size)` (64 bytes on x86-64, 128 bytes on Apple
   Silicon) to prevent false sharing and malicious cache line contention.

### 2. (§9.1–9.3) SPSC ring layout, framing, and atomic semantics unspecified

**Problem:** §9.1–9.3 omits the binary memory layout, header structures, frame framing, wrap-around
memory contiguity guarantees, and atomic synchronization semantics of the SPSC rings. An implementer
cannot construct interoperable or memory-safe ring buffer code from this specification.

**Evidence:** §9.1 (`:588-596`) states that each ring is sized to `vagent.native.ring_bytes`
(default: 1 MiB) with frame bodies up to `vagent.native.frame_bytes` (default: 64 KiB), and §9.2
(`:598-601`) states frames are bencoded dictionaries decoded with `wire.hpp:decodeValue`. However:

1. **Contiguous buffer requirement vs. Circular wrap:** `decodeValue(std::string_view bytes)`
   (`wire.hpp:98`) requires a contiguous memory view. In a standard circular ring buffer, a 64 KiB
   frame enqueued near the end of the 1 MiB ring buffer wraps across the end boundary, splitting the
   frame body into two disjoint memory segments. `decodeValue` cannot parse split buffers without an
   intermediate heap allocation and copy, defeating zero-copy shared memory. The document specifies
   neither circular virtual address mirroring nor wrap-around padding markers.
1. **Missing Header and Slot Layout:** The document specifies no header struct, no index widths
   (`uint32_t` vs `uint64_t`), and no alignment. If 32-bit indices are used with modular arithmetic,
   monotonic sequence checks fail after $2^{32}$ bytes (~4 GiB of throughput).
1. **Unspecified Framing:** The document does not specify whether frames have an envelope header
   (e.g., frame length prefix, frame type, CRC, or magic) preceding the bencoded payload, or how the
   consumer determines frame boundaries when reading the raw ring stream.
1. **Missing C++23 Atomic Semantics:** §9.3 references private copies of indices, but specifies no
   memory orderings. Concurrent SPSC synchronization requires explicit acquire-release semantics
   across process boundaries.

**Fix:** Specify the exact ring layout, framing, wrap-around mechanism, and C++23 atomic semantics
in §9.1:

1. **Memory Layout:**

   ```cpp
   struct alignas(64) RingHeader {
     // Cache line 0: Producer fields (written by producer, read by consumer)
     alignas(64) std::atomic<uint64_t> head{0}; // Monotonic byte write cursor
     uint64_t capacity{0};                      // Ring buffer size in bytes (power of 2)
     uint32_t max_frame_bytes{0};               // Maximum frame payload limit

     // Cache line 1: Consumer fields (written by consumer, read by producer)
     alignas(64) std::atomic<uint64_t> tail{0}; // Monotonic byte read cursor

     // Cache line 2: Buffer payload start
     alignas(64) uint8_t data[];
   };
   ```

1. **Contiguous Wrap-Around Guarantee:** Mandate either:

   - **Virtual Address Mirroring (Preferred):** The ring buffer region is mapped into two contiguous
     virtual address ranges of length `ring_bytes` back-to-back using double `mmap` calls on the
     same file descriptor (`mmap(addr, 2 * size, ...)` followed by
     `mmap(addr, size, MAP_FIXED, fd, 0)` and `mmap(addr + size, size, MAP_FIXED, fd, 0)`). Any
     frame up to `max_frame_bytes` starting at any offset in $[0, \text{capacity})$ is guaranteed to
     appear contiguous in memory, eliminating wrap handling in decoders.
   - **Wrap Padding Marker (Fallback):** If the space remaining between `head` and the physical ring
     boundary is smaller than the frame header plus body length, the producer writes a special
     `FRAME_WRAP_SENTINEL` (length = 0) and wraps `head` to offset 0, skipping the tail padding.

1. **Framing Format:** Each frame begins with an 8-byte framing header: `uint32_t frame_len`
   (payload length in bytes), `uint16_t magic` (`0x564E`, "VN"), `uint16_t flags`. The bencoded
   payload immediately follows.

1. **Atomic Semantics:**

   - Producer writes frame payload, then executes:
     `std::atomic_ref(header.head).store(new_head, std::memory_order_release);`
   - Consumer reads head:
     `auto cur_head = std::atomic_ref(header.head).load(std::memory_order_acquire);`
   - Consumer processes frames, then updates tail:
     `std::atomic_ref(header.tail).store(new_tail, std::memory_order_release);`
   - Producer reads tail:
     `auto cur_tail = std::atomic_ref(header.tail).load(std::memory_order_acquire);`

### 3. (§8.1, §9.1, §10.4) Out-of-band descriptor passing races with Ring B

**Problem:** Descriptors granted by authority brokers (e.g., slave pty for `open_terminal`) are
transmitted out-of-band over the Unix domain control socket (`fd 3`) via `SCM_RIGHTS`, while effect
completions and subsequent foreign calls travel across the shared memory rings (Ring A / Ring B).
Because these two IPC mechanisms have completely separate queues and scheduling, the arrival of
descriptors and effect completions is inherently asynchronous, creating a critical race condition.

**Evidence:** In §8.1 (`design/vagent-native-agent.md:473`), fd 3 is
`socketpair(AF_UNIX, SOCK_SEQPACKET)` used for `SCM_RIGHTS` grants. In §9.1 (`:580-586`), ring
frames carry `StartEffect` and `EffectDone`. In §10.4 (`:711-715`), the broker calls `posix_openpt`,
`grantpt`, `unlockpt` and sends the slave descriptor to the host. Consider the sequence when an
agent calls `open_terminal`:

1. Control plane broker approves the grant, creates the pty pair, sends the slave fd over `fd 3` via
   `SCM_RIGHTS`, and publishes `EffectDone{Terminal{token, ...}}` on Ring B.
1. The agent receives `EffectDone` and immediately executes the next intention step in its plan
   (e.g., `read_key` sequence or `initscr` foreign call), causing the executor to issue
   `StartEffect` on Ring A referencing the newly minted `token`.
1. Because Ring A is mapped shared memory and signaled via an eventfd, `StartEffect` can arrive and
   be dequeued by the host worker thread *before* the host's I/O thread has read `fd 3`, unpacked
   the ancillary control message (`recvmsg`), and registered the received descriptor into the host's
   handle table.
1. The host attempts to resolve the terminal descriptor for the foreign call, finds no descriptor
   bound to the token, and fails the effect with `stale` or `NativeCallFailed`. Furthermore, §9.1
   defines no message framing or correlation metadata on `fd 3`. When `recvmsg` receives a file
   descriptor, there is no specified protocol indicating which `token`, `intention`, or `seq` the
   descriptor belongs to.

**Fix:** Eliminate out-of-band descriptor race conditions and define the control socket protocol:

1. **Control Socket Message Framing:** Every `SCM_RIGHTS` datagram sent on `fd 3` must carry an
   explicit payload structure:

   ```cpp
   struct alignas(8) DescriptorGrantMessage {
     uint64_t seq;             // Correlating effect sequence number
     uint8_t token[16];        // 128-bit CSPRNG authority token
     uint32_t authority_kind;  // Terminal = 1, Directory = 2, etc.
     uint32_t flags;           // O_RDWR, etc.
   };
   ```

1. **Synchronous Handshake:** The control plane must NOT publish `EffectDone` or conclude the
   `open_terminal` effect turn until the host has read the descriptor message from `fd 3` and sent
   an acknowledgment datagram back over `fd 3` (`DescriptorAck{seq, token, status}`). This
   guarantees that the descriptor is bound and available in the host's descriptor table before any
   subsequent `StartEffect` frame can be staged or dispatched on Ring A.

### 4. (§8.2, §9.1, §14.1) Missing cross-platform IPC and lifecycle parity

**Problem:** §8.2, §14.1, and P20.5 dismiss non-Linux platforms (macOS, Windows, FreeBSD, OpenBSD)
as unsupported stubs ("not specified", "not planned", substituted by `native_unavailable.cpp`),
relying exclusively on Linux-specific primitives without specifying equivalent cross-platform
architectures. This violates the core requirement for full cross-platform feature parity across all
supported operating systems in the `gleditor` tree.

**Evidence:** In §8.2 (`design/vagent-native-agent.md:499-510`), FreeBSD, OpenBSD, and macOS are
listed as "not specified; spike needed", and Windows is dismissed as "not planned (N7)". §14.1
(`:1208-1211`) gates `supervisor.{hpp,cpp}`, `ipc.{hpp,cpp}`, and `broker.{hpp,cpp}` as Linux-only,
substituting `native_unavailable.cpp` on all other systems. P20.5 (`:1231`) excludes them from
non-Linux builds. The current design relies on five primitives that cannot compile or operate on
non-Linux platforms:

1. **`memfd_create` and `F_SEAL_*`:** Does not exist on macOS, OpenBSD, or Windows. Windows uses
   section objects; macOS and OpenBSD use POSIX shm.
1. **`eventfd`:** Does not exist on macOS, OpenBSD, or Windows. FreeBSD only added it in FreeBSD 13.
1. **`AF_UNIX` with `SOCK_SEQPACKET`:** On macOS (Darwin), `socketpair(AF_UNIX, SOCK_SEQPACKET, 0)`
   fails with `EPROTOTYPE` because macOS kernel only supports `SOCK_STREAM` and `SOCK_DGRAM` for
   Unix domain sockets.
1. **`SCM_RIGHTS`:** Does not exist on Windows. Windows requires `DuplicateHandle()`.
1. **Parent-Death Termination (`PR_SET_PDEATHSIG`):** Linux-only. FreeBSD uses `PROC_PDEATHSIG_CTL`.
   macOS and OpenBSD have no kernel death signal mechanism and require `kqueue` monitoring. Windows
   requires Job Objects.
1. **PTY Brokering:** Windows has no POSIX ptys (`posix_openpt`). Windows 10+ uses ConPTY
   (`CreatePseudoConsole`).

**Fix:** Specify full cross-platform architectural parity across all supported platforms in §8.2,
§9.1, and §14.1:

1. **Shared Memory Abstraction:**
   - *Linux:* `memfd_create` + `F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL`.
   - *FreeBSD:* `shm_open2(SHM_ANON, O_RDWR, 0600, SHM_GROW_ON_WRITE)` and `cap_rights_limit`.
   - *macOS & OpenBSD:* `shm_open("/vagent_<uuid>", O_CREAT | O_RDWR | O_EXCL, 0600)`, `ftruncate`,
     and immediate `shm_unlink` before spawning child, retaining open descriptor across
     `posix_spawn`.
   - *Windows:* `CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, NULL)`
     anonymous section object; handle inherited or duplicated into child.
1. **Doorbell Abstraction:**
   - *Linux & FreeBSD 13+:* `eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)`.
   - *macOS & OpenBSD:* Unidirectional `pipe2(p, O_CLOEXEC | O_NONBLOCK)` (1-byte notify write and
     drain) or `kqueue` `EVFILT_USER` user events.
   - *Windows:* `CreateEventW(NULL, FALSE, FALSE, NULL)` auto-reset event handles, signaled with
     `SetEvent()` and waited on via `WaitForMultipleObjects()`.
1. **Control Channel & Descriptor Passing:**
   - *Linux, FreeBSD, OpenBSD:* `socketpair(AF_UNIX, SOCK_SEQPACKET, 0)` with `SCM_RIGHTS`.
   - *macOS:* `socketpair(AF_UNIX, SOCK_STREAM, 0)` with a 4-byte message framing prefix and
     `SCM_RIGHTS`.
   - *Windows:* Duplex named message pipe (`CreateNamedPipeW` with `PIPE_TYPE_MESSAGE`) for control
     messages, combined with `DuplicateHandle(GetCurrentProcess(), hResource, hChildProcess, ...)`
     invoked by the supervisor broker to duplicate handles directly into the child process.
1. **Parent-Death Supervision:**
   - *Linux:* `prctl(PR_SET_PDEATHSIG, SIGKILL)`.
   - *FreeBSD:* `procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &sig)`.
   - *macOS & OpenBSD:* Child supervisor thread runs `kqueue` with `EVFILT_PROC` and `NOTE_EXIT` on
     `getppid()`, combined with EOF detection on the control socket/pipe.
   - *Windows:* Supervisor creates a Job Object with `SetInformationJobObject` setting
     `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, and assigns the spawned child with
     `AssignProcessToJobObject`. When the supervisor process exits, the Windows kernel automatically
     terminates the native host.
1. **Terminal Brokering:**
   - *POSIX (Linux, macOS, FreeBSD, OpenBSD):* `posix_openpt`, `grantpt`, `unlockpt` pty pair.
   - *Windows:* ConPTY (`CreatePseudoConsole(size, hInputPipe, hOutputPipe, 0, &hPC)`). Input and
     output pipe endpoints are duplicated into the child host process via `DuplicateHandle`.

## MAJOR

### 5. (§9.1, §9.3) Doorbell clearing and drain_per_wake frame stranding deadlock

**Problem:** §9.3 limits doorbell processing to `vagent.native.drain_per_wake` (default: 256)
frames. When a burst of frames exceeds this limit, remaining frames are stranded in Ring B
indefinitely without triggering another wakeup, freezing agent percept delivery.

**Evidence:** In §9.1 (`design/vagent-native-agent.md:585`), doorbells are eventfds where a write
signifies "a ring has frames". In §9.3 (`:618-619`), the I/O thread "drains at most
`vagent.native.drain_per_wake` frames per doorbell, so a flooding host cannot monopolise the
thread." When the control plane I/O thread wakes on the doorbell:

1. It reads the 8-byte eventfd integer, clearing the eventfd counter to 0.
1. It loops up to 256 times, reading frames from Ring B and updating the consumer index.
1. If the host produced 300 frames in a burst (e.g. streaming percepts, rapid key emissions, or bulk
   logs), 44 frames remain in Ring B.
1. The control plane exits its drain loop because the 256-frame quota is exhausted, and returns to
   `epoll_wait` (or `kevent` / `WaitForMultipleObjects`).
1. Because the eventfd was already drained to 0 in step 1, and the host has already finished
   enqueuing its batch and will not issue another doorbell write, no further events fire. The 44
   remaining frames sit unconsumed in Ring B. The agent hangs waiting for completions, causing a
   fatal distributed deadlock.

**Fix:** In §9.3, specify that after processing `vagent.native.drain_per_wake` frames, the I/O
thread checks if the ring still contains frames (`head != tail`). If unread frames remain, the I/O
thread must re-arm itself without waiting for an external doorbell event by either:

1. Writing a dummy token (`1`) to its own doorbell eventfd before returning to the event loop, or
1. Scheduling an immediate non-blocking continuation on the I/O event loop to drain the next slice,
   guaranteeing starvation protection without stranding pending messages.

### 6. (§9.1, §14.3) Unspecified ring backpressure causes deadlock or frame drop

**Problem:** The IPC specification omits backpressure and full-ring flow control handling for both
Ring A and Ring B.

**Evidence:** §9.1 sizes rings to `vagent.native.ring_bytes` (1 MiB). When high throughput or a slow
consumer causes a ring to become full:

- If the control plane attempts to enqueue a `StartEffect` to Ring A and finds insufficient space:
  if the control plane blocks synchronously waiting for the host to consume, the control plane I/O
  thread stalls. If the host is simultaneously blocked attempting to enqueue a completion onto a
  full Ring B, a classic cross-process deadlock occurs.
- If the control plane or host drops frames, sequence counters (`seq`) break, violating protocol
  invariants (§9.3: "On a protocol violation it kills the host").

**Fix:** Formally define non-blocking backpressure policies for both rings in §9.1 and §9.3:

1. **Ring A Full (Control → Host):** Frame staging must be strictly non-blocking. If Ring A has
   insufficient contiguous capacity, `HostBindings::start` must fail immediately with a typed error
   `NativeIpcError::RingFull`. The vagent runtime suspends the effect staging attempt, retrying on
   the next tick after a doorbell wake indicating Ring A space has been freed by the host consumer.
1. **Ring B Full (Host → Control):** If the host finds Ring B full, it must yield its worker thread
   and wait on the host doorbell with a bounded timeout (`vagent.native.call_grace`). If the ring
   remains full past the grace period, the host terminates cleanly with an exit code signaling
   buffer exhaustion, allowing the supervisor to detect the fault and recycle the incarnation.

### 7. (§8.1, §8.2) Capsicum startup order in §8.1 prevents FreeBSD fdlopen

**Problem:** §8.1 specifies that sandbox confinement is applied before dynamic libraries are loaded.
On FreeBSD under Capsicum, entering capability mode (`cap_enter`) completely revokes global
filesystem namespace lookups, causing all subsequent `dlopen()` calls to fail immediately with
`ECAPMODE`.

**Evidence:** In §8.1 (`design/vagent-native-agent.md:488-491`), the host startup sequence states:
"6. Apply Landlock, then seccomp (§8.3). 7. Load the module from the staging directory and the
binding manifest's libraries. Resolve every declared symbol." In §8.2 table (`:502`), the FreeBSD
row states: "Capsicum: no global namespace; libraries opened before `cap_enter` and loaded with
`fdlopen`". Under Capsicum, `cap_enter(2)` places the process into capability mode. In capability
mode, traditional system calls that open files by path (`open`, `openat` with relative/absolute
paths) fail with `ECAPMODE`. Therefore, step 7 cannot open any library file or call standard
`dlopen()`.

**Fix:** Reorder the startup sequence in §8.1 to explicitly accommodate Capsicum and pre-opened
descriptors:

- Insert Step 5b (POSIX/Capsicum pre-opening): Prior to sandbox lockdown, open file descriptors for
  the staged `module.so` and every library path declared in the binding manifest.
- Under FreeBSD Capsicum: Apply descriptor rights via `cap_rights_limit()`
  (`CAP_READ | CAP_MMAP_RX | CAP_FSTAT`), call `cap_enter()`, and then load libraries using
  `fdlopen(fd, RTLD_NOW | RTLD_LOCAL)`.
- Under Linux (Landlock + seccomp) and OpenBSD (pledge + unveil): Keep the existing sequence as an
  alternative branch.

## MINOR

### 8. (§8.1, §12.1) PR_SET_PDEATHSIG thread-lifetime vulnerability on Linux

**Problem:** In Linux, `prctl(PR_SET_PDEATHSIG, SIGKILL)` binds child process death to the lifecycle
of the *calling thread*, not the parent process as a whole.

**Evidence:** §8.1 (`design/vagent-native-agent.md:464-465`, `:480`) notes: "The supervisor spawns
from one long-lived thread of its own, because `PR_SET_PDEATHSIG` fires when the spawning thread
exits." If the supervisor's thread pool recreates worker threads, or if an unhandled thread
exception terminates the spawning thread while the supervisor process remains healthy, all native
host processes are instantly killed with `SIGKILL`.

**Fix:** On Linux 5.3+, have the supervisor pass a `pidfd` of the supervisor process to the child
(or have the child open `/proc/<ppid>` with `pidfd_open`). The host's background I/O thread adds the
pidfd to its `epoll` set (`EPOLLIN`). When the supervisor process exits, the pidfd becomes readable,
providing true process-level death detection independent of thread lifecycles.

### 9. (§9.1, §14.3) Host log pipe blocking under rate limits

**Problem:** §9.1 and §14.3 establish a log pipe for host stderr with a rate limit of 16 KiB/s
(`vagent.native.log_bytes_per_second`). Standard OS pipe buffers are 64 KiB. If a guest library
emits bursty debug logging to stderr, the pipe fills while throttled, causing foreign library
threads to block synchronously on `write(2, ...)`.

**Evidence:** §9.1 (`design/vagent-native-agent.md:586`): "the log pipe: host → control: the host's
stderr, drained and rate-limited into `vagent.native`; not parsed". §14.3 (`:1265`):
`vagent.native.log_bytes_per_second` = 16 KiB. When stderr writes block, foreign library calls (e.g.
ncurses initialization errors or driver debug prints) hang, eventually tripping the effect deadline
and causing the host to be killed with `native_killed`.

**Fix:** The supervisor must set `O_NONBLOCK` on the write end of the log pipe before passing it as
fd 2, or the control plane reader must continually drain the pipe into a memory ring buffer at full
speed and discard excess bytes over the rate limit, rather than throttling consumption at the pipe
descriptor boundary.

VERDICT: NOT CONVERGED
