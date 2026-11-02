# Round 1 critique: Codebase Expert (vagent-cluster-packaging.md)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 3 MAJOR, 4 MINOR.

## BLOCKING

### 1. (§4 K2, §8.2, §12 P21.5) `.vnpkg` inline fast-path cannot reuse `SpoolSliceBundle`

**Problem:** §4 K2 (lines 184–187), §8.2 (lines 591–596), and P21.5 (line 793) specify that cluster
packages $\le 1\text{ MiB}$ are packed into a `SpoolSliceBundle` (`mesh_value.hpp:158–164`) and
ingested directly via `sendBundle` / `ingestBundle` (`mesh_swarm.hpp:75–76, :90`). In C++,
`SpoolSliceBundle` and `ingestBundle` cannot process `.vnpkg` packages as specified without unlisted
modifications to `vortex/mesh_value.{hpp,cpp}`:

1. `decodeSpoolSliceBundle` (`mesh_value.cpp:746–765`) strictly enforces that the input bytes form a
   5-field bencode dictionary with keys `"carrier"`, `"history"`, `"manifest"`, `"payloads"`, and
   `"version"`. It further asserts `manifest` is a serialized `PublishedValue`
   (`receiveValue(bundle->manifestBytes, ...)`, `:897–899`).
1. A `.vnpkg` package starts with an 8-byte non-bencode magic header `\x89VAGNPKG\n` followed by an
   8-field package dictionary (`api_version`, `format`, `kind`, `compiler_format`, `program_body`,
   `targets`, `provenance`, `signature`). Passing `.vnpkg` bytes to `ingestBundle` triggers an
   immediate decode error (`"spool slice: invalid canonical bundle envelope"`).
1. On QUIC Lane 1, `MeshSwarmTransport::pollDirectChannel` (`mesh_swarm.cpp:555–558`)
   unconditionally passes incoming frame payloads to `(void)ingestBundle(quicFrame->payload)`. It
   contains no frame header or discriminator to route package bundles separately from store slice
   bundles.
1. P21.5 in Table 12 lists only `vortex/mesh_swarm.{hpp,cpp}` and `vortex/mesh_host.{hpp,cpp}` in
   its "Where" column; it omits `vortex/mesh_value.{hpp,cpp}` and provides no specification for how
   wire decoding demultiplexes `.vnpkg` packages from state spool bundles.

**Evidence:**

- `design/vagent-cluster-packaging.md` §4 K2 (lines 184–187), §8.2 (lines 591–596), §12 (P21.5)
- `apps/common/xanadu/vortex/mesh_value.hpp:158–164`
- `apps/common/xanadu/vortex/mesh_value.cpp:746–765`, `:887–902`
- `apps/common/xanadu/vortex/mesh_swarm.cpp:555–558`, `:700–713`

**Fix:** Either: (a) Extend P21.5 to include `vortex/mesh_value.{hpp,cpp}` and specify how `.vnpkg`
is encapsulated within `SpoolSliceBundle` (e.g. as a payload slice alongside a synthetic
`PublishedValue`), OR (b) Add a distinct QUIC Lane or frame type prefix on direct stream channels
for `.vnpkg` packages (e.g. `sendPackageBundle` / `ingestPackageBundle`) routed to
`package_cas.cpp`, keeping `SpoolSliceBundle` dedicated to Xanadu hypermedia store slices.

## MAJOR

### 1. (§4 K2, §5.2, §14.2) GPG-specific `SignedProvenance` incompatible with Ed25519

**Problem:** §4 K2 (line 180), §5.2 (lines 326–332, 357), and §14.2 (line 856) state that package
provenance matches `xanadu::SignedProvenance`, cite `mesh_value.hpp:33–43` and
`common/xanadu/publication.hpp`, and assert that corrupted signatures fail `verifyProvenance`. In
the live codebase:

1. `mesh_value.hpp:33–43` defines `struct PublishedValue`, NOT `SignedProvenance`.
1. `struct SignedProvenance` is defined in `common/xanadu/provenance.hpp:122–128` (not
   `publication.hpp`). It holds `{ std::string tsv; std::string signatureArmored; }`, representing a
   GPG ASCII-armored detached signature over TSV text.
1. `verifyProvenance` (`provenance.hpp:221–223`, `provenance.cpp:322–348`) spawns the external `gpg`
   binary via `posix_spawn` to verify GPG keyrings. It cannot verify an Ed25519 signature over a
   bencoded dictionary.
1. In §5.2, `.vnpkg` explicitly defines a 32-byte Ed25519 public key hex in `"provenance"` and a
   64-byte Ed25519 signature hex in `"signature"` over prefix `"xanadu:vagent-package:1:"` +
   bencode. In Xanadu, in-tree Ed25519 verification is performed via `xanadu::verifySignature` or
   `MutableKeys` (`common/xanadu/crypto.hpp`, `identity_layout.hpp`), not `verifyProvenance`.

**Evidence:**

- `apps/common/xanadu/vortex/mesh_value.hpp:33–43` (`PublishedValue`)
- `apps/common/xanadu/provenance.hpp:122–128` (`SignedProvenance`), `:221–223` (`verifyProvenance`)
- `apps/common/xanadu/provenance.cpp:322–348` (`runGpg` execution)
- `design/vagent-cluster-packaging.md` §4 (line 180), §5.2 (lines 326–332, 357), §14.2 (line 856)

**Fix:** Correct the references: remove `SignedProvenance` and `verifyProvenance`. Specify that
`.vnpkg` provenance uses `xanadu::PublicKey` and `xanadu::identity::Signature64` verified in-tree
via `xanadu::verifySignature`. Update the citation of `mesh_value.hpp:33–43` to refer to
`PublishedValue` or `provenance.hpp:122` where appropriate.

### 2. (§2, §4, §8, §14, P21.5) `prepareTorrent` is private and absent from `MeshHostTransport`

**Problem:** §2 (line 124), §4 (line 189), §8.1 (diagram line 571), §8.2 (lines 598–600), and §14.1
(line 840) specify that missing packages are pulled by calling
`prepareTorrent(infoHash, maxBytes) (mesh_swarm.hpp:113)` or `SwarmContentSource::prepareTorrent`.

1. `prepareTorrent` is declared under `private:` in `MeshSwarmTransport` (`mesh_swarm.hpp:113`). It
   is strictly an internal helper called by `prepare` and `prepareValue`.
1. `SwarmContentSource` (`apps/common/xanadu/swarm.hpp:101–230`) has no `prepareTorrent` method; its
   API consists of `addTorrent`, `addMagnet`, `waitForMetadata`, etc.
1. The abstract interface `MeshHostTransport` (`apps/common/xanadu/vortex/mesh_host.hpp:13–44`)
   declares no `prepareTorrent` or package resolution method.
1. Calling `prepareTorrent` across classes will fail compilation.

**Evidence:**

- `apps/common/xanadu/vortex/mesh_swarm.hpp:94, 113` (`prepareTorrent` is `private`)
- `apps/common/xanadu/vortex/mesh_host.hpp:13–44` (`MeshHostTransport` lacks `prepareTorrent`)
- `apps/common/xanadu/swarm.hpp:101–230` (`SwarmContentSource` lacks `prepareTorrent`)

**Fix:** Update P21.5 to declare a public virtual method on `MeshHostTransport` (and implemented in
`SwarmHostTransport` and `MeshSwarmTransport`), such as:

```cpp
[[nodiscard]] virtual std::expected<void, std::string>
preparePackage(const xanadu::InfoHash &hash, std::uint64_t maxBytes);
```

Update all text and sequence diagrams in §2, §4, §8.1, §8.2, and §14.1 to call this public method.

### 3. (§11.2, P21.1) Undefined `ActorError::ProgramPending` and missing admission protocol

**Problem:** §11.2 (lines 771–773) states: "When `receive()` processes `ActorSpawn` or
`ActorContinuation`, it invokes the registered program resolver. If the resolver returns
`ProgramPending`, the message is queued in `PendingProgramFetch`."

1. `ActorContinuation` is not an `ActorMessage` wire type; `receive()` (`mesh_actor.cpp:395–418`)
   only receives `ActorSpawn`, `ActorSpawnAck`, `ActorSend`, etc. Continuation data is received via
   `resume()`.
1. `enum class ActorError` (`mesh_actor.hpp:29–43`) does not contain `ProgramPending`.
1. `ProgramManifoldResolver` (`mesh_actor.hpp:138–139`) returns `const zigzag::Manifold *`, which
   can only signal found or `nullptr`.
1. In `MeshActorRuntime::receive()` (`mesh_actor.cpp:406–417`), receiving `ActorSpawn` immediately
   calls `spawn()` and emits an `ActorSpawnAck` refusal if `spawn()` fails. If `ProgramPending` is
   returned without an explicit protocol branch in `receive()`, the node will immediately emit a
   `"spawn refused"` rejection ACK back to the sender rather than deferring it.

**Evidence:**

- `apps/common/xanadu/vortex/mesh_actor.hpp:29–43` (`enum class ActorError`)
- `apps/common/xanadu/vortex/mesh_actor.hpp:138–139` (`ProgramManifoldResolver`)
- `apps/common/xanadu/vortex/mesh_actor.cpp:406–418` (`receive()` spawn dispatch and ACK emission)

**Fix:** In P21.1, explicitly specify:

1. Adding `ProgramPending` to `enum class ActorError` in `mesh_actor.hpp`.
1. Updating `receive()` in `mesh_actor.cpp` so that when `spawn()` returns
   `ActorError::ProgramPending`, it suppresses the immediate `ActorSpawnAck` rejection and enqueues
   the message in `PendingProgramFetch`.
1. Defining the exact signature of `AsyncProgramResolver`:

```cpp
using AsyncProgramResolver = std::function<
    std::expected<const zigzag::Manifold *, ActorError>(std::string_view programRef)>;
```

## MINOR

- (§1.2 lines 85–86) **Citation precision for `programRef` check in resume:** The text cites
  `mesh_actor.cpp:322–324`, which checks `actor->pid == continuation->pid`
  (`ActorError::WrongEpoch`). The check that `continuation->programRef != request.programRef` occurs
  at `mesh_actor.cpp:238`. Update citation to `:238, :322–324`.
- (§12 P21.8 line 796) **`COMMON_XANADU_SRCS` automatic discovery:** P21.8 lists "Makefile rules
  compiling `apps/common/xanadu/vagent/cluster/` sources into `COMMON_XANADU_SRCS`". In
  `Makefile:633`, `COMMON_XANADU_SRCS` is defined as
  `$(shell find apps/common/xanadu -name '*.cpp' 2>/dev/null)`, so any `.cpp` in the new cluster
  directory is picked up automatically without new Makefile compile rules.
- (§1.2 line 79, §4 line 160, §5.2 line 304) **`kProgramFormat` citation range:** `kProgramFormat`
  spans lines 37–39 in `apps/common/xanadu/vagent/runtime/program.hpp`. Use `:37–39` rather than
  `:39`.
- (§8.1 diagram step 10 line 562) **`seed()` method ownership:** The sequence diagram states
  `NA->>ST: Seed payloads ... via SwarmContentSource::seed()`. `SwarmContentSource` has no `seed()`
  method; `seed(const ValueSeed &)` is a method of `MeshHostTransport` (`mesh_host.hpp:43`) and
  `MeshSwarmTransport` (`mesh_swarm.hpp:91`). Update diagram text to cite
  `MeshSwarmTransport::seed()`.

VERDICT: NOT CONVERGED
