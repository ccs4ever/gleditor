# Round 6 Disposition: `design/vagent-native-agent.md` (v0.6 -> Converged)

- **Target Document:** `design/vagent-native-agent.md`
- **Base Version Reviewed:** `0.6` (2026-10-31)
- **Critics:** `security` (Security & Sandboxing Expert), `ipc` (IPC & Cross-Platform Shared Memory
  Expert), `user` (Native Agent Developer / User Journey), `writer` (Technical Writer)
- **Overall Result:** **CONVERGED** (0 BLOCKING, 0 MAJOR across all critics; `P20.1`–`P20.8`
  approved by owner)

## 1. Summary of Round 6 Verdicts

| Critic                        | Blocking | Major | Minor | Verdict         |
| :---------------------------- | :------: | :---: | :---: | :-------------- |
| `security` (`r6-security.md`) |    0     |   0   |   0   | `CONVERGED` ✓   |
| `ipc` (`r6-ipc.md`)           |    0     |   0   |   3   | `CONVERGED` ✓   |
| `user` (`r6-user.md`)         |    0     |   0   |   2   | `CONVERGED` ✓   |
| `writer` (`r6-writer.md`)     |    0     |   0   |   2   | `CONVERGED` ✓   |
| **Total**                     |  **0**   | **0** | **7** | **`CONVERGED`** |

## 2. Verification of Round 5 Findings

All 8 BLOCKING and 12 MAJOR findings from Round 5 (`r5-security.md`, `r5-ipc.md`, `r5-user.md`) were
verified as **RESOLVED** in v0.6 by their respective specialist critics:

- **`security` (`r6-security.md`):** Confirmed `RESOLVED` for `B1` (64-bit masked `ioctl` allowlist
  \+ high-bit `TIOCSTI` probe), `B2` (closed `strict` CSI allowlist dropping `n`, `c`, `t`, `p` and
  mouse/focus/paste modes), `M1` (two-phase lockdown with `PR_SET_MDWE` and phase-2 seccomp revoking
  `PROT_EXEC` on `mmap`/`mprotect` and denying `openat`), `M2` (seal-time handle linearity
  `ConsumedHandleReused` and 128-bit CSPRNG handle tokens), `M3` (`0 <= n <= max_out_bytes` check on
  `cstring_out(n)`), and `M4` (cross-platform sandbox invariants and TOCTOU scratch-copy
  protection).
- **`ipc` (`r6-ipc.md`):** Confirmed `RESOLVED` for `B1` (asymmetric read-only `shm_a` on `fd 4` and
  read-write `shm_b` on `fd 5`), `B2` (`alignas(128)` `ShmARegion`/`ShmBRegion`, 32-byte binary
  `FrameHeader`, `0xFFFFFFFFU` wrap marker, `std::atomic_ref` acquire/release + `seq_cst` fence, and
  zero-allocation `WireValueView` over a pre-allocated 64 KiB thread-local scratch copy), `B3`
  (synchronous `GrantInstall` / `GrantAck` handshake on `fd 3`), `B4`/`M7` (5-OS IPC, shared-memory,
  doorbell, handle-passing, parent-death, ConPTY, and Capsicum `fdlopen` parity), `M5` (doorbell
  re-arming when `head_b != tail_b` after `drain_per_wake`), and `M6` (Ring A `busy` backpressure,
  Ring B grace wait, and continuous `O_NONBLOCK` log pipe draining).
- **`user` (`r6-user.md`):** Confirmed `RESOLVED` for `B1` (`vagent run <file.vnprog>` automatically
  enabling `nativeSettings.enabled = true` and accepting `--module`, `--grant-policy`, `--ledger`),
  `B2` (`module_filename` in `.vnprog` and deterministic module resolution), and `M1`–`M5` (5-step
  developer walkthrough in §5.1, `VN_API_VERSION` and `vagent_native_skills` C ABI in §6.1,
  `VN_KIND_NATIVE_*` table in §6.2, C++ `Builder` methods in §6.4, and P20.8 CLI syntax in §14.2).

## 3. Disposition of Final Minor Polish Items

- **`user` M1 / `writer` M1 (§5.1 `pkg-config` name):** Updated §5.1 step 2 to use
  `$(pkg-config --cflags vagent-native)` to match `vagent-native.pc` in §14.1 and P20.5.
- **`ipc` M1 (§8.1 step 2 Windows handle hygiene):** Noted `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` on
  Windows alongside POSIX `POSIX_SPAWN_CLOEXEC_DEFAULT` and `close_range`.
- **`ipc` M3 (§9.2 `FrameHeader` endianness):** Noted that `FrameHeader` fields are in native
  little-endian byte order.
- **`writer` M2 (§13.4 `fd 3` gate wording):** Refined the §13.4 gate phrasing to "sends an
  unexpected descriptor on `fd 3`, or sends data on `fd 3` other than a valid `GrantAck`".
