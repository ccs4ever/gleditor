# Code Audit: 2026-08-30 → 2026-09-01

Audit of the last three days of commits (`12d1acc..0ff5b4e`, 51 commits,
182 files, +27,564 / -639) for code quality and fitness for purpose, read
against the specifications in `design/`.

Build: clean (`make -j`, exit 0). Tests: 63 pass, 0 fail, including the
rootless netns swarm suite. **Nothing below was caught by the test suite**,
which is itself part of the finding.

## Status

Phases 1 and 2 of the remediation have landed. Everything in §2 (Critical),
§3 (High) except §3.1, and §4.2, §4.3 and part of §4.5 is fixed, each with a
test confirmed to fail against the code as audited. Three findings were
sharpened from "structural" to "demonstrated" in the process:

- **§2.2** — the Merkle second-preimage forgery, which this report hedged on,
  is real. `proof.verify(sha256Digest(leaf))` returned `true`.
- **§4.3** — the arena's unaligned `mmap(MAP_FIXED)` was confirmed to return
  `EINVAL` for the design's own worked example, `[10000, 25000)`.
- **§4.1** — `sizeof(CompactZZCell)` measured at 960 bytes, align 8.

Four defects not in this report were found while fixing it: two null-pointer
dereferences in `sendExtendedRaw` and `isolateAndDisconnect`, a challenge
nonce hardcoded to `0xAA`, `quarantinePeer` keyed on the disconnect *reason*
rather than the peer, and all three fuzz targets linking uninstrumented
objects so libFuzzer had no coverage to steer by.

**Still outstanding: §3.1 (span deduplication), §4.1, §4.4, §4.6, the rest of
§4.5, and §5** — Phase 3 of the plan.

---

## 1. Summary

The window contains two distinct code cultures, and the difference is the
most useful thing in this report.

**The older layer** — `resolver.cpp`'s read path, `publication.cpp`,
`swarm.cpp`, and the `btfs-and-permascrolls.md` design note — is careful
work. It verifies piece hashes before returning bytes, refuses to return
partial answers because "downstream cannot tell verified bytes from
unverified ones," and its design note opens with "an investigation, not a
proposal to adopt" and concludes *against* the thing it investigated.
Comments explain why a choice was made. `verifyPublication` really does
check an ed25519 signature over a canonical bencoding.

**The last three days' bulk additions** — `apps/xudu/core/identity/`,
the transcopyright plumbing, `compact_zzcell.hpp`, the span-dedup path —
have the opposite character. They are structurally elaborate and
functionally hollow: doc comments and design specs assert properties the
code does not have, security checks are present as syntax but disabled or
unreachable in practice, and the protocol handlers that the design's
sequence diagrams depend on decode a message and return `true`.

The single most important fact:

> **Across ~3,000 lines of new identity, oracle, and transcopyright code,
> not one signature is ever verified.** The only real signature check in
> `apps/xudu` is `swarm.cpp:104`'s BEP 44 `ed25519_verify`, which predates
> this window. `Signature64` values are constructed, bencoded, transmitted,
> and checked only for `isZero()`.

Every trust claim in `oracle-identity-model.md`, `bep10-wire-extensions.md`,
and the threat-model table in `permascroll-holes-and-transcopyright.md`
rests on verification that does not exist.

---

## 2. Critical

### 2.1 Peer authentication is a no-op, with a hardcoded fake credential

[`identity_network_controller.cpp:145`](apps/xudu/core/identity/identity_network_controller.cpp:145)
— responding to a challenge:

```cpp
Signature64 sig;
if (controller_->options().localSigningKey) {
  sig = *controller_->options().localSigningKey;   // a key, not a signature
} else {
  sig.bytes.fill(0x55);                            // ...or 0x55 repeated
}
sendAuthResponse(chRes->nonce, *controller_->options().localFingerprint, sig);
```

[`:159`](apps/xudu/core/identity/identity_network_controller.cpp:159) —
validating one:

```cpp
if (!respRes->claimedIdentity.isValid() || respRes->signature.isZero()) { ... }
isAuthenticated_       = true;
authenticatedIdentity_ = respRes->claimedIdentity;
```

Three compounding defects:

1. The response is never a signature *over the nonce*. When a signing key
   is configured the code copies the key blob verbatim — so the value is
   identical for every challenge and replayable by anyone who has seen one.
2. When no key is configured — the default path — it sends 64 bytes of
   `0x55`, which passes the receiver's `!isZero()` test.
3. The receiver performs no cryptographic verification at all.

**Any peer can authenticate as any fingerprint.** This is the gate that
`bep10-wire-extensions.md` and the "Sybil & DoS" row of the transcopyright
threat model are built on.

### 2.2 Merkle tree has no leaf/internal domain separation

[`identity_validation.cpp:16-95`](apps/xudu/core/identity/identity_validation.cpp:16).
Leaves hash as `SHA256(bencode)`; internal nodes hash as `SHA256(l || r)` —
the same construction, no `0x00`/`0x01` prefix (cf. RFC 6962 §2.1). This is
the classic Merkle second-preimage setup: a 64-byte "leaf" that is actually
a concatenation of two internal hashes verifies against a legitimate root.

`LedgerMerkleProof::verify` takes `leafHash` as a caller-supplied field and
never checks that it is a leaf, and `computeLeafHash(std::span<const
std::uint8_t>)` is a public overload accepting arbitrary bytes.

*Caveat, stated honestly:* exploitability via the typed overloads is
constrained because a leaf preimage must be valid bencode of an
`IdentityEntry`/`VoteEntry`. I did not construct a working forgery. The
structural weakness is real regardless and the fix is two bytes of prefix.

### 2.3 The transcopyright micropayment protocol does not exist

[`identity_network_controller.cpp:212-230`](apps/xudu/core/identity/identity_network_controller.cpp:212).
All four message types the design's sequence diagram depends on:

```cpp
case MessageType::TcInvoiceQuery:    return decodeTcInvoiceQuery(...).has_value();
case MessageType::TcInvoiceResponse: return decodeTcInvoiceResponse(...).has_value();
case MessageType::TcSettleRequest:   return decodeTcSettleRequest(...).has_value();
case MessageType::TcKeyDelivery:     return decodeTcKeyDelivery(...).has_value();
```

No invoice is generated, no payment ticket is verified (the design's step
"Seeder->>Seeder: Verify Payment Ticket"), no CEK is delivered, nothing is
written to LMDB. `permascroll-holes-and-transcopyright.md` §4 documents this
as an implemented pipeline. It is a decode-and-discard stub.

Likewise [`:297`](apps/xudu/core/identity/identity_network_controller.cpp:297),
`sendIdentityResponse(const IdentityEntry &entry, const LedgerMerkleProof &
/*proof*/)` — the inclusion proof is accepted as a parameter and dropped.
The proof never reaches the wire, so no receiver can verify ledger inclusion
even in principle. The `IdentityResponse` receive handler
([`:181`](apps/xudu/core/identity/identity_network_controller.cpp:181))
decodes and discards too.

### 2.4 CEKs stored world-readable in a predictable `/tmp` path

[`resolver.hpp:147`](apps/xudu/core/resolver.hpp:147) defaults `cacheDir` to
`/tmp/xudu_cache`; [`lmdb_cache.hpp:113`](apps/xudu/core/lmdb_cache.hpp:113)
opens the environment with mode `0644`.

Content Encryption Keys bought with real micropayments, plus decrypted paid
plaintext, land world-readable at a fixed path. Any local user can read
another's purchased keys, and `create_directories` on an attacker-precreated
`/tmp/xudu_cache` is a standard `/tmp` hijack. Should be under
`$XDG_CACHE_HOME`/`$XDG_DATA_HOME` with mode `0600`.

---

## 3. High

### 3.1 Span deduplication fabricates transclusions

This is the newest commit (`0ff5b4e`) and the finding I'd act on first after
the auth bypass, because it corrupts the data model rather than the
transport.

[`session.cpp:683`](apps/xudu/session.cpp:683) routes every flushed insert
through `findExistingSpan(op.text, minSpanDedupLength)` with a threshold of
24 bytes ([`session.hpp:454`](apps/xudu/session.hpp:454)). The
implementation ([`user_permascroll.cpp:137`](apps/xudu/core/user_permascroll.cpp:137))
is a flat `std::string_view::find` over the entire lifetime permascroll.

`links-vs-transclusions-computation-and-optimization.md` defines transclusion
as "**Pure sameness**… emergent physical fact of shared primedia
coordinates," "Ontological identity," with royalties settling "direct to
original primedia author." Shared span identity *is* transclusion — it is
what draws Identity Gold prisms and what transcopyright bills against.

Deduplication therefore manufactures ontological identity out of textual
coincidence. Two documents that both contain a 24-byte boilerplate line —
`"Copyright 2026 Example Corp."` is 28 — become permanently, visibly
transcluded from one another, and reads of one route royalties to whoever
typed it first. That is not an optimization of the model; it is a violation
of it.

Three further consequences:

- **Holes.** `spool_.bytes()` is the flat local view, which per the design
  retains private plaintext where the wire is zero-filled. Typing text that
  also occurs inside a withheld or `TranscopyrightLock` region mints a span
  pointing *into the hole*. The document then renders as a redaction
  blackout or a paid lock for text the author just typed in the clear — and
  the public EDL leaks the exact offset and length of private content.
- **Revocation.** Dedup can target a region tombstoned as
  `HoleReason::Revoked`, resurrecting a live reference to revoked data.
- **Cost.** A full linear scan of an append-only lifetime scroll — the same
  structure the design sizes at a 64 GiB arena — runs on the idle-flush path.

### 3.2 The transcopyright read path bypasses piece verification

[`resolver.cpp:191`](apps/xudu/core/resolver.cpp:191) calls
`source->readStream(...)` directly, where the plain path
([`:231`](apps/xudu/core/resolver.cpp:231)) goes through `readSegment()` and
`meta->verifyPiece()`. Unverified swarm bytes go straight into the
decryptor.

Poly1305 catches tampering, so this is not plaintext forgery — but it
collapses "hostile or corrupt seeder" into "you haven't paid yet," which is
the one distinction the UI needs, and it abandons the verify-before-return
discipline the rest of the file argues for in its own comments.

Adjacent, same block:

- [`:189`](apps/xudu/core/resolver.cpp:189) — `meta->totalLength() -
  segment->streamOffset` underflows (unsigned) if `streamOffset` exceeds
  total length; unguarded.
- [`:193`](apps/xudu/core/resolver.cpp:193) — bare `16` where
  `crypto::kTagSize` is used two lines above.

### 3.3 Hashcash: both anti-abuse invariants are off in production

[`identity_validation.cpp:559`](apps/xudu/core/identity/identity_validation.cpp:559).
`currentSystemTime` defaults to `0`, and the sole production caller
([`identity_network_controller.cpp:189`](apps/xudu/core/identity/identity_network_controller.cpp:189))
passes `0`. So:

- **Invariant 2 (clock skew / anti-premining) never runs** — `if
  (currentSystemTime > 0)`. Stamps of any age replay.
- **`pruneSpentCache` never runs** — same guard. `spentNonces` grows without
  bound, keyed on `(resource, nonce)` where `resource` is the
  attacker-supplied `targetEmail`. A remote peer drives unbounded memory
  growth against the component whose stated purpose is DoS resistance.

Also `difficultyBits > 256` on a `std::uint8_t` is tautologically false —
dead range check.

### 3.4 Oracle votes accepted from unauthenticated peers

[`identity_network_controller.cpp:175`](apps/xudu/core/identity/identity_network_controller.cpp:175)
— `OracleVoteBroadcast` decodes and forwards to
`controller_->handleIncomingVote()` with no `isAuthenticated_` gate and no
signature check on `VoteEntry::signature`. Consensus input is open to
anyone who completes a TCP handshake.

---

## 4. Medium

### 4.1 `CompactZZCell` is 960 bytes, not 64

Measured: `sizeof == 960`, `alignof == 8`. CLAUDE.md ("64-byte aligned
compact cell layout"), the design doc, and the struct's own comment
("High-density, zero-copy") all state 64.

It contains a `std::vector`, three `std::optional`s, and two `std::string`s
— including `std::string type{"cell"}` as a type tag (should be an enum) and
`ephemeralText`, which duplicates the primedia its own `span` field
addresses, contradicting "zero-copy … linking directly into Xudu primedia
spools" in the same comment. There is **no `static_assert` on the size**
anywhere in `apps/zigzag/core/` — the central invariant of the "120 FPS
staging" design is unenforced, which is why it drifted 15×.

### 4.2 `decryptSeekableSpan` does not seek

[`transcopyright_crypto.cpp:268`](apps/xudu/core/transcopyright_crypto.cpp:268)
decrypts the entire ciphertext and calls `substr`. The header promises it
"extracts the requested subspan without decrypting the entire file"; the
design doc gives the block-counter formula.

The implementation is the *safer* choice — you cannot authenticate a
Poly1305 tag over a fragment, so genuine seeking would mean unauthenticated
decryption. The design is what is wrong here. But the name and doc comment
now assert a performance property the caller relies on: `resolver.cpp`
re-decrypts the whole segment on **every span resolve**, in the render path.
Either rename to `decryptSpanSlice` and correct the doc, or adopt a real
chunked-AEAD framing (per-block tags) if seeking is actually required.

### 4.3 `VirtualMemoryArena` hole hot-swapping is unwired, and would not work

`mapZeroPagesFixed` and `remapSpanFixed` — the mechanism
`permascroll-holes-and-transcopyright.md` §3 describes for holes — have
**no production callers**; they appear only in `transcopyright_test.cpp`.

They also would not work as specified. `mapZeroPagesFixed`
([`virtual_memory_arena.cpp:157`](apps/xudu/core/virtual_memory_arena.cpp:157))
passes `targetAddr`/`length` to `mmap(MAP_FIXED)` without page alignment, so
it returns `EINVAL` for any hole not starting on a page boundary — and the
design's own worked example is `[10000, 25000)`. Even aligned, it would zero
whole pages, clobbering adjacent plaintext. `mapFileFixed` has the same
unaligned-`fileOffset` problem.

Minor, same file: `remapSpanFixed` is a `commitAnonymous` + `memcpy`, not a
remap; `flush()` calls `msync` on `MAP_PRIVATE|MAP_ANONYMOUS` memory, where
it is meaningless; `reserve()` omits the `MAP_NORESERVE` the design
specifies.

### 4.4 The LMDB content cache is write-only

`resolver.cpp:239` is the only production `cache.put(span, …)`. There is no
production `cache.get(span, …)` — `resolve()` never consults the cache
before doing the work. Confirmed by grep: every content `get` is in
`tests/xudu/lmdb_cache.cpp`.

So every resolve pays a full piece-verify plus an LMDB write whose result is
never read, and the cache grows without bound. The design's "Subsequent
reads … resolve instantaneously without network round-trips" is true only
for CEKs (which do have a `get_cek` at `resolver.cpp:181`).

### 4.5 Crypto hygiene

`transcopyright_crypto.cpp`:

- **Non-standard construction named as a standard one.** The header says
  "XChaCha20"; `deriveXChaChaSubkeys` substitutes HKDF-SHA256 over the first
  16 nonce bytes for HChaCha20. Defensible as a construction, but it is not
  XChaCha20 and will not interoperate with any implementation that is. Name
  it for what it does.
- **No zeroization.** `Key32 sharedSecret`, derived subkeys, and decrypted
  plaintext are left on the stack and in `std::string` buffers.
  `OPENSSL_cleanse` appears nowhere.
- `EVP_PKEY_derive`'s output `secretLen` is never checked to be 32
  (lines 369, 430).
- `wrapCek` binds only the ephemeral public key into the HKDF salt, not the
  recipient's.
- Doc/impl mismatch: the threat model claims nonces are "derived from the
  HKDF key identifier"; `generateNonce()` is random (which is better — fix
  the doc). But `resolver.cpp:196` *does* derive the nonce from the public
  `keyId`, so the two halves disagree with each other as well as with the
  spec.

### 4.6 Hole reasons collapse to one rendering

`resolver.cpp:223` maps `Withheld`, `Revoked`, `Takedown`, and `Unsealed`
alike to `WithheldRedacted`. The design gives them distinct legal and
editorial meanings; a reader cannot distinguish "the author is holding this
back" from "this was legally taken down." `holeRecord` is passed through, so
the information is available — nothing consumes it.

---

## 5. Low

- **`uncommitted_op_log.cpp:77`** — `out.back().text.resize(remainingLen)`
  truncates a `std::string` at a byte offset with no UTF-8 boundary check.
  If `at` is ever a byte offset (it is: `recordInsert` sets `length =
  utf8.size()`), backspacing into a multi-byte grapheme splits the encoding.
  The algebra itself I checked and believe correct — the insert/insert
  merge, the tail-truncation, the subsume-and-continue residual, and both
  delete/delete adjacency cases all preserve coordinates properly.
- **`uncommitted_op_log.hpp:39`** — `CompactedOp` sets `length = 0` for
  inserts and `text = {}` for deletes; two half-used fields where a
  `variant` would prevent misuse.
- **`identity_network_controller.cpp:198`** — `packet[5] =
  static_cast<char>(remoteExtId)`; `remoteExtId` is an `int` checked only
  `> 0`, silently truncated to 8 bits.
- **The head commit fails the blocking format gate.** `make format` on a
  clean checkout of `0ff5b4e` rewrites three files —
  `apps/xudu/core/uncommitted_op_log.{hpp,cpp}` and
  `tests/xudu/span_deduplication_test.cpp` — so `make format-check`, which
  CLAUDE.md lists as blocking in `.github/workflows/c-cpp.yml`, is red on
  `main`. All three are files added by the most recent commit; the run
  before committing was skipped.
- **CLAUDE.md** documents `apps/zigzag/core/compact_cell.hpp`; the file is
  `compact_zzcell.hpp`. Drift introduced in this window.
- **`design/` file map accuracy** — `permascroll-holes-and-transcopyright.md`
  §7's implementation table lists components (`virtual_memory_arena`,
  BEP 10 micropayment protocol) as delivered that are stubs or unwired.

---

## 6. Fitness for purpose

**What is sound.** The addressing model is right, and it is the part that
matters most. `btfs-and-permascrolls.md` argues that a span's address must
live in scroll coordinates rather than in whatever torrent happens to carry
it, and `Scroll`/`ScrollSegment`/`Resolver::readSegment` implement exactly
that — scroll coordinates in, stream coordinates out, with the segment as
the only thing a re-seal touches. Publication signing and verification are
real. Piece-hash verification on the plain read path is real and correctly
refuses partial answers. The swarm and mutable-name tests genuinely exercise
two network namespaces.

**What is not.** The three days' new work is scaffolding shaped like a
system. The identity, oracle, and transcopyright subsystems have complete
type hierarchies, bencode serializers, message enums, network controllers,
and 1,600 lines of tests — and no verification, no settlement, and no
delivery. The tests pass because they test the serializers against
themselves: `identity_test.cpp` fills signatures with `0x7A` and `0x3C` and
never asserts that a bad one is rejected, because nothing rejects one.

The gap between the design docs and the code is not the ordinary gap of
work-in-progress. `permascroll-holes-and-transcopyright.md` §7 is a table of
delivered components; several rows are stubs. The `CompactZZCell` comment
claims 64 bytes and zero-copy for a 960-byte struct holding two strings.
`decryptSeekableSpan` is named for an operation it does not perform. These
are not TODOs — they are assertions, and they will be believed by the next
person to read them, which is the thing that makes them expensive.

Worth noting the contrast in the docs themselves: `btfs-and-permascrolls.md`
investigates a technology and recommends against it, states what it could
not demonstrate and why, and marks one of its own earlier proposals as "the
wrong one." That is a document you can trust. The newer specs assert
throughout and hedge nowhere.

## 7. Suggested order of work

1. Implement real signature verification, or make its absence loud —
   `isAuthenticated_` should not be settable without it (§2.1, §3.4).
2. Add leaf/internal domain separation to the Merkle tree (§2.2). Two bytes.
3. Move the CEK cache off `/tmp` and to mode `0600` (§2.4).
4. Decide what span dedup is for (§3.1). If the goal is storage economy it
   belongs below the address layer, not at it; as written it changes what a
   transclusion *means*. I'd revert it pending that decision.
5. Pass a real clock into `HashcashEngine::verify` and drop the `> 0`
   guards, or delete the invariants rather than shipping them disabled
   (§3.3).
6. Reconcile the docs to the code — especially the §7 implementation table,
   the `CompactZZCell` size claim, and `decryptSeekableSpan`'s name. Add the
   `static_assert` that would have caught the 15× drift.
7. Add negative tests. Every finding above except §4.1 and §4.4 would be
   caught by one test that feeds bad input and asserts rejection.
