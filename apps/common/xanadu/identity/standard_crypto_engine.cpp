/**
 * @file standard_crypto_engine.cpp
 * @brief Core OpenSSL 3.0+ Ed25519, X.509 v3, CSR, and path validation engine.
 */
#include "standard_crypto_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509_vfy.h>

namespace xanadu::identity {

bool isCanonicalEd25519Scalar(
    std::span<const std::uint8_t, 32> sBytes) noexcept {
  // L = 2^252 + 27742317777372353535851937790883648493
  // In little-endian byte order:
  static constexpr std::array<std::uint8_t, 32> kL = {
      0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
      0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
  for (int i = 31; i >= 0; --i) {
    if (sBytes[i] < kL[i]) {
      return true;
    }
    if (sBytes[i] > kL[i]) {
      return false;
    }
  }
  return false; // Exactly equal to L is not canonical (S < L)
}

std::string base64Encode(std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) {
    return "";
  }
  const std::size_t outLen = 4UZ * ((bytes.size() + 2UZ) / 3UZ);
  std::string out(outLen, '\0');
  const int written =
      EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                      bytes.data(), static_cast<int>(bytes.size()));
  if (written < 0) {
    return "";
  }
  return out;
}

std::expected<std::vector<std::uint8_t>, ValidationError>
base64Decode(std::string_view b64) {
  std::string clean;
  clean.reserve(b64.size());
  for (const char c : b64) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      clean.push_back(c);
    }
  }
  if (clean.empty()) {
    return std::vector<std::uint8_t>{};
  }
  if (clean.size() % 4 != 0) {
    return std::unexpected(ValidationError::SerializationError);
  }
  std::size_t pad = 0;
  if (clean.back() == '=') {
    pad++;
    if (clean.size() >= 2 && clean[clean.size() - 2] == '=') {
      pad++;
    }
  }
  std::vector<std::uint8_t> decoded((clean.size() / 4) * 3);
  const int ret = EVP_DecodeBlock(
      decoded.data(), reinterpret_cast<const unsigned char *>(clean.data()),
      static_cast<int>(clean.size()));
  if (ret < 0 || static_cast<std::size_t>(ret) < pad) {
    return std::unexpected(ValidationError::SerializationError);
  }
  decoded.resize(static_cast<std::size_t>(ret) - pad);
  return decoded;
}

StandardCryptoEngine::StandardCryptoEngine() = default;

std::expected<StandardCryptoEngine, ValidationError>
StandardCryptoEngine::generate() noexcept {
  StandardCryptoEngine engine;
  engine.generateKeyPair();
  if (!engine.hasPrivateKey()) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  return engine;
}

StandardCryptoEngine *StandardCryptoEngine::generateKeyPair() noexcept {
  ERR_clear_error();
  EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr),
                    &EVP_PKEY_CTX_free);
  if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0) {
    return this;
  }
  EVP_PKEY *rawPkey = nullptr;
  if (EVP_PKEY_keygen(ctx.get(), &rawPkey) <= 0 || !rawPkey) {
    return this;
  }
  evpKey_.reset(rawPkey);
  std::size_t pubLen = 32;
  if (EVP_PKEY_get_raw_public_key(evpKey_.get(), pubKey_.bytes.data(),
                                  &pubLen) <= 0 ||
      pubLen != 32) {
    evpKey_.reset();
    return this;
  }
  std::size_t privLen = 32;
  if (EVP_PKEY_get_raw_private_key(evpKey_.get(), privKeyBytes_.data(),
                                   &privLen) <= 0 ||
      privLen != 32) {
    evpKey_.reset();
    return this;
  }
  hasPrivKeyBytes_ = true;
  return this;
}

StandardCryptoEngine *StandardCryptoEngine::setPrivateKeyRaw(
    std::span<const std::uint8_t, 32> raw32) noexcept {
  ERR_clear_error();
  EVP_PKEY *rawPkey =
      EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, raw32.data(), 32);
  if (!rawPkey) {
    return this;
  }
  evpKey_.reset(rawPkey);
  std::copy(raw32.begin(), raw32.end(), privKeyBytes_.begin());
  hasPrivKeyBytes_   = true;
  std::size_t pubLen = 32;
  if (EVP_PKEY_get_raw_public_key(evpKey_.get(), pubKey_.bytes.data(),
                                  &pubLen) <= 0 ||
      pubLen != 32) {
    evpKey_.reset();
    hasPrivKeyBytes_ = false;
  }
  return this;
}

StandardCryptoEngine *
StandardCryptoEngine::setPublicKey(const PubKey32 &pubKey) noexcept {
  pubKey_ = pubKey;
  if (!hasPrivKeyBytes_) {
    ERR_clear_error();
    EVP_PKEY *rawPkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                    pubKey.bytes.data(), 32);
    if (rawPkey) {
      evpKey_.reset(rawPkey);
    }
  }
  return this;
}

StandardCryptoEngine *
StandardCryptoEngine::setDefaultDomain(std::string_view domain) noexcept {
  defaultDomain_ = std::string(domain);
  return this;
}

std::string StandardCryptoEngine::algorithmName() const noexcept {
  return "Ed25519";
}

PubKey32 StandardCryptoEngine::publicKey() const noexcept { return pubKey_; }

std::optional<std::span<const std::uint8_t, 32>>
StandardCryptoEngine::rawPrivateKey() const noexcept {
  if (hasPrivKeyBytes_) {
    return std::span<const std::uint8_t, 32>(privKeyBytes_);
  }
  return std::nullopt;
}

std::expected<Signature64, ValidationError> StandardCryptoEngine::sign(
    std::span<const std::uint8_t> message) const noexcept {
  if (!evpKey_ || !hasPrivKeyBytes_) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  ERR_clear_error();
  EvpMdCtxPtr mctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!mctx) {
    return std::unexpected(ValidationError::UnsupportedAlgorithm);
  }
  if (EVP_DigestSignInit(mctx.get(), nullptr, nullptr, nullptr,
                         evpKey_.get()) <= 0) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  Signature64 sig{};
  std::size_t sigLen = 64;
  if (defaultDomain_.empty()) {
    if (EVP_DigestSign(mctx.get(), sig.bytes.data(), &sigLen, message.data(),
                       message.size()) <= 0 ||
        sigLen != 64) {
      return std::unexpected(ValidationError::CorruptSignature);
    }
  } else {
    std::vector<std::uint8_t> buffer;
    buffer.reserve(defaultDomain_.size() + message.size());
    buffer.insert(buffer.end(), defaultDomain_.begin(), defaultDomain_.end());
    buffer.insert(buffer.end(), message.begin(), message.end());
    if (EVP_DigestSign(mctx.get(), sig.bytes.data(), &sigLen, buffer.data(),
                       buffer.size()) <= 0 ||
        sigLen != 64) {
      return std::unexpected(ValidationError::CorruptSignature);
    }
  }
  return sig;
}

std::expected<void, ValidationError>
StandardCryptoEngine::verify(const PubKey32 &pubKey,
                             std::span<const std::uint8_t> message,
                             const Signature64 &signature) const noexcept {
  if (pubKey.isZero()) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  if (signature.isZero()) {
    return std::unexpected(ValidationError::CorruptSignature);
  }
  // Enforce RFC 8032 Section 5.1.7 canonical scalar verification (S < L)
  std::span<const std::uint8_t, 32> sBytes(signature.bytes.data() + 32, 32);
  if (!isCanonicalEd25519Scalar(sBytes)) {
    return std::unexpected(ValidationError::CorruptSignature);
  }

  ERR_clear_error();
  EvpPkeyPtr pkey(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                              pubKey.bytes.data(), 32),
                  &EVP_PKEY_free);
  if (!pkey) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  EvpMdCtxPtr mctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!mctx) {
    return std::unexpected(ValidationError::UnsupportedAlgorithm);
  }
  if (EVP_DigestVerifyInit(mctx.get(), nullptr, nullptr, nullptr, pkey.get()) <=
      0) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  const int ret = EVP_DigestVerify(mctx.get(), signature.bytes.data(), 64,
                                   message.data(), message.size());
  if (ret != 1) {
    return std::unexpected(ValidationError::CorruptSignature);
  }
  return {};
}

std::expected<Signature64, ValidationError>
StandardCryptoEngine::signWithDomain(
    std::string_view domain,
    std::span<const std::uint8_t> message) const noexcept {
  if (!evpKey_ || !hasPrivKeyBytes_) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  ERR_clear_error();
  EvpMdCtxPtr mctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!mctx) {
    return std::unexpected(ValidationError::UnsupportedAlgorithm);
  }
  if (EVP_DigestSignInit(mctx.get(), nullptr, nullptr, nullptr,
                         evpKey_.get()) <= 0) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  std::vector<std::uint8_t> buffer;
  buffer.reserve(domain.size() + message.size());
  buffer.insert(buffer.end(), domain.begin(), domain.end());
  buffer.insert(buffer.end(), message.begin(), message.end());

  Signature64 sig{};
  std::size_t sigLen = 64;
  if (EVP_DigestSign(mctx.get(), sig.bytes.data(), &sigLen, buffer.data(),
                     buffer.size()) <= 0 ||
      sigLen != 64) {
    return std::unexpected(ValidationError::CorruptSignature);
  }
  return sig;
}

std::expected<void, ValidationError> StandardCryptoEngine::verifyWithDomain(
    const PubKey32 &pubKey, std::string_view domain,
    std::span<const std::uint8_t> message,
    const Signature64 &signature) const noexcept {
  std::vector<std::uint8_t> buffer;
  buffer.reserve(domain.size() + message.size());
  buffer.insert(buffer.end(), domain.begin(), domain.end());
  buffer.insert(buffer.end(), message.begin(), message.end());
  return verify(pubKey, buffer, signature);
}

std::expected<Signature64, ValidationError> StandardCryptoEngine::signManifest(
    std::span<const std::uint8_t> manifest) const noexcept {
  return signWithDomain(kDomainManifest, manifest);
}

std::expected<void, ValidationError> StandardCryptoEngine::verifyManifest(
    const PubKey32 &pubKey, std::span<const std::uint8_t> manifest,
    const Signature64 &signature) const noexcept {
  return verifyWithDomain(pubKey, kDomainManifest, manifest, signature);
}

std::expected<Signature64, ValidationError> StandardCryptoEngine::signDeviceCsr(
    std::span<const std::uint8_t> csrPayload) const noexcept {
  return signWithDomain(kDomainDeviceCsr, csrPayload);
}

std::expected<void, ValidationError> StandardCryptoEngine::verifyDeviceCsr(
    const PubKey32 &pubKey, std::span<const std::uint8_t> csrPayload,
    const Signature64 &signature) const noexcept {
  return verifyWithDomain(pubKey, kDomainDeviceCsr, csrPayload, signature);
}

std::expected<Signature64, ValidationError>
StandardCryptoEngine::signDeviceRevocation(
    std::span<const std::uint8_t> revocationPayload) const noexcept {
  return signWithDomain(kDomainDeviceRevocation, revocationPayload);
}

std::expected<void, ValidationError>
StandardCryptoEngine::verifyDeviceRevocation(
    const PubKey32 &pubKey, std::span<const std::uint8_t> revocationPayload,
    const Signature64 &signature) const noexcept {
  return verifyWithDomain(pubKey, kDomainDeviceRevocation, revocationPayload,
                          signature);
}

std::expected<Signature64, ValidationError> StandardCryptoEngine::signPeerAuth(
    std::span<const std::uint8_t> authPayload) const noexcept {
  return signWithDomain(kDomainPeerAuth, authPayload);
}

std::expected<void, ValidationError> StandardCryptoEngine::verifyPeerAuth(
    const PubKey32 &pubKey, std::span<const std::uint8_t> authPayload,
    const Signature64 &signature) const noexcept {
  return verifyWithDomain(pubKey, kDomainPeerAuth, authPayload, signature);
}

std::expected<std::vector<std::uint8_t>, ValidationError>
StandardCryptoEngine::exportPrivateKeyPkcs8Der(
    std::string_view passphrase) const noexcept {
  if (!evpKey_ || !hasPrivKeyBytes_) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  ERR_clear_error();
  BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free_all);
  if (!bio) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  const EVP_CIPHER *cipher = passphrase.empty() ? nullptr : EVP_aes_256_cbc();
  if (i2d_PKCS8PrivateKey_bio(
          bio.get(), evpKey_.get(), cipher,
          passphrase.empty() ? nullptr : const_cast<char *>(passphrase.data()),
          static_cast<int>(passphrase.size()), nullptr, nullptr) <= 0) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  BUF_MEM *bptr = nullptr;
  BIO_get_mem_ptr(bio.get(), &bptr);
  if (!bptr || !bptr->data) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  return std::vector<std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(bptr->data),
      reinterpret_cast<const std::uint8_t *>(bptr->data) + bptr->length);
}

std::expected<std::string, ValidationError>
StandardCryptoEngine::exportPrivateKeyPkcs8Pem(
    std::string_view passphrase) const noexcept {
  if (!evpKey_ || !hasPrivKeyBytes_) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  ERR_clear_error();
  BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free_all);
  if (!bio) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  const EVP_CIPHER *cipher = passphrase.empty() ? nullptr : EVP_aes_256_cbc();
  if (PEM_write_bio_PKCS8PrivateKey(
          bio.get(), evpKey_.get(), cipher,
          passphrase.empty() ? nullptr : const_cast<char *>(passphrase.data()),
          static_cast<int>(passphrase.size()), nullptr, nullptr) <= 0) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  BUF_MEM *bptr = nullptr;
  BIO_get_mem_ptr(bio.get(), &bptr);
  if (!bptr || !bptr->data) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  return std::string(bptr->data, bptr->length);
}

std::expected<void, ValidationError>
StandardCryptoEngine::loadPrivateKeyPkcs8Der(
    std::span<const std::uint8_t> der, std::string_view passphrase) noexcept {
  if (der.empty()) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  ERR_clear_error();
  BioPtr bio(BIO_new_mem_buf(der.data(), static_cast<int>(der.size())),
             &BIO_free_all);
  if (!bio) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  EVP_PKEY *rawPkey = d2i_PKCS8PrivateKey_bio(
      bio.get(), nullptr, nullptr,
      passphrase.empty() ? nullptr : const_cast<char *>(passphrase.data()));
  if (!rawPkey) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  evpKey_.reset(rawPkey);
  std::size_t pubLen = 32;
  if (EVP_PKEY_get_raw_public_key(evpKey_.get(), pubKey_.bytes.data(),
                                  &pubLen) <= 0 ||
      pubLen != 32) {
    evpKey_.reset();
    return std::unexpected(ValidationError::CorruptKey);
  }
  std::size_t privLen = 32;
  if (EVP_PKEY_get_raw_private_key(evpKey_.get(), privKeyBytes_.data(),
                                   &privLen) <= 0 ||
      privLen != 32) {
    evpKey_.reset();
    return std::unexpected(ValidationError::CorruptKey);
  }
  hasPrivKeyBytes_ = true;
  return {};
}

std::expected<void, ValidationError>
StandardCryptoEngine::loadPrivateKeyPkcs8Pem(
    std::string_view pem, std::string_view passphrase) noexcept {
  if (pem.empty()) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  ERR_clear_error();
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())),
             &BIO_free_all);
  if (!bio) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  EVP_PKEY *rawPkey = PEM_read_bio_PrivateKey(
      bio.get(), nullptr, nullptr,
      passphrase.empty() ? nullptr : const_cast<char *>(passphrase.data()));
  if (!rawPkey) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  evpKey_.reset(rawPkey);
  std::size_t pubLen = 32;
  if (EVP_PKEY_get_raw_public_key(evpKey_.get(), pubKey_.bytes.data(),
                                  &pubLen) <= 0 ||
      pubLen != 32) {
    evpKey_.reset();
    return std::unexpected(ValidationError::CorruptKey);
  }
  std::size_t privLen = 32;
  if (EVP_PKEY_get_raw_private_key(evpKey_.get(), privKeyBytes_.data(),
                                   &privLen) <= 0 ||
      privLen != 32) {
    evpKey_.reset();
    return std::unexpected(ValidationError::CorruptKey);
  }
  hasPrivKeyBytes_ = true;
  return {};
}

StandardCryptoEngine *StandardCryptoEngine::importPrivateKeyPkcs8Der(
    std::span<const std::uint8_t> der, std::string_view passphrase,
    std::expected<void, ValidationError> *status) noexcept {
  auto res = loadPrivateKeyPkcs8Der(der, passphrase);
  if (status) {
    *status = res;
  }
  return this;
}

StandardCryptoEngine *StandardCryptoEngine::importPrivateKeyPkcs8Pem(
    std::string_view pem, std::string_view passphrase,
    std::expected<void, ValidationError> *status) noexcept {
  auto res = loadPrivateKeyPkcs8Pem(pem, passphrase);
  if (status) {
    *status = res;
  }
  return this;
}

std::expected<std::vector<std::uint8_t>, ValidationError>
StandardCryptoEngine::generateCsr(
    const ISigner &deviceSigner, const std::string &deviceId,
    const std::string &deviceName) const noexcept {
  ERR_clear_error();
  X509ReqPtr req(X509_REQ_new(), &X509_REQ_free);
  if (!req) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }
  if (X509_REQ_set_version(req.get(), 0) <= 0) {
    return std::unexpected(ValidationError::SerializationError);
  }

  X509_NAME *name = X509_REQ_get_subject_name(req.get());
  if (!name) {
    return std::unexpected(ValidationError::SerializationError);
  }
  const std::string cn = !deviceName.empty() ? deviceName : deviceId;
  if (!cn.empty()) {
    if (X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_UTF8,
            reinterpret_cast<const unsigned char *>(cn.c_str()), -1, -1,
            0) <= 0) {
      return std::unexpected(ValidationError::SerializationError);
    }
  }
  if (!deviceId.empty()) {
    if (X509_NAME_add_entry_by_txt(
            name, "OU", MBSTRING_UTF8,
            reinterpret_cast<const unsigned char *>(deviceId.c_str()), -1, -1,
            0) <= 0) {
      return std::unexpected(ValidationError::SerializationError);
    }
  }

  const PubKey32 devPub = deviceSigner.publicKey();
  if (devPub.isZero()) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  EvpPkeyPtr pkeyPub(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                 devPub.bytes.data(), 32),
                     &EVP_PKEY_free);
  if (!pkeyPub || X509_REQ_set_pubkey(req.get(), pkeyPub.get()) <= 0) {
    return std::unexpected(ValidationError::CorruptKey);
  }

  EvpPkeyPtr tempSignKey(nullptr, &EVP_PKEY_free);
  EVP_PKEY *signKey = nullptr;
  if (const auto *engineSigner =
          dynamic_cast<const StandardCryptoEngine *>(&deviceSigner)) {
    signKey = engineSigner->rawEvpPkey();
  }
  if (!signKey) {
    auto optPriv = deviceSigner.rawPrivateKey();
    if (optPriv) {
      tempSignKey.reset(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                     optPriv->data(), 32));
      signKey = tempSignKey.get();
    }
  }
  if (!signKey) {
    return std::unexpected(ValidationError::CorruptKey);
  }

  if (X509_REQ_sign(req.get(), signKey, nullptr) <= 0) {
    return std::unexpected(ValidationError::CsrSignatureInvalid);
  }

  const int derLen = i2d_X509_REQ(req.get(), nullptr);
  if (derLen <= 0) {
    return std::unexpected(ValidationError::SerializationError);
  }
  std::vector<std::uint8_t> der(derLen);
  unsigned char *p = der.data();
  if (i2d_X509_REQ(req.get(), &p) <= 0) {
    return std::unexpected(ValidationError::SerializationError);
  }
  return der;
}

std::expected<X509Certificate, ValidationError>
StandardCryptoEngine::createDelegationCertificate(
    const ISigner &masterSigner, const PubKey32 &devicePublicKey,
    const CertificateConstraints &constraints) const noexcept {
  if (devicePublicKey.isZero()) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  ERR_clear_error();
  EvpPkeyPtr pkeyPub(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                 devicePublicKey.bytes.data(),
                                                 32),
                     &EVP_PKEY_free);
  if (!pkeyPub) {
    return std::unexpected(ValidationError::CorruptKey);
  }

  X509Ptr cert(X509_new(), &X509_free);
  if (!cert) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }
  // X.509 v3 (version value 2)
  if (X509_set_version(cert.get(), 2) <= 0) {
    return std::unexpected(ValidationError::SerializationError);
  }

  if (ASN1_INTEGER_set_uint64(X509_get_serialNumber(cert.get()),
                              constraints.serialNumber) <= 0) {
    return std::unexpected(ValidationError::SerializationError);
  }

  X509_NAME *certSubj  = X509_get_subject_name(cert.get());
  const std::string cn = !constraints.deviceName.empty()
                             ? constraints.deviceName
                             : constraints.deviceId;
  if (!cn.empty()) {
    X509_NAME_add_entry_by_txt(
        certSubj, "CN", MBSTRING_UTF8,
        reinterpret_cast<const unsigned char *>(cn.c_str()), -1, -1, 0);
  }
  if (!constraints.deviceId.empty()) {
    X509_NAME_add_entry_by_txt(
        certSubj, "OU", MBSTRING_UTF8,
        reinterpret_cast<const unsigned char *>(constraints.deviceId.c_str()),
        -1, -1, 0);
  }

  X509_NAME *issuerName      = X509_get_issuer_name(cert.get());
  const std::string authorCn = !constraints.authorName.empty()
                                   ? constraints.authorName
                                   : "Xanadu Master Authority";
  if (X509_NAME_add_entry_by_txt(
          issuerName, "CN", MBSTRING_UTF8,
          reinterpret_cast<const unsigned char *>(authorCn.c_str()), -1, -1,
          0) <= 0) {
    return std::unexpected(ValidationError::SerializationError);
  }
  if (!constraints.authorEmail.empty()) {
    X509_NAME_add_entry_by_txt(issuerName, "emailAddress", MBSTRING_UTF8,
                               reinterpret_cast<const unsigned char *>(
                                   constraints.authorEmail.c_str()),
                               -1, -1, 0);
  }

  if (ASN1_TIME_set(X509_getm_notBefore(cert.get()),
                    static_cast<time_t>(constraints.notBefore)) == nullptr ||
      ASN1_TIME_set(X509_getm_notAfter(cert.get()),
                    static_cast<time_t>(constraints.notAfter)) == nullptr) {
    return std::unexpected(ValidationError::SerializationError);
  }

  if (X509_set_pubkey(cert.get(), pkeyPub.get()) <= 0) {
    return std::unexpected(ValidationError::CorruptKey);
  }

  X509V3_CTX v3ctx;
  X509V3_set_ctx_nodb(&v3ctx);
  X509V3_set_ctx(&v3ctx, cert.get(), cert.get(), nullptr, nullptr, 0);

  X509_EXTENSION *extBc = X509V3_EXT_conf_nid(
      nullptr, &v3ctx, NID_basic_constraints, "critical,CA:FALSE");
  if (extBc) {
    X509_add_ext(cert.get(), extBc, -1);
    X509_EXTENSION_free(extBc);
  }

  X509_EXTENSION *extKu =
      X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_key_usage,
                          "critical,digitalSignature,nonRepudiation");
  if (extKu) {
    X509_add_ext(cert.get(), extKu, -1);
    X509_EXTENSION_free(extKu);
  }

  EvpPkeyPtr tempMasterKey(nullptr, &EVP_PKEY_free);
  EVP_PKEY *masterPkey = nullptr;
  if (const auto *engineSigner =
          dynamic_cast<const StandardCryptoEngine *>(&masterSigner)) {
    masterPkey = engineSigner->rawEvpPkey();
  }
  if (!masterPkey) {
    auto optPriv = masterSigner.rawPrivateKey();
    if (optPriv) {
      tempMasterKey.reset(EVP_PKEY_new_raw_private_key(
          EVP_PKEY_ED25519, nullptr, optPriv->data(), 32));
      masterPkey = tempMasterKey.get();
    }
  }
  if (!masterPkey) {
    return std::unexpected(ValidationError::CorruptKey);
  }

  if (X509_sign(cert.get(), masterPkey, nullptr) <= 0) {
    return std::unexpected(ValidationError::DelegationSignatureInvalid);
  }

  const int derLen = i2d_X509(cert.get(), nullptr);
  if (derLen <= 0) {
    return std::unexpected(ValidationError::SerializationError);
  }
  std::vector<std::uint8_t> certDer(derLen);
  unsigned char *derOut = certDer.data();
  if (i2d_X509(cert.get(), &derOut) <= 0) {
    return std::unexpected(ValidationError::SerializationError);
  }

  X509Certificate res;
  res.setDer(std::move(certDer));
  res.setSerial(constraints.serialNumber);
  res.setValidity(constraints.notBefore, constraints.notAfter);
  res.setSubjectPublicKey(devicePublicKey);
  res.setIssuerPublicKey(masterSigner.publicKey());

  char subjBuf[256] = {};
  X509_NAME_oneline(X509_get_subject_name(cert.get()), subjBuf,
                    sizeof(subjBuf));
  res.setSubjectDn(subjBuf);

  char issBuf[256] = {};
  X509_NAME_oneline(X509_get_issuer_name(cert.get()), issBuf, sizeof(issBuf));
  res.setIssuerDn(issBuf);

  return res;
}

std::expected<X509Certificate, ValidationError>
StandardCryptoEngine::authorizeCsr(
    const ISigner &masterSigner, std::span<const std::uint8_t> csrDer,
    const CertificateConstraints &constraints) const noexcept {
  if (csrDer.empty()) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }
  ERR_clear_error();
  const unsigned char *p = csrDer.data();
  X509ReqPtr req(d2i_X509_REQ(nullptr, &p, static_cast<long>(csrDer.size())),
                 &X509_REQ_free);
  if (!req || p != csrDer.data() + csrDer.size()) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }

  // Verify CSR signature
  EvpPkeyPtr reqPubkey(X509_REQ_get_pubkey(req.get()), &EVP_PKEY_free);
  if (!reqPubkey) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  if (X509_REQ_verify(req.get(), reqPubkey.get()) <= 0) {
    return std::unexpected(ValidationError::CsrSignatureInvalid);
  }

  // Enforce canonical Ed25519 scalar verification (S < L)
  const ASN1_BIT_STRING *csrSig = nullptr;
  const X509_ALGOR *csrAlg      = nullptr;
  X509_REQ_get0_signature(req.get(), &csrSig, &csrAlg);
  if (csrSig && csrSig->length == 64) {
    std::span<const std::uint8_t, 32> sBytes(csrSig->data + 32, 32);
    if (!isCanonicalEd25519Scalar(sBytes)) {
      return std::unexpected(ValidationError::CsrSignatureInvalid);
    }
  }

  PubKey32 devicePubKey{};
  std::size_t pubLen = 32;
  if (EVP_PKEY_get_raw_public_key(reqPubkey.get(), devicePubKey.bytes.data(),
                                  &pubLen) <= 0 ||
      pubLen != 32) {
    return std::unexpected(ValidationError::CorruptKey);
  }

  CertificateConstraints effective = constraints;
  X509_NAME *reqSubj               = X509_REQ_get_subject_name(req.get());
  if (reqSubj && effective.deviceName.empty()) {
    char cnBuf[256] = {};
    if (X509_NAME_get_text_by_NID(reqSubj, NID_commonName, cnBuf,
                                  sizeof(cnBuf)) > 0) {
      effective.deviceName = cnBuf;
    }
  }

  return createDelegationCertificate(masterSigner, devicePubKey, effective);
}

std::optional<PubKey32>
StandardCryptoEngine::publicKeyFromAnyFormat(std::string_view keyStr) noexcept {
  if (keyStr.size() == 32) {
    PubKey32 pk;
    std::memcpy(pk.bytes.data(), keyStr.data(), 32);
    return pk;
  }
  if (keyStr.size() == 64) {
    auto hOpt = Hash32::fromHex(keyStr);
    if (hOpt) {
      PubKey32 pk;
      std::memcpy(pk.bytes.data(), hOpt->bytes.data(), 32);
      return pk;
    }
  }
  if (!keyStr.empty()) {
    auto b64Res = base64Decode(keyStr);
    if (b64Res.has_value() && b64Res->size() == 32) {
      PubKey32 pk;
      std::memcpy(pk.bytes.data(), b64Res->data(), 32);
      return pk;
    }
    ERR_clear_error();
    BIO *bio = BIO_new_mem_buf(keyStr.data(), static_cast<int>(keyStr.size()));
    if (bio) {
      EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
      BIO_free(bio);
      if (pkey) {
        PubKey32 pk;
        std::size_t len = 32;
        int ok = EVP_PKEY_get_raw_public_key(pkey, pk.bytes.data(), &len);
        EVP_PKEY_free(pkey);
        if (ok == 1 && len == 32) {
          return pk;
        }
      }
    }
  }
  return std::nullopt;
}

std::expected<void, ValidationError> StandardCryptoEngine::verifyDelegation(
    const X509Certificate &cert, const PubKey32 &expectedMasterKey,
    std::uint64_t currentTime) const noexcept {
  if (cert.empty()) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }
  if (expectedMasterKey.isZero()) {
    return std::unexpected(ValidationError::CorruptKey);
  }

  ERR_clear_error();
  const unsigned char *p = cert.der.data();
  X509Ptr x509Cert(d2i_X509(nullptr, &p, static_cast<long>(cert.der.size())),
                   &X509_free);
  if (!x509Cert || p != cert.der.data() + cert.der.size()) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }

  // 1. Temporal checks: [notBefore, notAfter]
  if (currentTime > 0) {
    const ASN1_TIME *nb = X509_get0_notBefore(x509Cert.get());
    const ASN1_TIME *na = X509_get0_notAfter(x509Cert.get());
    auto curT           = static_cast<time_t>(currentTime);
    if (nb && X509_cmp_time(nb, &curT) > 0) {
      return std::unexpected(ValidationError::CertificateNotYetValid);
    }
    if (na && X509_cmp_time(na, &curT) < 0) {
      return std::unexpected(ValidationError::CertificateExpired);
    }
  }

  // 2. BasicConstraints: must be present and cA=FALSE
  const uint32_t exFlags = X509_get_extension_flags(x509Cert.get());
  if ((exFlags & EXFLAG_BCONS) == 0 || (exFlags & EXFLAG_CA) != 0 ||
      X509_check_ca(x509Cert.get()) != 0) {
    return std::unexpected(ValidationError::PathValidationFailed);
  }

  // 3. KeyUsage: must be present with KU_DIGITAL_SIGNATURE and
  // KU_NON_REPUDIATION
  if ((exFlags & EXFLAG_KUSAGE) == 0) {
    return std::unexpected(ValidationError::PathValidationFailed);
  }
  const uint32_t ku = X509_get_key_usage(x509Cert.get());
  if ((ku & KU_DIGITAL_SIGNATURE) == 0 || (ku & KU_NON_REPUDIATION) == 0) {
    return std::unexpected(ValidationError::PathValidationFailed);
  }

  // 4. Verify canonical Ed25519 signature on certificate
  const ASN1_BIT_STRING *sig = nullptr;
  const X509_ALGOR *alg      = nullptr;
  X509_get0_signature(&sig, &alg, x509Cert.get());
  if (sig && sig->length == 64) {
    std::span<const std::uint8_t, 32> sBytes(sig->data + 32, 32);
    if (!isCanonicalEd25519Scalar(sBytes)) {
      return std::unexpected(ValidationError::DelegationSignatureInvalid);
    }
  }

  // 5. Direct signature verification against master public key
  EvpPkeyPtr rootPkey(
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                  expectedMasterKey.bytes.data(), 32),
      &EVP_PKEY_free);
  if (!rootPkey) {
    return std::unexpected(ValidationError::CorruptKey);
  }
  if (X509_verify(x509Cert.get(), rootPkey.get()) <= 0) {
    return std::unexpected(ValidationError::DelegationSignatureInvalid);
  }

  // 6. Full X509_STORE and X509_STORE_CTX path validation
  X509StorePtr store(X509_STORE_new(), &X509_STORE_free);
  if (!store) {
    return std::unexpected(ValidationError::PathValidationFailed);
  }

  X509Ptr rootCert(X509_new(), &X509_free);
  if (!rootCert) {
    return std::unexpected(ValidationError::PathValidationFailed);
  }
  X509_set_version(rootCert.get(), 2);
  ASN1_INTEGER_set_uint64(X509_get_serialNumber(rootCert.get()), 0);
  X509_NAME *issName = X509_get_issuer_name(x509Cert.get());
  X509_set_subject_name(rootCert.get(), issName);
  X509_set_issuer_name(rootCert.get(), issName);
  ASN1_TIME_set(X509_getm_notBefore(rootCert.get()), 0);
  ASN1_TIME_set(X509_getm_notAfter(rootCert.get()),
                static_cast<time_t>(currentTime > 0 ? currentTime + 86400 * 3650
                                                    : 2147483647));
  X509_set_pubkey(rootCert.get(), rootPkey.get());

  X509V3_CTX rootCtx;
  X509V3_set_ctx_nodb(&rootCtx);
  X509V3_set_ctx(&rootCtx, rootCert.get(), rootCert.get(), nullptr, nullptr, 0);
  X509_EXTENSION *rootBc = X509V3_EXT_conf_nid(
      nullptr, &rootCtx, NID_basic_constraints, "critical,CA:TRUE");
  if (rootBc) {
    X509_add_ext(rootCert.get(), rootBc, -1);
    X509_EXTENSION_free(rootBc);
  }
  X509_EXTENSION *rootKu = X509V3_EXT_conf_nid(nullptr, &rootCtx, NID_key_usage,
                                               "critical,keyCertSign,cRLSign");
  if (rootKu) {
    X509_add_ext(rootCert.get(), rootKu, -1);
    X509_EXTENSION_free(rootKu);
  }

  if (X509_STORE_add_cert(store.get(), rootCert.get()) <= 0) {
    return std::unexpected(ValidationError::PathValidationFailed);
  }

  X509StoreCtxPtr ctx(X509_STORE_CTX_new(), &X509_STORE_CTX_free);
  if (!ctx) {
    return std::unexpected(ValidationError::PathValidationFailed);
  }
  if (X509_STORE_CTX_init(ctx.get(), store.get(), x509Cert.get(), nullptr) <=
      0) {
    return std::unexpected(ValidationError::PathValidationFailed);
  }

  X509_VERIFY_PARAM *param = X509_STORE_CTX_get0_param(ctx.get());
  if (param) {
    X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_PARTIAL_CHAIN);
    if (currentTime > 0) {
      X509_VERIFY_PARAM_set_time(param, static_cast<time_t>(currentTime));
    } else {
      X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_NO_CHECK_TIME);
    }
  }

  const int verifyRes = X509_verify_cert(ctx.get());
  if (verifyRes <= 0) {
    const int err = X509_STORE_CTX_get_error(ctx.get());
    if (err == X509_V_ERR_CERT_HAS_EXPIRED) {
      return std::unexpected(ValidationError::CertificateExpired);
    }
    if (err == X509_V_ERR_CERT_NOT_YET_VALID) {
      return std::unexpected(ValidationError::CertificateNotYetValid);
    }
    if (err == X509_V_ERR_CERT_SIGNATURE_FAILURE) {
      return std::unexpected(ValidationError::DelegationSignatureInvalid);
    }
    return std::unexpected(ValidationError::PathValidationFailed);
  }

  return {};
}

std::expected<std::string, ValidationError>
StandardCryptoEngine::certificateToPem(const X509Certificate &cert) noexcept {
  if (cert.empty()) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }
  ERR_clear_error();
  const unsigned char *p = cert.der.data();
  X509Ptr x509(d2i_X509(nullptr, &p, static_cast<long>(cert.der.size())),
               &X509_free);
  if (!x509) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }
  BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free_all);
  if (!bio || PEM_write_bio_X509(bio.get(), x509.get()) <= 0) {
    return std::unexpected(ValidationError::SerializationError);
  }
  BUF_MEM *bptr = nullptr;
  BIO_get_mem_ptr(bio.get(), &bptr);
  if (!bptr || !bptr->data) {
    return std::unexpected(ValidationError::SerializationError);
  }
  return std::string(bptr->data, bptr->length);
}

std::expected<X509Certificate, ValidationError>
StandardCryptoEngine::certificateFromPem(std::string_view pem) noexcept {
  if (pem.empty()) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }
  ERR_clear_error();
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())),
             &BIO_free_all);
  if (!bio) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }
  X509Ptr x509(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr),
               &X509_free);
  if (!x509) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }
  const int derLen = i2d_X509(x509.get(), nullptr);
  if (derLen <= 0) {
    return std::unexpected(ValidationError::SerializationError);
  }
  std::vector<std::uint8_t> der(derLen);
  unsigned char *out = der.data();
  if (i2d_X509(x509.get(), &out) <= 0) {
    return std::unexpected(ValidationError::SerializationError);
  }
  return certificateFromDer(der);
}

std::expected<X509Certificate, ValidationError>
StandardCryptoEngine::certificateFromDer(
    std::span<const std::uint8_t> der) noexcept {
  if (der.empty()) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }
  ERR_clear_error();
  const unsigned char *p = der.data();
  X509Ptr x509(d2i_X509(nullptr, &p, static_cast<long>(der.size())),
               &X509_free);
  if (!x509 || p != der.data() + der.size()) {
    return std::unexpected(ValidationError::CorruptCertificate);
  }

  X509Certificate cert;
  cert.setDer(std::vector<std::uint8_t>(der.begin(), der.end()));

  const ASN1_INTEGER *serialAsn = X509_get0_serialNumber(x509.get());
  if (serialAsn) {
    uint64_t serialVal = 0;
    if (ASN1_INTEGER_get_uint64(&serialVal, serialAsn) == 1) {
      cert.setSerial(serialVal);
    }
  }

  const ASN1_TIME *nb = X509_get0_notBefore(x509.get());
  const ASN1_TIME *na = X509_get0_notAfter(x509.get());
  struct tm tmNb      = {};
  struct tm tmNa      = {};
  time_t tNb          = 0;
  time_t tNa          = 0;
  if (nb && ASN1_TIME_to_tm(nb, &tmNb)) {
    tNb = timegm(&tmNb);
  }
  if (na && ASN1_TIME_to_tm(na, &tmNa)) {
    tNa = timegm(&tmNa);
  }
  cert.setValidity(static_cast<std::uint64_t>(tNb >= 0 ? tNb : 0),
                   static_cast<std::uint64_t>(tNa >= 0 ? tNa : 0));

  EvpPkeyPtr pub(X509_get_pubkey(x509.get()), &EVP_PKEY_free);
  if (pub) {
    PubKey32 devPub{};
    std::size_t pubLen = 32;
    if (EVP_PKEY_get_raw_public_key(pub.get(), devPub.bytes.data(), &pubLen) ==
            1 &&
        pubLen == 32) {
      cert.setSubjectPublicKey(devPub);
    }
  }

  char subjBuf[256] = {};
  X509_NAME_oneline(X509_get_subject_name(x509.get()), subjBuf,
                    sizeof(subjBuf));
  cert.setSubjectDn(subjBuf);

  char issBuf[256] = {};
  X509_NAME_oneline(X509_get_issuer_name(x509.get()), issBuf, sizeof(issBuf));
  cert.setIssuerDn(issBuf);

  return cert;
}

std::string StandardCryptoEngine::certificateToBase64Der(
    const X509Certificate &cert) noexcept {
  return base64Encode(cert.der);
}

std::expected<X509Certificate, ValidationError>
StandardCryptoEngine::certificateFromBase64Der(std::string_view b64) noexcept {
  auto derRes = base64Decode(b64);
  if (!derRes) {
    return std::unexpected(derRes.error());
  }
  return certificateFromDer(*derRes);
}

} // namespace xanadu::identity
