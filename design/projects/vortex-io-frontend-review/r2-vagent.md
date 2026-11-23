# Round 2 critique: VAgent (vortex-io-redesign.md)

Paths below are relative to the repository root. Target verified: v0.10, SHA-256
`fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`.

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (message operations lacked a normative boundary): RESOLVED — optional message capability now
  specifies one complete message per successful receive, one frozen message per send, preservation
  through coalescing, zero-length messages distinct from EOF, typed refusal before admission, and
  explicit oversize disposition (`design/vortex-io-redesign.md:393-406,548-563,1050`).

## VAgent follow-up

The optional message contract resolves my R1 concern without requiring every backend to implement
message transport. VAgent can stage a message-sending effect as an actor intent; after commit its
adapter submits a frozen single-message operation, and the request keeps its own identity and
outcome. A stream-only adapter can typed-refuse the requested message capability before admission.
The draft also makes oversized receive behavior explicit: `MessageTooLarge` cannot masquerade as a
complete partial message, and the adapter states whether the message was consumed/discarded or
retained for a later bounded receive (`design/vortex-io-redesign.md:401-406,548-563`).

Actor semantics remain consistent. The mesh callback is atomic and rejects direct suspending
`Submit`/`Await`; handler effects remain reversible staged intents and expose no external work
before commit. Their immutable byte/order meaning is fixed before commit; post-commit admission
transfers or retains a bounded payload lease and durably records metadata before service work.
Restore checks proof before invoking the executor, refuses missing or uncertain proof, and never
silently retries; crash uncertainty remains `Unknown`. Completion is a later owner turn with
request/correlation and outcome cells, not a stale `CellRef`
(`design/vortex-io-redesign.md:264-301`). The generic VM/core remains language-neutral; the VAgent
adapter maps staged intents, and a local-runner host profile may use its own explicit submit point
and VAgent-local continuation (`:343-356,585`).

The frozen Write/Flush example and buffer rules are also coherent: actor staging freezes semantic
bytes and endpoint order before commit; post-commit admission creates the durable attempt. This is
distinct from VLace pure local buffer construction plus `perform(flush buffer)`, and from a
service-owned buffered request that already has an identity before native coalescing. `FlushAfter`
is an ordered asynchronous barrier over earlier writes, not buffer construction; durable `Sync` is
separate, and graceful close versus abort/cancel retains partial and unknown progress
(`design/vortex-io-redesign.md:531-546,585,665-681,702-707`).

A direct actor turn still does not await: completion and token/frame input are delivered to later
owner turns, while frontend tokenization stays outside core. Shared Pending, terminal outcomes,
awaiter-versus-request cancellation, last-owner cleanup, and bounded release semantics continue to
apply (`design/vortex-io-redesign.md:256-260,296-301,343-356,657-707,721-750,990-1004`). This fits
VAgent's existing split between staged handler effects, suspended plan effects, and its pure guard /
read-only contexts (`design/vagent-agent-language.md:187-199,860-885,1123-1135`).

Current-source evidence remains the same as R1: the executor stages an outbox entry and correlation
for effects (`apps/common/xanadu/vagent/executor.cpp:1264-1328`); the adapter currently drains after
the executor returns and invokes native effects inline with mutable worker context
(`apps/common/xanadu/vagent/adapter.cpp:406-408,662-710`). v0.10 is a proposed replacement contract,
not a claim that this path is already asynchronous. The current standalone runner uses a single-node
`MeshActorRuntime` (`apps/vagent/main.cpp:155-160`), but ABI is explicitly free and the target
separates generic actor-profile refusal from frontend-owned continuation policy
(`design/vortex-io-redesign.md:60,343-356`).

## Cross-role dialogue

- **VLace:** I asked whether its R1 buffer/flush/close distinction still matches v0.10. VLace
  confirms `perform(flush buffer)` is one fresh admitted write request; `FlushAfter` is a separate
  fence; `Sync`, graceful close, and `Abort` are distinct. Abandoning a continuation releases
  frontend waiter state but leaves an admitted request and its partial/unknown outcome
  service-owned. This confirms the v0.10 reading above; no VAgent finding remains.
- **Technical writer:** I replied that `stage effect Write` followed by
  `stage effect Flush{after:lastWrite}` is a staged write plus later ordering-barrier intent, not
  buffer construction (`design/vortex-io-redesign.md:585,674-681`). The writer's R2 Pending/payload/
  Abort issue is confined to expanded VPL examples; VAgent receives an actor completion in a later
  turn and has no corresponding inline `Seen.payload` or catch-all abort path. No VAgent example
  finding remains.
- **Wildcard:** Its R2 review confirms the optional message-mode contract and oversize/refusal rules
  resolve the shared gap. VProlog and VIcon R2 reports independently verify the same semantics.
  Message capability is generic transport policy; VAgent's effect grammar and staged-admission
  mapping stay frontend-owned.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

VERDICT: CONVERGED
