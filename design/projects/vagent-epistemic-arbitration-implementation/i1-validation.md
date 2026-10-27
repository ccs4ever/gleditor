# I1: generic math and epistemic ABI foundation

Source baseline: `80ebaef1`. The [21-file source manifest](i1-source-sha256.txt) has SHA-256
`de1a44708840bf6f3965b33669bfeab8c00a8665ae5e27df9a35d1ece8d16fa3`. Independent full review:
[ML expert gate](ml-gate-i1.md), CONVERGED with zero BLOCKING/MAJOR.

Implemented:

- Append Exp/Ln/Sqrt without renumbering existing opcodes. Require one finite Float input, valid
  logarithm/square-root domain and finite output; permit finite-zero exponential underflow. Contract
  failure publishes no output. Add real std:math opcode exports and strict new-call arity.
- Update decoder/name audits; persisted pure-job artifacts deliberately retain their existing Cut
  limit and reject the new opcodes, including tampered wire input.
- Register d.trust/d.epistemic, logical source/candidate defaults and schema notes; validate signed
  configuration and host-provided zero bounds before actor creation. These are admission-policy
  foundations; observation-count enforcement arrives with ingestion, not in this iteration.
- Change the compiler program-format domain to vagent-program-2 for the new compiled graph shape.
  CompactOpNode, store formats and existing opcode numbers are unchanged.

Validation (headless with isolated XDG state):

- `make -j$(nproc) vagent vagent_actor_test vagent_runtime_test build/xuzz_test`: passed.
- `make -j$(nproc) test/vagent-actor test/vagent-runtime`: 596 actor and 112 runtime tests passed.
- xuzz_test filter SystemDocsTest.*, VortexStdLibTest.*, VQL\*.*, VPL*.\*: 204 passed.
- Tests cover finite/domain/type/arity/overflow/underflow, unchanged output on failure, persisted
  opcode decoding in a fresh arena, real graph exports, settings defaults and invalid host bounds.
- Changed C++ passes clang-format 19; Markdown passes mdformat with required plugins and mdl;
  whitespace checks pass. No full repository/network suite is claimed for this prerequisite slice.

The provisional build failure (copied range variable under warnings-as-errors) was fixed. A first
persistence fixture incorrectly assumed ordinary integer operand refs would be promoted; it now
restores each opcode and binds fresh context-local operands, matching the current runtime boundary.
The expert reread the corrected source and successful results before issuing PASS.

No scoring, priors, calibration, learning or native-model behavior is implemented or enabled here.
Typed epistemic query refusals, scoped results, migration, checked ingress and later gates remain
pending. Numeric precondition faults here are the generic VM contract, not a completed P19 query
API.
