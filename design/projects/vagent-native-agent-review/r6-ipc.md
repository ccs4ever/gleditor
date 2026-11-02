# Round 6 critique: IPC & Cross-Platform Shared Memory Expert (vagent-native-agent.md v0.6)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **R5.B1 (§8.1, §9.1, §9.3, §15 Asymmetric `shm_a` / `shm_b`):** RESOLVED. The shared region is
  cleanly decoupled into two distinct shared-memory objects: `shm_a` (`fd 4`), opened strictly
  read-only in the host (`PROT_READ` / `FILE_MAP_READ`), and `shm_b` (`fd 5`), opened read-write to
  carry the host's Ring B frames and Ring A consumer cursor `tail_a`. A compromised host can no
  longer corrupt Ring A's buffer or producer cursor while the control plane writes, eliminating the
  C++23 data race and cross-process memory corruption hazard. The §13.4 probe suite adds an explicit
  `mprotect`/`mmap` `PROT_WRITE` check on `fd 4` observing `EACCES`/`EPERM`, and §15 records the
  single writable shared-memory alternative as refused.
- **R5.B2 (§9.1, §9.2, §9.3, §15 SPSC ring layout, framing, atomics, and `WireValueView`):**
  RESOLVED. Exact binary structs `ShmARegion` and `ShmBRegion` are specified with `alignas(128)`
  cache-line isolation for cursors (covering both 64-byte x86-64 and 128-byte Apple Silicon lines).
  Framing uses a 32-byte aligned fixed binary `FrameHeader` (`magic = 0x564E`, `type`, `seq`,
  `token`), a `0xFFFFFFFFU` wrap marker for contiguous buffer boundaries, and monotonic 64-bit byte
  offsets masked with `cursor & (ring_bytes - 1)`. Atomic semantics specify
  `std::atomic_ref<uint64_t>` acquire-loads and release-stores paired with a `seq_cst` thread fence
  before doorbell notification. The hot path adopts a zero-allocation `WireValueView`
  (`decodeValueView`) returning `std::string_view` and `std::span` slices directly into a
  pre-allocated 64 KiB thread-local scratch buffer, which completely defeats shared-memory TOCTOU /
  double-fetch attacks from host-writable `shm_b`.
- **R5.B3 (§8.1, §9.1, §9.3, §10.4 Synchronous `GrantInstall` / `GrantAck` handshake):** RESOLVED.
  Descriptors are no longer passed out-of-band without synchronization. When granting an authority,
  the broker sends `GrantInstall{seq, token, kind}` with the descriptor over `fd 3` and waits for
  `GrantAck{seq, token}` before invoking `EffectCompletion` for `open_terminal`. This guarantees
  that the descriptor is bound and present in the host's grant table before any subsequent
  `StartEffect` frame can arrive on Ring A.
- **R5.B4 / R5.M7 (§8.1, §8.2, §10.4, §11.2, §12.1, §14.1 Cross-platform feature parity):**
  RESOLVED. All five platforms (Linux, FreeBSD, OpenBSD, macOS, Windows) are specified in full
  architectural parity in §8.2:
  - *Shared memory:* `memfd_create` (Linux), `shm_open2(SHM_ANON)` (FreeBSD), `shm_open` +
    `shm_unlink` (macOS/OpenBSD), `CreateFileMappingW` (Windows).
  - *Doorbells:* `eventfd` (Linux/FreeBSD), non-blocking pipe (macOS/OpenBSD), auto-reset `Event`
    handles (Windows).
  - *Control channel & handle passing:* `SOCK_SEQPACKET` (Linux/FreeBSD/OpenBSD), `SOCK_STREAM` with
    4-byte length prefix (macOS), named pipe + `DuplicateHandle` (Windows).
  - *Parent-death:* `PR_SET_PDEATHSIG` + `pidfd` (Linux), `PROC_PDEATHSIG_CTL` (FreeBSD), `kqueue`
    `EVFILT_PROC` `NOTE_EXIT` (macOS/OpenBSD), Job Object `KILL_ON_JOB_CLOSE` (Windows).
  - *Terminal brokering:* `posix_openpt` (POSIX) vs. ConPTY `CreatePseudoConsole` (Windows).
  - *Capsicum pre-opening:* On FreeBSD, libraries and `/usr/share/terminfo` are pre-opened with
    `cap_rights_limit` prior to `cap_enter()`, and loaded via `fdlopen` (step 5b/step 7).
- **R5.M5 (§9.3, §13.4, §14.3 Doorbell re-arming on `drain_per_wake` burst):** RESOLVED. If unread
  frames remain in Ring B after the I/O thread drains `vagent.native.drain_per_wake` (256) frames
  (`head_b != tail_b`), the thread immediately re-arms its doorbell before returning to the poll
  loop. A §13.4 gate verifies that a 600-frame burst drains completely without stranding.
- **R5.M6 (§9.1, §9.3, §10.3, §14.3 Ring A/B backpressure and non-blocking log pipe):** RESOLVED.
  Staging into a full Ring A immediately fails non-blockingly with cause `busy`. Ring B writer waits
  on the control doorbell up to `vagent.native.call_grace`, exiting cleanly on timeout to prevent
  distributed deadlock. The log pipe (`fd 2`) is non-blocking (`O_NONBLOCK`) and drained
  continuously, dropping excess bytes over `log_bytes_per_second` in memory so foreign `stderr`
  writes never stall.

## Audit of v0.6 IPC and shared-memory changes

- **Option 1 binary framing (§9.2):** Retaining canonical bencode for `.vnprog` artifacts and the op
  log preserves bit-exact cryptographic reproducibility for `programRef` (SHA-256) and mesh CAS.
  Moving the shared-memory rings to a fixed 32-byte `FrameHeader` paired with `WireValueView`
  eliminates bencode parsing overhead on the hot path while avoiding heavy external dependencies
  (`avro-cpp` Boost/JSON dependencies).
- **TOCTOU double-fetch immunity (§9.3):** The combination of a private 64 KiB thread-local scratch
  buffer copy and linear `WireValueView` parsing solves the vulnerability where a hostile peer in
  `shm_b` mutates offsets in place after verification. Because the copy is bounded to 64 KiB, the
  operation stays within L1/L2 cache (~1–2 µs) and incurs zero dynamic heap allocations.
- **Cache-line isolation (§9.1):** Sizing cursor alignment to `alignas(128)` prevents false sharing
  on both 64-byte lines (x86-64) and 128-byte lines (Apple Silicon M-series), ensuring lock-free
  atomic scalability across all architectures.

## BLOCKING

None.

## MAJOR

None.

## MINOR

1. (§8.1) **Windows handle hygiene in step 2:** Step 2 references `close_range(8, ~0U, 0)`. On
   Windows, child handle leakage is prevented at spawn time via `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`
   in `UpdateProcThreadAttribute`. Note this Windows-specific handle list mechanism alongside POSIX
   `close_range`.
1. (§9.1) **`tail_b_published` doorbell signaling:** §9.1 notes that the control plane signals the
   doorbell whenever advancing `tail_b_published` frees space from a full Ring B. In implementation,
   ensure the control plane checks `head_b - tail_b_published >= capacity - max_frame_bytes` before
   signaling the doorbell, avoiding redundant doorbell writes when Ring B is operating well below
   capacity.
1. (§9.2) **`FrameHeader::magic` endianness:** The constant `0x564E` ("VN") is specified as a
   `uint16_t`. Explicitly note that multi-byte header integers (`magic`, `type`, `seq`) are encoded
   in native little-endian byte order (standard across all supported tier-1 architectures: x86-64,
   aarch64, arm64).

VERDICT: CONVERGED
