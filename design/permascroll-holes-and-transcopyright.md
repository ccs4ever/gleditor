# Permascroll Publishing with Holes and Transcopyright Support

An architectural specification and design document for authorial withholding
("holes") and decentralized micropayment unlocks ("transcopyright") across the
`gleditor`, `xudu`, and `zigzag` systems.

---

## 1. Foundations: The Nelsonian Permascroll and the Right to Withhold

In Project Xanadu and Ted Nelson's foundational vision (as articulated in
*Literary Machines*, *Possiplex*, and *Byte Magazine* 1992), primedia is
organized into append-only, immutable coordinate spaces called **permascrolls**.
Every byte typed into an author's sovereign stream receives a permanent,
immutable coordinate:

$$\text{Address} = (\text{ScrollId}, \text{Offset})$$

Once assigned, an address must **never shift, re-index, or collapse**.
Universal transclusion depends strictly on coordinate space invariance: if an
author could delete or shift bytes, every downstream document quoting that
permascroll would silently suffer link rot or point to altered text.

### The Problem: Sovereign Authorship vs. Public Swarms

Real-world authorship requires private scratchpads, draft branches, confidential
notes, and embargoed revisions. In traditional publishing, private drafts are
siloed in separate files. In Xanadu, where an author maintains a single lifetime
permascroll, private primedia coexists in the same address space as published
works.

Furthermore, authors must have the legal, editorial, and economic right to:
1. **Withhold Spans**: Keep private notes or embargoed sections unreleased to the
   public BitTorrent swarm.
2. **Revoke Spans**: Cryptographically tombstone compromised or obsolete data.
3. **Comply with Legal Disputes**: Mark disputed text as taken down while
   preserving coordinate integrity.
4. **Monetize Spans (Transcopyright)**: Offer premium text that unlocks only
   when the reader pays per-byte micropayments directly to the author.

### Coordinate Space Invariance

When an author holds back bytes $[10000, 25000)$, the address space must **not**
contract. If addresses were re-indexed, byte 25000 would become byte 10000,
instantly breaking all surrounding Edit Decision Lists (EDLs).

Instead, the withheld range remains an explicit, permanent **Hole** in the
coordinate fabric:

```
Author Spool:
[ 0 ......... 9,999 ][ 10,000 ............ 24,999 ][ 25,000 .......... 50,000 ]
  Public Intro          Private Scratch / Secret       Public Body Chapter
  (Kind: Plain)         (Kind: Withheld / Hole)        (Kind: Plain)
```

Downstream readers transcluding span $[9500, 10500)$ receive verified public
bytes for $[9500, 10000)$, and an authenticated blackout/redaction placeholder
for $[10000, 10500)$, with zero coordinate shifting.

---

## 2. Ted Nelson's Transcopyright Doctrine

In 1992, Ted Nelson proposed **Transcopyright** as the economic foundation of
the Docuverse. Transcopyright fundamentally inverts traditional Digital Rights
Management (DRM):

- **Permissionless Quotation**: Anyone may quote or transclude any published
  passage without asking permission, negotiating licensing, or paying up-front
  fees.
- **Downstream Authors Never Pay**: Quoting authors are never billed for
  including transclusions in their documents.
- **Direct Reader-to-Author Micropayments**: When a final reader views a
  document, the reader's client pays the original author directly on a per-byte
  rendered basis.
- **Decentralized Settlement**: No publishing conglomerate or platform sits
  between the reader and the origin author.

In our implementation, Transcopyright is realized as an encrypted hole with
economic metadata: `HoleReason::TranscopyrightLock`. The swarm carries encrypted
ciphertext, while the decryption key is obtained via a direct peer-wire
micropayment exchange.

---

## 3. Storage and Cryptographic Architecture

### Spool and Addressing Model (`scroll.hpp`)

Scroll segments explicitly declare their carrier data kind and hole metadata:

```cpp
enum class HoleReason : std::uint8_t {
  Withheld           = 0x00, // Editorial / embargoed withholding
  Revoked            = 0x01, // Tombstoned / cryptographic revocation
  Takedown           = 0x02, // Legal takedown / dispute
  TranscopyrightLock = 0x03, // Ted Nelson Transcopyright: pay tokens to unlock
  Unsealed           = 0x04  // Local span not yet sealed into BitTorrent
};

enum class SegmentKind : std::uint8_t {
  Plain    = 0x00, // Cleartext verified torrent payload
  Withheld = 0x01  // Zero-filled wire payload or AEAD ciphertext
};

struct TranscopyrightDescriptor {
  std::uint64_t priceAtomicUnits{0}; // Price in nano-xu (1 XU = 10^9 nano-xu)
  std::array<std::uint8_t, 32> keyId{}; // Content Encryption Key ID
  std::string currencySymbol{"XU"};  // "XU", "SAT", etc.
  identity::Fingerprint authorWallet{}; // PGP / BEP 46 author identity
};
```

### BitTorrent v2 Merkle Piece Stability & Zero-Filling

BitTorrent v2 uses SHA-256 Merkle trees over 64 KiB blocks. If an author
omitted bytes from the torrent payload, block alignment and Merkle piece hashes
would diverge between the author's local file and the swarm torrent.

To guarantee cryptographic integrity:
1. **Wire Zero-Filling**: In `UserPermascroll::sealToTorrent`, withheld spans are
   filled with zeros on the wire/torrent file while the author's local memory
   retains the private plaintext.
2. **Ciphertext Streams**: For Transcopyright spans, the wire carries
   authenticated ChaCha20-Poly1305 ciphertext in place of the plaintext.
3. **Publication Manifests**: The author signs a Bencoded publication manifest
   declaring the exact byte offsets, lengths, cryptographic commitments, and
   pricing of all holes.

### Virtual Memory (`VirtualMemoryArena`)

The virtual memory subsystem reserves a sparse address space with
`mmap(PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS)` and commits segments into it as
they are opened. `SegmentedPrimediaSpool` and `SegmentedOpsSpool` map sealed
segment files into it with `mapFileFixed()`, both checking page alignment
first and falling back to a plain read when a segment does not qualify.

**Holes need no memory mapping, and an earlier version of this section was
wrong to say they did.** It described `mapZeroPagesFixed()` mapping zero pages
over a hole and `remapSpanFixed()` hot-swapping decrypted plaintext into one.
Neither function ever had a caller outside its own test, and neither could
have done that job: `mmap(MAP_FIXED)` requires page-aligned addresses and
lengths, a hole is an arbitrary byte range, and the worked example this
document uses — $[10000, 25000)$ — returns `EINVAL`. Page-aligning it would
not help either, since zeroing whole pages would flatten the surrounding
plaintext that shares them.

Both functions have been removed. What a hole actually does is simpler and
lives a layer up: `Resolver::resolve()` answers `WithheldRedacted` or
`TranscopyrightLocked` for the span, and the renderer draws over it. The
coordinate space is preserved by the segment table, not by the page tables.

### Cryptographic Subsystem (`transcopyright_crypto.hpp`)

The cryptographic engine provides:
- **ChaCha20-Poly1305 AEAD**: Authenticated encryption and decryption with 24-byte
  extended nonces.
- **Span Slicing (`decryptSpanSlice`)**: Decrypts a segment and returns the
  requested range of the plaintext.

  This was specified as seekable block decryption — set ChaCha20's counter to
  $\lfloor \text{OffsetInSegment} / 64 \rfloor$ and generate only the keystream
  the range needs — and named `decryptSeekableSpan`. **That specification was
  not implementable as written and was never implemented.** A Poly1305 tag
  authenticates a whole message, so decrypting a fragment in isolation means
  returning bytes nothing has vouched for, in a subsystem whose entire purpose
  is that a reader can trust what they paid for. The code always decrypted the
  whole segment; only the name and this paragraph claimed otherwise.

  Genuine random access needs a chunked AEAD framing with a tag per chunk.
  Until then, callers that resolve the same span repeatedly should cache the
  plaintext rather than decrypt per frame.
- **X25519 Key Encapsulation (KEM)**: Wraps Content Encryption Keys (CEKs) under
  recipient public keys.
- **HKDF-SHA256**: Deterministically derives per-span CEKs from master secret keys.

---

## 4. BEP 10 Peer-Wire Micropayment Protocol (`xudu_transcopyright`)

Unlocking Transcopyright content occurs over the BitTorrent peer wire using the
`xudu_transcopyright` BEP 10 extension.

```mermaid
sequenceDiagram
    autonumber
    actor Reader as Reader Node (Leecher)
    participant Seeder as Author / Seeder Node
    participant LMDB as Reader LMDB Cache

    Reader->>Reader: Resolver::resolve(span) -> TranscopyrightLocked
    Reader->>Seeder: TcInvoiceQueryMsg (keyId, requestedBytes)
    Seeder-->>Reader: TcInvoiceResponseMsg (keyId, priceAtomicUnits, authorWallet, paymentChallenge)
    Reader->>Reader: Generate Micropayment Ticket (xucoin / state-channel)
    Reader->>Seeder: TcSettleRequestMsg (keyId, paymentChallenge, ticket, payerPubKey)
    Seeder->>Seeder: PaymentVerifier::verify (interface; no backend implemented)
    Seeder-->>Reader: TcKeyDeliveryMsg (keyId, wrappedCek under payerPubKey)
    Reader->>LMDB: Put CEK into LMDB 'ceks' Table
    Reader->>Reader: Resolver::resolve(span) -> VerifiedBytes
```

### Message Structs (`identity_layout.hpp`)
1. **`TcInvoiceQueryMsg`**: Requests an invoice and pricing for a `keyId`.
2. **`TcInvoiceResponseMsg`**: Delivers pricing, wallet address, and an
   ephemeral `paymentChallenge`.
3. **`TcSettleRequestMsg`**: Provides the micropayment ticket, and the X25519
   public key the CEK is to be wrapped under.
4. **`TcKeyDeliveryMsg`**: Returns the wrapped CEK.

### What is implemented, and what settlement is not

The protocol above is implemented: an author registers an offer
(`IdentityNetworkController::offerTranscopyright`), invoices on request with a
single-use `paymentChallenge`, and on settlement wraps the CEK under the
payer's X25519 key. A reader that never asked for an invoice ignores one; a
challenge is spent when used, so a captured settlement request buys nothing
twice; a CEK wrapped for a different reader does not unwrap.

**Settlement itself is not implemented, and nothing here should be read as
claiming otherwise.** There is no xucoin, no state channel and no Lightning
node in this tree, so what a `micropaymentTicket` should contain is a question
about a system that does not exist yet. The decision is left behind a
`PaymentVerifier` interface:

- `RefusingVerifier` is the default. A node that has not been told how it gets
  paid refuses to sell, which is not the same as giving the work away.
- `AlwaysAcceptVerifier` is for development and warns on every call, because a
  seeder running it hands paywalled content to anyone who asks.

Implementing real settlement means implementing `PaymentVerifier` and nothing
else on this path.

### LMDB Storage (`lmdb_cache.hpp`)
The reader persists unlocked CEKs in LMDB table `ceks`, indexed by the 32-byte
`keyId`. Subsequent reads of the same span resolve instantaneously without
network round-trips.

---

## 5. 120 FPS Rendering and Spatial Integration

### 2D Document Model (`Doc` / `Page` / `Session`)

In `Session::decorate()`, the document renderer queries `Store::resolve(span)`:
- **Public Text (`VerifiedBytes`)**: Normal HarfBuzz text shaping and FreeType
  glyph caching.
- **Withheld Holes (`WithheldRedacted`)**: Generates an Obsidian blackout highlight
  quad (`0x111827FF`) across the withheld span.
- **Locked Transcopyright (`TranscopyrightLocked`)**: Renders an Amber Gold badge
  quad (`0xF59E0BCC`) with lock icon indicator and price metadata.

### 3D Optical Beams (`beams.cpp`)

When 3D link ribbons connect transcluded pages:
- **Standard Transclusions**: Render with **Identity Gold** volumetric bands
  (`0xFFD700FF`).
- **Transcopyright Ribbons**: Render with pulsating **Amber Gold** waves
  (`0xF59E0BFF`) modulated by real-time `pulsePhase` animation.
- **Obsidian Blackout Bands**: Render with matte, non-emitting charcoal bands
  (`0x1F2937FF`) indicating severed or withheld links.

### Zigzag Multidimensional Visualizer (`apps/zigzag`)

In the Zigzag visualizer:
- **`CompactZZCell`**: Tracks `resolutionStatus` and `transcopyrightInfo`.
  Cells containing withheld or locked text format their preview labels as
  `"[Redacted - Withheld]"` or `"[🔒 100 XU]"`.
- **`UnifiedTransclusionEngine`**: Staging sets cell paper background quads to
  Obsidian (`17, 24, 39`) for withheld cells and Amber Gold (`245, 158, 11`)
  for locked cells.
- **Manifold Invariance**: 2-rank manifold topology (`d.doc`, `d.version`,
  `d.clone`) remains strictly consistent regardless of hole states.

---

## 6. Threat Model and Security Analysis

| Threat Vector | Mitigation |
| :--- | :--- |
| **Length Leakage Attack** | The byte length of a hole is visible in the coordinate space. For ultra-sensitive secrets (e.g. passwords), authors pad withheld spans with random whitespace prior to sealing. |
| **Data Withholding / Fake Holes** | Seeder nodes cannot falsely claim a plain segment is a hole. Readers verify the author's PGP/BEP 46 signature on the Bencoded publication manifest. |
| **Sybil & DoS Attacks** | BEP 10 invoice queries are gated by dynamic Hashcash Proof-of-Work (`HashcashEngine`). |
| **Keystream Reuse** | A segment's nonce is derived from its `keyId` (`crypto::nonceForKeyId`), so a reader needs no nonce shipped alongside the ciphertext. This is safe *only because* a `keyId` names one CEK encrypting one segment — the uniqueness ChaCha20-Poly1305 requires is per key, not per nonce. Reusing a `keyId` across two spans reuses a keystream and leaks the XOR of both plaintexts to anyone holding both. Mint a fresh `keyId` per sealed span. |
| **Free-Riding / Key Theft** | Micropayments settle via atomic state-channel tickets. CEKs are cached locally per device. |

---

## 7. Implementation File Map

| Component | Files | Description |
| :--- | :--- | :--- |
| **Core Models** | [`apps/xudu/core/scroll.hpp`](apps/xudu/core/scroll.hpp) | `HoleReason`, `SegmentKind`, `TranscopyrightDescriptor`, `PublishedHoleRecord` |
| **Publication** | [`apps/xudu/core/publication.hpp/.cpp`](apps/xudu/core/publication.hpp) | Bencode hole serialization and manifest verification |
| **Cryptography** | [`apps/xudu/core/transcopyright_crypto.hpp/.cpp`](apps/xudu/core/transcopyright_crypto.hpp) | ChaCha20-Poly1305, whole-segment decrypt with slicing, HKDF, X25519 KEM |
| **Storage** | [`apps/xudu/core/lmdb_cache.hpp`](apps/xudu/core/lmdb_cache.hpp) | LMDB `ceks` key-storage table, owner-only under `$XDG_CACHE_HOME` |
| **Memory** | [`apps/xudu/core/virtual_memory_arena.hpp/.cpp`](apps/xudu/core/virtual_memory_arena.hpp) | Sparse reservation and page-aligned segment mapping. Not used for holes — see §3 |
| **BEP 10 Protocol** | [`apps/xudu/core/identity/`](apps/xudu/core/identity/) | Micropayment layout, serialization, and network controller |
| **Settlement** | [`apps/xudu/core/identity/payment_verifier.hpp`](apps/xudu/core/identity/payment_verifier.hpp) | **Interface only.** No settlement backend exists; the default refuses every payment |
| **Resolver & Store** | [`apps/xudu/core/resolver.hpp/.cpp`](apps/xudu/core/resolver.hpp), [`apps/xudu/core/store.hpp/.cpp`](apps/xudu/core/store.hpp) | Multi-span resolution, unlock pipeline, and caching |
| **2D / 3D Rendering**| [`apps/xudu/session.cpp`](apps/xudu/session.cpp), [`apps/xudu/beams.cpp`](apps/xudu/beams.cpp) | Obsidian blackout quads, amber ribbons, and pulse shaders |
| **Zigzag Engine** | [`apps/zigzag/core/compact_zzcell.hpp`](apps/zigzag/core/compact_zzcell.hpp), [`apps/zigzag/core/unified_transclusion_engine.cpp`](apps/zigzag/core/unified_transclusion_engine.cpp) | Holographic cell styling and manifold projection |
