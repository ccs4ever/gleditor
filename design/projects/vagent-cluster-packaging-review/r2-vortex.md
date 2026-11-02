# Round 2 critique: Vortex & Spool-Mesh Expert (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (B1 - Inline fast-path cannot reuse SpoolSliceBundle or ingestBundle): RESOLVED. §4 K2,
  §8.1–8.2, and P21.5 define a dedicated `PackageBundle` envelope on QUIC Lane 1 (demultiplexed via
  frame tag `0x02`), and declare a public `preparePackageBlob` method on `MeshHostTransport` and
  `MeshSwarmTransport` committing directly to CAS.
- R1.2 (B2 - Target-specific SONAME in op_log breaks logical programRef): RESOLVED. §4 K1 and §5.2
  add `name` and `canonical_soname` to each entry in `targets[<triple>].bundled_libs`, plus
  `system_library_aliases`, mapped at load and Step 7 `dlopen` time without mutating `op_log` or
  `programRef`.
- R1.3 (M1 - PendingProgramFetch drops messages to pending PIDs, lease expiry, and upgrade race):
  RESOLVED. §4 K5, §8.3, and P21.1 specify pre-admission mailbox buffering for pending `ActorPid`s
  (up to `grant.maxMailboxBytes`), crediting fetch wait time against lease expiration at admission,
  and suspending turn execution during live upgrade fetches.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- **Canonical encoding of omitted `system_library_aliases` (§5.2 line 420).** To guarantee
  bit-for-bit identical `targets_digest` calculation across packaging implementations, clarify
  whether a target slice with no system library aliases encodes `system_library_aliases` as an empty
  bencoded dictionary (`de`) or omits the key entirely.
- **`completePendingProgram` result parameter type (§8.3 line 776, §11.2 line 949, §12 P21.1).** In
  §8.3 and §11.2, specify the C++ parameter type for the completion outcome on
  `MeshActorRuntime::completePendingProgram` (`std::expected<void, ActorError>` or `bool success`).
- **Pre-admission queue overflow error code (§8.3 lines 762–764).** Clarify that when an incoming
  message for a pending `ActorPid` causes its pre-admission buffer to exceed
  `grant.maxMailboxBytes`, the message is rejected with `ActorError::MailboxFull` (consistent with
  `MeshActorRuntime::enqueue` at `mesh_actor.cpp:387`).

VERDICT: CONVERGED
