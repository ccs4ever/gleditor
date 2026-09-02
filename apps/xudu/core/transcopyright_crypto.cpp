#include "transcopyright_crypto.hpp"

#include <cstring>
#include <format>
#include <memory>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <span>
#include <stdexcept>

namespace xudu::crypto {

namespace {

struct EvpCipherCtxCloser {
  void operator()(EVP_CIPHER_CTX *ctx) const noexcept {
    if (ctx) {
      EVP_CIPHER_CTX_free(ctx);
    }
  }
};
using ScopedCipherCtx = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxCloser>;

struct EvpPkeyCloser {
  void operator()(EVP_PKEY *pkey) const noexcept {
    if (pkey) {
      EVP_PKEY_free(pkey);
    }
  }
};
using ScopedPkey = std::unique_ptr<EVP_PKEY, EvpPkeyCloser>;

struct EvpPkeyCtxCloser {
  void operator()(EVP_PKEY_CTX *ctx) const noexcept {
    if (ctx) {
      EVP_PKEY_CTX_free(ctx);
    }
  }
};
using ScopedPkeyCtx = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxCloser>;

struct EvpKdfCtxCloser {
  void operator()(EVP_KDF_CTX *ctx) const noexcept {
    if (ctx) {
      EVP_KDF_CTX_free(ctx);
    }
  }
};
using ScopedKdfCtx = std::unique_ptr<EVP_KDF_CTX, EvpKdfCtxCloser>;

Key32 hkdfSha256(const std::span<const std::uint8_t> ikm,
                 const std::span<const std::uint8_t> salt,
                 const std::string_view info) {
  EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
  if (!kdf) {
    throw std::runtime_error("EVP_KDF_fetch for HKDF failed");
  }
  ScopedKdfCtx ctx{EVP_KDF_CTX_new(kdf)};
  EVP_KDF_free(kdf);
  if (!ctx) {
    throw std::runtime_error("EVP_KDF_CTX_new failed");
  }

  char mdName[] = "SHA256";
  std::vector<OSSL_PARAM> params;
  params.push_back(OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
                                                    mdName, sizeof(mdName)));
  params.push_back(OSSL_PARAM_construct_octet_string(
      OSSL_KDF_PARAM_KEY, const_cast<std::uint8_t *>(ikm.data()), ikm.size()));
  if (!salt.empty()) {
    params.push_back(OSSL_PARAM_construct_octet_string(
        OSSL_KDF_PARAM_SALT, const_cast<std::uint8_t *>(salt.data()),
        salt.size()));
  }
  if (!info.empty()) {
    params.push_back(OSSL_PARAM_construct_octet_string(
        OSSL_KDF_PARAM_INFO, const_cast<char *>(info.data()), info.size()));
  }
  params.push_back(OSSL_PARAM_construct_end());

  Key32 out{};
  if (EVP_KDF_derive(ctx.get(), out.data(), out.size(), params.data()) <= 0) {
    throw std::runtime_error("HKDF key derivation failed");
  }
  return out;
}

std::pair<Key32, std::array<std::uint8_t, 12>>
deriveXChaChaSubkeys(const Key32 &key, const Nonce24 &nonce) {
  // First 16 bytes of nonce act as salt to derive 32-byte subkey
  const auto subKey = hkdfSha256(
      std::span<const std::uint8_t>{key.data(), key.size()},
      std::span<const std::uint8_t>{nonce.data(), 16}, "xudu-xchacha20-subkey");

  // Remaining 8 bytes form the 12-byte standard IETF ChaCha20 IV with zero
  // prefix
  std::array<std::uint8_t, 12> iv{};
  std::memcpy(iv.data() + 4, nonce.data() + 16, 8);
  return {subKey, iv};
}

} // namespace

Key32 generateKey() {
  Key32 k{};
  if (RAND_bytes(k.data(), static_cast<int>(k.size())) <= 0) {
    throw std::runtime_error("RAND_bytes failed to generate key");
  }
  return k;
}

Nonce24 generateNonce() {
  Nonce24 n{};
  if (RAND_bytes(n.data(), static_cast<int>(n.size())) <= 0) {
    throw std::runtime_error("RAND_bytes failed to generate nonce");
  }
  return n;
}

Key32 deriveSpanCek(const Key32 &masterKey, const std::uint64_t spanStart,
                    const std::uint64_t spanLength) {
  const auto info =
      std::format("xudu-transcopyright-span-v1:{}:{}", spanStart, spanLength);
  return hkdfSha256(
      std::span<const std::uint8_t>{masterKey.data(), masterKey.size()}, {},
      info);
}

std::string encryptAead(const std::string_view plaintext, const Key32 &key,
                        const Nonce24 &nonce, const std::string_view ad) {
  const auto [subKey, iv] = deriveXChaChaSubkeys(key, nonce);

  ScopedCipherCtx ctx{EVP_CIPHER_CTX_new()};
  if (!ctx) {
    throw std::runtime_error("Failed to allocate EVP_CIPHER_CTX");
  }

  if (EVP_EncryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr,
                         nullptr) <= 0) {
    throw std::runtime_error("EVP_EncryptInit_ex failed");
  }

  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) <=
      0) {
    throw std::runtime_error("Setting AEAD IV length failed");
  }

  if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, subKey.data(),
                         iv.data()) <= 0) {
    throw std::runtime_error("Initializing AEAD key and IV failed");
  }

  int outlen = 0;
  if (!ad.empty()) {
    if (EVP_EncryptUpdate(ctx.get(), nullptr, &outlen,
                          reinterpret_cast<const unsigned char *>(ad.data()),
                          static_cast<int>(ad.size())) <= 0) {
      throw std::runtime_error("Setting AEAD associated data failed");
    }
  }

  std::string output;
  output.resize(plaintext.size() + kTagSize);

  if (!plaintext.empty()) {
    if (EVP_EncryptUpdate(
            ctx.get(), reinterpret_cast<unsigned char *>(output.data()),
            &outlen, reinterpret_cast<const unsigned char *>(plaintext.data()),
            static_cast<int>(plaintext.size())) <= 0) {
      throw std::runtime_error("AEAD encryption update failed");
    }
  }

  int finalLen = 0;
  if (EVP_EncryptFinal_ex(
          ctx.get(), reinterpret_cast<unsigned char *>(output.data()) + outlen,
          &finalLen) <= 0) {
    throw std::runtime_error("AEAD encryption finalization failed");
  }

  Tag16 tag{};
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, kTagSize,
                          tag.data()) <= 0) {
    throw std::runtime_error("Getting AEAD authentication tag failed");
  }

  std::memcpy(output.data() + plaintext.size(), tag.data(), kTagSize);
  return output;
}

std::optional<std::string> decryptAead(const std::string_view ciphertextWithTag,
                                       const Key32 &key, const Nonce24 &nonce,
                                       const std::string_view ad) {
  if (ciphertextWithTag.size() < kTagSize) {
    return std::nullopt;
  }

  const std::size_t cipherLen = ciphertextWithTag.size() - kTagSize;
  const auto cipherData       = ciphertextWithTag.substr(0, cipherLen);
  const auto tagData          = ciphertextWithTag.substr(cipherLen, kTagSize);

  const auto [subKey, iv] = deriveXChaChaSubkeys(key, nonce);

  ScopedCipherCtx ctx{EVP_CIPHER_CTX_new()};
  if (!ctx) {
    return std::nullopt;
  }

  if (EVP_DecryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr,
                         nullptr) <= 0) {
    return std::nullopt;
  }

  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) <=
      0) {
    return std::nullopt;
  }

  if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, subKey.data(),
                         iv.data()) <= 0) {
    return std::nullopt;
  }

  if (EVP_CIPHER_CTX_ctrl(
          ctx.get(), EVP_CTRL_AEAD_SET_TAG, kTagSize,
          const_cast<unsigned char *>(
              reinterpret_cast<const unsigned char *>(tagData.data()))) <= 0) {
    return std::nullopt;
  }

  int outlen = 0;
  if (!ad.empty()) {
    if (EVP_DecryptUpdate(ctx.get(), nullptr, &outlen,
                          reinterpret_cast<const unsigned char *>(ad.data()),
                          static_cast<int>(ad.size())) <= 0) {
      return std::nullopt;
    }
  }

  std::string plaintext;
  plaintext.resize(cipherLen);

  if (cipherLen > 0) {
    if (EVP_DecryptUpdate(
            ctx.get(), reinterpret_cast<unsigned char *>(plaintext.data()),
            &outlen, reinterpret_cast<const unsigned char *>(cipherData.data()),
            static_cast<int>(cipherLen)) <= 0) {
      return std::nullopt;
    }
  }

  int finalLen = 0;
  if (EVP_DecryptFinal_ex(ctx.get(),
                          reinterpret_cast<unsigned char *>(plaintext.data()) +
                              outlen,
                          &finalLen) <= 0) {
    // Poly1305 authentication failed
    return std::nullopt;
  }

  return plaintext;
}

std::optional<std::string>
decryptSpanSlice(const std::string_view ciphertext,
                 const std::uint64_t cipherBaseOffset, const Key32 &key,
                 const Nonce24 &nonce, const std::uint64_t reqOffset,
                 const std::uint64_t reqLength) {
  if (reqLength == 0) {
    return std::string{};
  }
  if (reqOffset < cipherBaseOffset) {
    return std::nullopt;
  }

  auto fullDecrypted = decryptAead(ciphertext, key, nonce);
  if (!fullDecrypted) {
    return std::nullopt;
  }

  const std::uint64_t relOffset = reqOffset - cipherBaseOffset;
  if (relOffset + reqLength > fullDecrypted->size()) {
    return std::nullopt;
  }

  return fullDecrypted->substr(relOffset, reqLength);
}

std::array<std::uint8_t, 32>
computeHoleCommitment(const std::string_view bytes) {
  std::array<std::uint8_t, 32> hash{};
  SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(),
         hash.data());
  return hash;
}

X25519KeyPair X25519KeyPair::generate() {
  ScopedPkeyCtx pctx{EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr)};
  if (!pctx || EVP_PKEY_keygen_init(pctx.get()) <= 0) {
    throw std::runtime_error("X25519 keygen init failed");
  }

  EVP_PKEY *rawPkey = nullptr;
  if (EVP_PKEY_keygen(pctx.get(), &rawPkey) <= 0 || !rawPkey) {
    throw std::runtime_error("X25519 key generation failed");
  }
  ScopedPkey pkey{rawPkey};

  X25519KeyPair pair;
  std::size_t pubLen  = pair.publicKey.size();
  std::size_t privLen = pair.privateKey.size();

  if (EVP_PKEY_get_raw_public_key(pkey.get(), pair.publicKey.data(), &pubLen) <=
          0 ||
      EVP_PKEY_get_raw_private_key(pkey.get(), pair.privateKey.data(),
                                   &privLen) <= 0) {
    throw std::runtime_error("Extracting raw X25519 keys failed");
  }
  return pair;
}

X25519KeyPair X25519KeyPair::fromSeed(const Key32 &seed) {
  ScopedPkey pkey{
      EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, seed.data(), 32)};
  if (!pkey) {
    throw std::runtime_error("Failed to load X25519 private key from seed");
  }

  X25519KeyPair pair;
  pair.privateKey    = seed;
  std::size_t pubLen = pair.publicKey.size();
  if (EVP_PKEY_get_raw_public_key(pkey.get(), pair.publicKey.data(), &pubLen) <=
      0) {
    throw std::runtime_error("Failed to derive X25519 public key from seed");
  }
  return pair;
}

Key32 X25519KeyPair::derivePublicKey(const Key32 &privateKey) {
  return fromSeed(privateKey).publicKey;
}

std::vector<std::uint8_t> wrapCek(const Key32 &cek,
                                  const Key32 &recipientPubKey) {
  // 1. Generate ephemeral X25519 keypair
  const auto ephemeral = X25519KeyPair::generate();

  // 2. Perform ECDH to compute shared secret
  ScopedPkey ephPriv{EVP_PKEY_new_raw_private_key(
      EVP_PKEY_X25519, nullptr, ephemeral.privateKey.data(), 32)};
  ScopedPkey recipPub{EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                  recipientPubKey.data(), 32)};
  if (!ephPriv || !recipPub) {
    throw std::runtime_error("Failed to load X25519 keys for KEM wrap");
  }

  ScopedPkeyCtx dctx{EVP_PKEY_CTX_new(ephPriv.get(), nullptr)};
  if (!dctx || EVP_PKEY_derive_init(dctx.get()) <= 0 ||
      EVP_PKEY_derive_set_peer(dctx.get(), recipPub.get()) <= 0) {
    throw std::runtime_error("ECDH key agreement init failed");
  }

  Key32 sharedSecret{};
  std::size_t secretLen = sharedSecret.size();
  if (EVP_PKEY_derive(dctx.get(), sharedSecret.data(), &secretLen) <= 0) {
    throw std::runtime_error("ECDH derivation failed");
  }

  // 3. Derive KEM wrapping key via HKDF
  const auto wrapKey = hkdfSha256(
      std::span<const std::uint8_t>{sharedSecret.data(), sharedSecret.size()},
      std::span<const std::uint8_t>{ephemeral.publicKey.data(),
                                    ephemeral.publicKey.size()},
      "xudu-tc-kem-wrap-v1");

  // 4. Encrypt CEK
  const auto nonce               = generateNonce();
  const auto encryptedCekWithTag = encryptAead(
      std::string_view{reinterpret_cast<const char *>(cek.data()), cek.size()},
      wrapKey, nonce, "xudu-tc-cek-v1");

  // Layout: [32B EphemeralPub][24B Nonce][32B EncryptedCEK + 16B Tag] = 104
  // bytes
  std::vector<std::uint8_t> payload;
  payload.reserve(32 + 24 + encryptedCekWithTag.size());
  payload.insert(payload.end(), ephemeral.publicKey.begin(),
                 ephemeral.publicKey.end());
  payload.insert(payload.end(), nonce.begin(), nonce.end());
  payload.insert(payload.end(), encryptedCekWithTag.begin(),
                 encryptedCekWithTag.end());
  return payload;
}

std::optional<Key32> unwrapCek(const std::vector<std::uint8_t> &wrapped,
                               const Key32 &recipientPrivKey) {
  // Wrapped structure: 32B ephPub + 24B nonce + 32B cipher + 16B tag = 104
  // bytes
  if (wrapped.size() != 32 + 24 + 32 + 16) {
    return std::nullopt;
  }

  Key32 ephPub{};
  Nonce24 nonce{};
  std::memcpy(ephPub.data(), wrapped.data(), 32);
  std::memcpy(nonce.data(), wrapped.data() + 32, 24);

  const std::string_view encryptedWithTag{
      reinterpret_cast<const char *>(wrapped.data() + 56), 32 + 16};

  ScopedPkey recipPriv{EVP_PKEY_new_raw_private_key(
      EVP_PKEY_X25519, nullptr, recipientPrivKey.data(), 32)};
  ScopedPkey ephPubKey{
      EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, ephPub.data(), 32)};
  if (!recipPriv || !ephPubKey) {
    return std::nullopt;
  }

  ScopedPkeyCtx dctx{EVP_PKEY_CTX_new(recipPriv.get(), nullptr)};
  if (!dctx || EVP_PKEY_derive_init(dctx.get()) <= 0 ||
      EVP_PKEY_derive_set_peer(dctx.get(), ephPubKey.get()) <= 0) {
    return std::nullopt;
  }

  Key32 sharedSecret{};
  std::size_t secretLen = sharedSecret.size();
  if (EVP_PKEY_derive(dctx.get(), sharedSecret.data(), &secretLen) <= 0) {
    return std::nullopt;
  }

  const auto wrapKey = hkdfSha256(
      std::span<const std::uint8_t>{sharedSecret.data(), sharedSecret.size()},
      std::span<const std::uint8_t>{ephPub.data(), ephPub.size()},
      "xudu-tc-kem-wrap-v1");

  auto decrypted =
      decryptAead(encryptedWithTag, wrapKey, nonce, "xudu-tc-cek-v1");
  if (!decrypted || decrypted->size() != kKeySize) {
    return std::nullopt;
  }

  Key32 cek{};
  std::memcpy(cek.data(), decrypted->data(), kKeySize);
  return cek;
}

} // namespace xudu::crypto
