# Critique Log: `design/vortex-transport-and-or-parallelism-study.md`

**Reviewers**:

1. **System Architect & Codebase Expert** (`gleditor` / `xuzz` / `vortex`)
1. **Senior Technical Writer**

---

## 1. System Architect & Codebase Audit Findings (All Resolved)

1. **`identity_network_controller.cpp:180` $\to$ `:649` (`pc_.send_buffer`)**:
   Corrected line citation for `pc_.send_buffer()` from `:180` (which is inside `on_extended`) to
   `:649` (inside `sendPacket`), and separated cumulative fragment waiting (`:174-178`) from actor
   message dispatch (`:205-232`).
1. **`identity_layout.hpp:457-479` $\to$ `:498-522` (`PeerChallenge`)**:
   Updated line range for `PeerChallenge` and `PeerChallengeResponse` to
   `identity_layout.hpp:498-522`.
1. **`MutablePointer` Member Name (`swarm.hpp:45-55`)**:
   Corrected `InfoHash carrier` to `InfoHash hash` matching `struct MutablePointer` at
   `swarm.hpp:46`.
1. **`SegmentedOpsSpool` Physical Segments vs. `Store` Logical Partition Key**:
   Clarified in Table §2.3 that `segmented_ops_spool.hpp:221-227` defines physical `SegmentInfo`
   descriptors while `store.hpp:52-78, 169` owns the logical `(StoreId, AuthorKey, Branch)`
   partition identity.
1. **`Store::rebuildManifold()` Citation (`store.hpp:413`, `store.cpp:486`)**:
   Replaced `manifold.hpp:330` with the actual declaration and definition in `store.hpp:413` and
   `store.cpp:486`.
1. **Model 2 (`[0..mark)` Arena Prefix Hydration) Internal Invariants**:
   - Clarified that `slots_[0..mark.cellCount)` stores the shadowed `CellSlot`s (with
     `slots_[dense].birthOp` holding the original base `CellRef`, `arena_manifold.cpp:1488`), while
     `overlay_` (`std::unordered_map<CellRef, std::uint32_t>`, `arena_manifold.hpp:715`) must be
     reconstructed on the receiving worker so `ArenaManifold::denseOf(ref)`
     (`arena_manifold.cpp:258-270`) does not fall through to `base_`.
   - Added explicit hydration requirements for `liveLinks_`, `liveContent_`, `floors_` (split floor
     for trailing parent slots), `arenaDims_`, federation/quoting prefixes, and
     `VortexVM::cursorPC_`.
   - Added the intra-machine concurrency caveat (snapshot on spark publish or copy at cooperative
     scheduler yield).
1. **Primedia Span Invariant for `SegmentedOpsSpool` Streaming**:
   Specified that because `CompactOpNode` (`compact_op.hpp:28-65`) is an EDL node (`scrollId`,
   `spanStart`, `spanLength`), streaming `rawOps()` slices must be paired with an inline
   `ScratchSpan` buffer or BLAKE3/Bao verified slice whenever operations reference newly authored
   bytes in `localScroll` or `ArenaManifold::scratch_`.
1. **Dedicated UDP Port (`quic_port`) & `GEMINI.md` C++ vs. Vortex Justification**:
   Specified that `MeshQuicTransport` binds a dedicated UDP port advertised in BEP 10
   `add_handshake` (avoiding kernel UDP demux conflicts with `libtorrent`'s uTP/DHT socket), and
   added §4.2 justifying the C++ substrate vs. Vortex standard-library boundary per `GEMINI.md`.

---

## 2. Senior Technical Writer Findings (All Resolved)

1. **`Promoted` vs. `ValueSeed` Terminology**:
   Distinguished in-memory `Promoted` (`arena_manifold.hpp:237`) from the multi-torrent on-disk
   `ValueSeed` (`mesh_value.hpp:50`) produced by `publishValueImpl`.
1. **Inbox Queue Default Limit (`mesh_swarm.hpp:29-30`)**:
   Corrected default event limit from 1,024 to 256 (`maximumActorEvents = 256`).
1. **Heartbeat Method vs. Wire Frame (`mesh_actor.cpp:580`)**:
   Clarified that `MeshActorRuntime::heartbeat` (`mesh_actor.cpp:580`) is the runtime C++ method
   driven by periodic `ActorAck` / `ActorSend` pings rather than a separate `ActorHeartbeat` enum in
   `identity_layout.hpp:272-279`.
1. **VM Citations (`vortex_core.hpp:256`, `vortex_vm.cpp:174, 902, 1079`)**:
   Updated `arena_` to `vortex_core.hpp:256`, `cutBarrier` push to `vortex_vm.cpp:174`,
   `OpcodeKind::Cut` dispatch to `vortex_vm.cpp:902`, `stepScheduler()` to
   `vortex_vm.hpp:127` / `vortex_vm.cpp:1079-1110`, and marked `cyclesSinceRoot` as a Stage 3
   addition to `ChoicePoint`.
1. **Section Transitions & `d.branch` Coverage**:
   Added bridging prose between §1 $\to$ §2 and §2 $\to$ §3, and included `d.branch` worker-PID
   binding in §3.3 Model 4.
1. **Markdown & Style (`MD013`, `MD022`, `MD029`, `MD031`, `MD032`)**:
   Normalized ordered lists to `1.`, added blank lines around all headings and fenced `math` blocks,
   and wrapped all prose lines at $\le 100$ columns.
