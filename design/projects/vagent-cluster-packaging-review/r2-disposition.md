# Round 2 disposition (`design/vagent-cluster-packaging.md` v0.3 → v0.4)

All seven critics (`writer`, `vortex`, `codebase`, `architect`, `ergo`, `zigzag`, `security`)
converged in Round 2 with **0 BLOCKING** and **0 MAJOR** findings:

| Critic      | BLOCKING | MAJOR | MINOR | Verdict   |
| ----------- | -------- | ----- | ----- | --------- |
| `writer`    | 0        | 0     | 5     | CONVERGED |
| `vortex`    | 0        | 0     | 3     | CONVERGED |
| `codebase`  | 0        | 0     | 2     | CONVERGED |
| `architect` | 0        | 0     | 2     | CONVERGED |
| `ergo`      | 0        | 0     | 3     | CONVERGED |
| `zigzag`    | 0        | 0     | 0     | CONVERGED |
| `security`  | 0        | 0     | 0     | CONVERGED |

All unique Round 2 `MINOR` polish items are **ACCEPTED** and applied in v0.4:

1. **Header review record link (`writer` m2):** Point line 6 of `design/vagent-cluster-packaging.md`
   to `projects/vagent-cluster-packaging-review/README.md`.
1. **Crypto header citations (`writer` m1):** Cite `identity/identity_layout.hpp:220`,
   `mutable_link.hpp:58`, and `identity/standard_crypto_engine.hpp` instead of
   `common/xanadu/crypto.hpp` in §4 K2, §5.2, and §9.1.
1. **Canonical encoding of `system_library_aliases` (`writer` m4, `vortex` m1, `ergo` m2):** Specify
   in §5.2 that `system_library_aliases` keys are sorted lexicographically by raw byte value per
   BEP-3, and when a target has no aliases it is encoded as an empty dictionary (`de`) so
   `targets_digest` is deterministic.
1. **`completePendingProgram` signature & thread marshaling (`vortex` m2, `codebase` m1, `architect`
   m1):** Specify `completePendingProgram(std::string_view, std::expected<void, ActorError>)` in
   §8.3, §11.2, and P21.1, invoked on the runtime thread driving `MeshActorRuntime`.
1. **Pre-admission queue overflow error code (`vortex` m3, `codebase` m2):** Specify in §8.3 that
   when an incoming message for a pending `ActorPid` causes its pre-admission buffer to exceed
   `grant.maxMailboxBytes`, it is rejected with `ActorError::MailboxFull` (`mesh_actor.cpp:387`).
1. **CAS eviction low watermark (`architect` m2):** Document in §6.2 and §13 that when unpinned CAS
   bytes exceed `vagent.cluster.cas_max_bytes` (default `10 GiB`), LRU eviction sweeps down to an
   80% low watermark (`8 GiB`) to avoid thrashing on every incoming blob.
1. **CLI `--kind` in authority approval examples & `--bundle-dir` resolution (`writer` m3/m5, `ergo`
   m1/m3):** Add `--kind terminal` to the `vagent native approve` and `withdraw` examples in §10.3,
   and state in §10.1 that `--bundle-dir` scans the directory, validates basenames against the K3
   regex allowlist, extracts `DT_SONAME`s, and computes the topological dependency order
   automatically.
