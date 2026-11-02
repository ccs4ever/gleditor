# Round 2 critique: Codebase Expert (vagent-cluster-packaging.md v0.3)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **R1.B1 (§4 K2, §8.2, §12 P21.5 — `.vnpkg` fast-path cannot reuse `SpoolSliceBundle`): RESOLVED.**
  Section 4 K2 (lines 222–228), §8.1–8.2 (lines 692, 718–724), and P21.5 introduce a dedicated
  `PackageBundle` network container (`{version: 1, manifest: <bytes>, blobs: {<sha256>: <bytes>}}`)
  transmitted over QUIC Lane 1 and demultiplexed via a 1-byte frame discriminator (`0x01` =
  `SpoolSliceBundle`, `0x02` = `PackageBundle`). P21.5 declares public `sendPackageBundle` and
  `ingestPackageBundle` on `MeshHostTransport` and `MeshSwarmTransport`, leaving `SpoolSliceBundle`
  and `decodeSpoolSliceBundle` (`mesh_value.cpp:746–765`) dedicated to Xanadu hypermedia slices.
- **R1.M1 (§4 K2, §5.2, §14.2 — GPG-specific `SignedProvenance` incompatible with Ed25519):
  RESOLVED.** Section 4 K2 (lines 204–207), §5.2 (lines 462–466), and §14.2 replace GPG TSV
  `SignedProvenance` citations with in-tree Ed25519 `PackageProvenance` using `xanadu::PublicKey`,
  `xanadu::identity::Signature64`, and `xanadu::verifySignature` (`common/xanadu/crypto.hpp`,
  `identity_layout.hpp`), matching the signature pattern of `PublishedValue`
  (`mesh_value.hpp:33–43`).
- **R1.M2 (§2, §4, §8, P21.5 — `prepareTorrent` private / absent from `MeshHostTransport`):
  RESOLVED.** Section 4 K2 (lines 229–232), §8.1 (line 696), §8.2 (lines 727–728), and P21.5 declare
  a public virtual method on `MeshHostTransport` and `MeshSwarmTransport`:
  `preparePackageBlob(const xanadu::InfoHash &hash, std::uint64_t maxBytes,`
  `const std::filesystem::path &destCasPath)`.
- **R1.M3 (§11.2, P21.1 — Undefined `ActorError::ProgramPending` & missing admission hook):
  RESOLVED.** Section 4 K5 (lines 289–296), §8.3 (lines 740–777), and P21.1 add
  `ActorError::ProgramPending` to `mesh_actor.hpp`, implement pre-admission buffering of incoming
  actor messages (`ActorSend`, `ActorDeliverValue`, `ActorDown`) to pending PIDs up to
  `grant.maxMailboxBytes`, credit fetch wait time against lease expiration, suspend turns during
  upgrades, and expose `completePendingProgram(programRef, result)` in `MeshActorRuntime`.
  `PendingProgramFetch` is cleanly layered inside `vagent/cluster/pending_fetch.{hpp,cpp}`.

## Status of previous MINOR findings

- **R1.m1 (Citation precision for resume check): RESOLVED.** Section 1.2 line 86 and §4 line 200
  correctly cite `mesh_actor.cpp:238, :322–324`.
- **R1.m2 (`COMMON_XANADU_SRCS` auto-discovery): RESOLVED.** P21.8 explicitly notes that
  `Makefile:633` auto-discovers all `.cpp` files under `apps/common/xanadu/`.
- **R1.m3 (`kProgramFormat` citation range): RESOLVED.** Section 4 line 168 and §5.2 line 442 cite
  `vagent/runtime/program.hpp:37–39`.
- **R1.m4 (`seed()` method ownership): RESOLVED.** Section 8.1 line 686 correctly cites
  `MeshSwarmTransport::seed()` (`mesh_swarm.hpp:91`).

## Verification of new v0.3 code citations and proposals

All 18 code citations and 8 proposal entries (`P21.1`–`P21.8`) in v0.3 were verified against the
live C++23 codebase:

- `mesh_actor.hpp:47–57` (`ActorGrant`), `:162–165` (`spawn`), `:114–120` (`ActorContinuation`),
  `:170–174` (`resume`), `:205–216` (`upgrade`), `:190–191` (`pumpOne`): all match exact lines.
- `mesh_value.hpp:33–43` (`PublishedValue`), `:68–74, :87–94` (`publishValue` overloads), `:167–172`
  (`computeSpoolMerkleLeaves`, `verifySpoolMerkleLeaves`): all match exact lines.
- `agent_executor.hpp:246` (`retainProgram`), `:247–248` (`resolveProgram`): exact match.
- `mesh_swarm.hpp:50–124` (`MeshSwarmTransport`), `:91` (`seed`), `:113` (`prepareTorrent`): exact
  match.
- `mesh_host.hpp:11–12` (non-blocking owner thread invariant), `:13–44` (`MeshHostTransport`):
  exact.
- `program.hpp:37–39` (`kProgramFormat`), `:151–152` (`LocalHost::Options`): exact match.
- `Makefile:633` (`COMMON_XANADU_SRCS := $(shell find apps/common/xanadu -name '*.cpp' ...)`),
  `:1334` (`TEST_PROGRAMS`): exact match.
- `system_docs.cpp:565` (`defaultSettingSpecs`): exact match for P21.7 settings integration.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- (§8.3 line 776, §11.2 line 949, §12 P21.1) **`completePendingProgram` signature precision:** In
  `vortex/mesh_actor.hpp`, declare the completion callback signature explicitly as:
  `void completePendingProgram(std::string_view programRef,`
  `std::expected<void, ActorError> result);` matching the idiomatic `std::expected` error
  propagation used across `MeshActorRuntime`.
- (§8.3 lines 762–764) **Pre-admission queue overflow error code:** When an incoming message
  targeting a pending `ActorPid` causes its buffered queue to exceed `grant.maxMailboxBytes`,
  specify that the message is rejected with `ActorError::MailboxFull` (consistent with
  `mesh_actor.cpp:387`).

VERDICT: CONVERGED
