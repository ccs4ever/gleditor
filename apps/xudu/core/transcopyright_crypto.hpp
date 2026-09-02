/**
 * @file transcopyright_crypto.hpp
 * @brief Cryptographic engine for Transcopyright micropayments and Permascroll
 * Holes.
 *
 * Implements:
 * 1. AEAD Encryption/Decryption: ChaCha20-Poly1305 / XChaCha20 authenticated
 * encryption.
 * 2. Whole-segment AEAD decryption with plaintext slicing.
 *
 * 3. Hierarchical Key Derivation: HKDF-SHA256 derivation of SpanCEK from
 * SegmentMasterKey.
 * 4. HPKE / X25519 Key Encapsulation: Wrap/unwrap CEKs for peer-to-peer
 * delivery over BEP 10.
 * 5. Content Commitments: SHA-256 Merkle root computation over withheld
 * primedia.
 */
#ifndef XUDU_TRANSCOPYRIGHT_CRYPTO_HPP
#define XUDU_TRANSCOPYRIGHT_CRYPTO_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xudu::crypto {

/// Standard cryptographic dimensions
constexpr std::size_t kKeySize   = 32; ///< 256-bit symmetric key
constexpr std::size_t kNonceSize = 24; ///< 192-bit extended nonce
constexpr std::size_t kTagSize   = 16; ///< 128-bit Poly1305 tag
constexpr std::size_t kBlockSize = 64; ///< 64-byte ChaCha20 cipher block size

using Key32   = std::array<std::uint8_t, kKeySize>;
using Nonce24 = std::array<std::uint8_t, kNonceSize>;
using Tag16   = std::array<std::uint8_t, kTagSize>;

/// Generate cryptographically secure random bytes
[[nodiscard]] Key32 generateKey();
[[nodiscard]] Nonce24 generateNonce();

/**
 * @brief The nonce a segment sealed under @p keyId is encrypted with.
 *
 * Deterministic, so a reader can decrypt without the author having to ship a
 * nonce alongside the ciphertext. Safe because ChaCha20-Poly1305 needs a
 * nonce unique *per key*, and a keyId names exactly one CEK encrypting
 * exactly one segment: distinct segments have distinct keyIds and therefore
 * distinct keys, so no keystream is ever reused.
 *
 * That invariant is the whole safety argument, so it is worth stating
 * plainly: **reusing a keyId for a second segment reuses a keystream**, and
 * an attacker holding both ciphertexts recovers the XOR of the plaintexts.
 * Mint a fresh keyId per sealed span.
 *
 * This convention previously lived as an open-coded memcpy in the resolver
 * and another in a test -- two copies of an unwritten rule, which is how
 * conventions drift into incompatibility.
 */
[[nodiscard]] Nonce24
nonceForKeyId(const std::array<std::uint8_t, 32> &keyId) noexcept;

/**
 * @brief Derive a span-specific Content Encryption Key (CEK) from a master
 * segment key.
 *
 * Uses HKDF-SHA256 with info = "xudu-transcopyright-span-v1:<start>:<length>".
 */
[[nodiscard]] Key32 deriveSpanCek(const Key32 &masterKey,
                                  std::uint64_t spanStart,
                                  std::uint64_t spanLength);

/**
 * @brief Authenticated encryption of plaintext using AEAD ChaCha20-Poly1305.
 * @param plaintext Unencrypted data bytes.
 * @param key 256-bit symmetric key.
 * @param nonce 192-bit extended nonce.
 * @param ad Optional associated authenticated data.
 * @return Encrypted ciphertext with 16-byte Poly1305 tag appended.
 */
[[nodiscard]] std::string encryptAead(std::string_view plaintext,
                                      const Key32 &key, const Nonce24 &nonce,
                                      std::string_view ad = {});

/**
 * @brief Authenticated decryption of ciphertext using AEAD ChaCha20-Poly1305.
 * @param ciphertextWithTag Ciphertext ending with 16-byte Poly1305 tag.
 * @param key 256-bit symmetric key.
 * @param nonce 192-bit extended nonce.
 * @param ad Optional associated authenticated data.
 * @return Decrypted plaintext, or std::nullopt if authentication fails.
 */
[[nodiscard]] std::optional<std::string>
decryptAead(std::string_view ciphertextWithTag, const Key32 &key,
            const Nonce24 &nonce, std::string_view ad = {});

/**
 * @brief Decrypt a segment and return the requested slice of the plaintext.
 *
 * Named for what it does. It was decryptSeekableSpan, documented as seeking
 * to a 64-byte block counter and decrypting only the requested range -- which
 * it never did, and could not: a Poly1305 tag authenticates a whole message,
 * so decrypting a fragment means returning bytes nothing has vouched for.
 * Whole-segment decryption is the right call; only the name and the claim
 * were wrong.
 *
 * The cost is real, so callers that resolve the same span repeatedly should
 * cache the plaintext rather than call this per frame. Genuine random access
 * would need a chunked AEAD framing, with a tag per chunk.
 */
[[nodiscard]] std::optional<std::string>
decryptSpanSlice(std::string_view ciphertext, std::uint64_t cipherBaseOffset,
                 const Key32 &key, const Nonce24 &nonce,
                 std::uint64_t reqOffset, std::uint64_t reqLength);

/**
 * @brief Compute a 256-bit cryptographic commitment (SHA-256) over withheld
 * bytes.
 */
[[nodiscard]] std::array<std::uint8_t, 32>
computeHoleCommitment(std::string_view bytes);

/**
 * @struct X25519KeyPair
 * @brief Asymmetric X25519 curve25519 keypair for HPKE key encapsulation.
 */
struct X25519KeyPair {
  Key32 publicKey{};
  Key32 privateKey{};

  [[nodiscard]] static X25519KeyPair generate();
  [[nodiscard]] static X25519KeyPair fromSeed(const Key32 &seed);
  [[nodiscard]] static Key32 derivePublicKey(const Key32 &privateKey);

  bool operator==(const X25519KeyPair &) const = default;
};

/**
 * @brief Wrap a 32-byte Content Encryption Key (CEK) for an X25519 public key.
 *
 * Ephemeral-Static ECDH -> HKDF-SHA256 -> ChaCha20-Poly1305.
 * Output payload layout: [32-byte EphemeralPubKey][24-byte Nonce][32-byte
 * EncryptedCEK][16-byte Tag] = 104 bytes.
 */
[[nodiscard]] std::vector<std::uint8_t> wrapCek(const Key32 &cek,
                                                const Key32 &recipientPubKey);

/**
 * @brief Unwrap a 32-byte CEK using the recipient's X25519 private key.
 * @return Decrypted CEK, or std::nullopt if unwrap/authentication fails.
 */
[[nodiscard]] std::optional<Key32>
unwrapCek(const std::vector<std::uint8_t> &wrapped,
          const Key32 &recipientPrivKey);

} // namespace xudu::crypto

#endif // XUDU_TRANSCOPYRIGHT_CRYPTO_HPP
