# Round 6 critique: Security & Sandboxing Expert (vagent-native-agent.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous findings

- **B1 (64-bit `ioctl` bypass via sign/high-bit extension): RESOLVED.** §8.3 lines 734–738
  explicitly specifies `SCMP_CMP_MASKED_EQ(1, 0xFFFFFFFFULL, cmd)` over the restricted terminal
  allowlist (`TCGETS`, `TCSETS`, `TCSETSW`, `TCSETSF`, `TIOCGWINSZ`, `TIOCSWINSZ`, `FIONREAD`),
  preventing 64-bit register truncation bypasses in `sys_ioctl`. §13.4 lines 1482–1483 adds the
  escape probe `syscall(SYS_ioctl, grant, (1ULL << 32) | TIOCSTI, "x")` asserting `EPERM`.
- **B2 (Closed `strict` CSI allowlist defeating synthetic input injection): RESOLVED.** §10.4 lines
  1019–1039 replaces the open-ended CSI definition with a closed state-machine parser: standard CSIs
  are restricted to display rendering (`A`–`H`, `J`, `K`, `L`, `M`, `P`, `S`, `T`, `X`, `@`, `d`,
  `f`, `m`, `r`), private modes are restricted to `?25` and `?1049`, and all queries (`n`, `c`),
  window manipulation (`t` / XTWINOPS), ANSI key remapping (`p`), and input-generating modes are
  dropped. §13.4 lines 1515–1518 adds the stand-in gate asserting zero response bytes reach stdin.
- **M1 (Two-phase lockdown, post-load W^X enforcement, and `PR_SET_MDWE`): RESOLVED.** §8.1 lines
  656–682 splits lockdown into Phase 1 (pre-load Landlock and seccomp) and Phase 2 (post-load
  lockdown in step 8). Step 8 invokes `prctl(PR_SET_MDWE, PR_MDWE_REFUSE_EXEC_GAIN, 0, 0, 0)` and
  stacks a second seccomp filter denying `PROT_EXEC` on `mmap`/`mprotect` and `mremap` with
  `MREMAP_MAYMOVE` on executable mappings. §13.4 lines 1484–1485 validates post-load `PROT_EXEC`
  denials.
- **M2 (Seal-time handle linearity and 128-bit CSPRNG handle tokens): RESOLVED.** §11.2 line 1145
  specifies 128-bit CSPRNG hex tokens bound to their `NativeHandleType` and host incarnation. §11.4
  lines 1209–1212 enforces seal-time linearity validation (`ConsumedHandleReused`) forbidding
  repeated use of `consumed` handles in a call or subsequent steps, and §11.4 line 1223 skips
  cleanups for already-consumed handles. §13.4 lines 1505–1507 gates seal refusal.
- **M3 (Signedness and integer overflow check on `cstring_out(n)`): RESOLVED.** §11.2 line 1144
  mandates `0 <= n <= vagent.native.max_out_bytes`, failing with `ffi_range` before allocation if
  `n < 0` or `n > max_out_bytes`. §13.4 lines 1504–1505 gates `n = -1` and `n = INT_MIN`.
- **M4 (Concrete cross-platform sandbox invariants and TOCTOU protection): RESOLVED.** §8.2 lines
  687–700 specifies concrete mechanisms across Linux, FreeBSD, OpenBSD, macOS, and Windows enforcing
  the same five security invariants. §9.2–9.3 lines 852–899 specifies the 32-byte `FrameHeader` and
  thread-local 64 KiB scratch copy that defeats shared-memory TOCTOU on `shm_b`. §13.4 lines
  1467–1490 provides the multi-platform escape probe matrix.
- **Minors 1–5 (fcntl restrictions, openat2 ENOSYS, close_range, CSPRNG tokens, mremap): RESOLVED.**
  All five minor items are incorporated into §8.1 lines 640–642, §8.3 lines 721–730 and 745–748, and
  §11.2 line 1145.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

VERDICT: CONVERGED
