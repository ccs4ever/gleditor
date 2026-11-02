# Round 2 disposition (vagent-native-agent.md v0.2 → v0.3)

Round 2 tallies: writer 0/3/5, vortex 1/4/5, codebase 1/3/5, architect 0/3/5, ergo 1/4/5, zigzag
0/2/5 (BLOCKING/MAJOR/MINOR). All six critics: NOT CONVERGED.

The orchestrator verified the shared BLOCKING finding:

- `compiler.cpp:6761-6763` refuses native skills, and `:6102` and `:6822` refuse effect methods.
- `Gen::effectCall` has no caller.
- `vagent_effects_test.cpp` drives the adapter with hand-built outbox entries.
- Plan lowering itself works: only effects and native skills are refused.

## Rulings

| Finding                                                         | Ruling                                                                                                                                                                                                                                                                                                                         |
| --------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| codebase B1, vortex B1 (compiler refuses effects/native skills) | Accepted as **prerequisite Q1** (§1.1), not a new proposal. The specification already defines effects and native skills, and vagent milestone 6 owes them. §1.1 states the exact scope, including the capacity and deadline fields `effectCall` omits, and a gate. §13 is marked as unable to run before Q1, and C3/C5 cite it |
| ergo M2 (no effect `else` in the front end)                     | Accepted as **prerequisite Q2**. The design no longer depends on it: N13 makes grants intention-scoped                                                                                                                                                                                                                         |
| ergo B1 (ABI gaps)                                              | Accepted. Status codes, `append_str`, `set_module`, `add_import`, slot classes with allowed child kinds, optional and once-only rules, generated constants, `PolicyArg` and `ArbitrationClause` (§6.1–6.2)                                                                                                                     |
| ergo M3 (cause design)                                          | Accepted. Fixed cause vocabulary (§10.3); detail goes to the `NativeCallFailed` percept and the log                                                                                                                                                                                                                            |
| ergo M4, writer M3a (sequence language)                         | Accepted. Grammar and six rules (§11.4); `delscreen` declared; `outf`/`inf`; synthesis named on the `library` line                                                                                                                                                                                                             |
| ergo M5 (read-back)                                             | Accepted. `len(text)`; buffer `n + 1`, bounded by `max_out_bytes`                                                                                                                                                                                                                                                              |
| writer M1, vortex M2 (checkpoint vs grants)                     | Accepted. A checkpoint changes nothing; stale-by-pid on resume; grants end at `stopped`; `checkpointed` removed                                                                                                                                                                                                                |
| writer M2 (stale test cannot fail)                              | Accepted. The `curses_keep` variant and exact ledger rows (§13.2 steps 8–9)                                                                                                                                                                                                                                                    |
| writer M3b, codebase M1 (`programRef` parts)                    | Accepted. Four parts: `kProgramFormat`, tag, op log (manifest inside), module digest                                                                                                                                                                                                                                           |
| vortex M3 (control-plane death)                                 | Accepted. `PR_SET_PDEATHSIG`, `_exit` on end-of-file, epoch-keyed staging and a sweep, and a gate                                                                                                                                                                                                                              |
| vortex M4 (live upgrade)                                        | Accepted. `upgraded` signal and rule (§12.4); cause `program_changed`                                                                                                                                                                                                                                                          |
| vortex M5 (handler drops read result)                           | Accepted. §11.6 adopts a `ReadKey` goal; §13.4 gate                                                                                                                                                                                                                                                                            |
| architect M1 (allowlist not enumerated)                         | Accepted. Enumerated allowlist plus a `LOG`-mode audit check (§8.3)                                                                                                                                                                                                                                                            |
| architect M2 (architecture/x32)                                 | Accepted. `ACT_BADARCH` set to kill; x32 bit kills; `int 0x80` probe                                                                                                                                                                                                                                                           |
| architect M3, codebase M2, zigzag R2.2b (ledger permascroll)    | Accepted. A private permascroll at `authorities/permascroll/`, a typed refusal if missing, and fail-closed `approved` (§10.5)                                                                                                                                                                                                  |
| codebase M3 (control-side glob)                                 | Accepted. Linux-gated files are filtered out, with a refusing substitute (§14.1, P20.5)                                                                                                                                                                                                                                        |
| zigzag R2.1 (decision-to-approval fan-in)                       | Accepted. Decisions quote spans and do not link; revocation is one-to-one (§10.5)                                                                                                                                                                                                                                              |
| zigzag R2.2a (withdrawal)                                       | Accepted. A `d.approval` event rank of `approve`/`withdraw`, with the tail as current state; `vagent native withdraw`                                                                                                                                                                                                          |

## MINOR

Applied:

- **writer:**
  - the C3 citation;
  - seal-command naming;
  - the allowlist;
  - the wording of the bindings row;
  - `vn_api` naming.
- **codebase:**
  - incarnation durability, stated as conditional on `reserveNodeId` and added to P20.8;
  - checkpoint consistency;
  - `PolicyArg` and `ArbitrationClause`;
  - `NativeSettings` moved to `LocalHost::Options` only;
  - the binding-name clash across programs, ruled harmless under per-agent routing (N11).
- **ergo:**
  - module and imports entry points;
  - `fail_if` gains `le`, `gt` and `ge`;
  - a naming rule for authority records.
- **vortex:**
  - a structural comparator;
  - a resume in a second `LocalHost` with a higher epoch;
  - the expiry of a cancelled `open_terminal`;
  - pid versus incarnation in frames.
- **zigzag:**
  - the operation counts derived;
  - idempotent approval;
  - the signing identity, which is the node's device key;
  - not system-xanadoc dimensions;
  - retention bounded by segment growth.

Not applied:

- **ergo m5** (`close_terminal` as cleanup). Unchanged: the example closes in the plan, and N13
  covers failure.
