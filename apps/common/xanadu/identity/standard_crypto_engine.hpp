/**
 * @file standard_crypto_engine.hpp
 * @brief Core OpenSSL 3.0+ Ed25519, X.509 v3, CSR, and path validation engine.
 */
#ifndef XANADU_STANDARD_CRYPTO_ENGINE_HPP
#define XANADU_STANDARD_CRYPTO_ENGINE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "identity_engine.hpp"

namespace xanadu::identity {

// OpenSSL RAII deleter wrappers
using EvpPkeyPtr  = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using EvpPkeyCtxPtr =
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using X509Ptr      = std::unique_ptr<X509, decltype(&X509_free)>;
using X509ReqPtr   = std::unique_ptr<X509_REQ, decltype(&X509_REQ_free)>;
using X509StorePtr = std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)>;
using X509StoreCtxPtr =
    std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free_all)>;

/// RFC 8032 Section 5.1.7: Canonical scalar verification ($S < L$).
[[nodiscard]] bool
isCanonicalEd25519Scalar(std::span<const std::uint8_t, 32> sBytes) noexcept;

/// Base64 encoding and decoding utilities (single-line Base64 DER support).
[[nodiscard]] std::string base64Encode(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::expected<std::vector<std::uint8_t>, ValidationError>
base64Decode(std::string_view b64);

/**
 * @class StandardCryptoEngine
 * @brief In-process OpenSSL 3.0+ engine providing Ed25519 digital signatures,
 *        X.509 v3 device delegation certificates, and PKCS#10 CSR processing.
 */
class StandardCryptoEngine : public ISigner,
                             public IVerifier,
                             public ICertificateEngine {
public:
  StandardCryptoEngine();
  ~StandardCryptoEngine() override = default;

  StandardCryptoEngine(StandardCryptoEngine &&) noexcept            = default;
  StandardCryptoEngine &operator=(StandardCryptoEngine &&) noexcept = default;

  StandardCryptoEngine(const StandardCryptoEngine &)            = delete;
  StandardCryptoEngine &operator=(const StandardCryptoEngine &) = delete;

  // --- Factory / Key Generation ---
  [[nodiscard]] static std::expected<StandardCryptoEngine, ValidationError>
  generate() noexcept;

  StandardCryptoEngine *generateKeyPair() noexcept;

  // --- Setters (Fluent chaining returning this) ---
  StandardCryptoEngine *
  setPrivateKeyRaw(std::span<const std::uint8_t, 32> raw32) noexcept;

  StandardCryptoEngine *setPublicKey(const PubKey32 &pubKey) noexcept;

  StandardCryptoEngine *setDefaultDomain(std::string_view domain) noexcept;

  // --- ISigner Interface ---
  [[nodiscard]] std::string algorithmName() const noexcept override;
  [[nodiscard]] PubKey32 publicKey() const noexcept override;
  [[nodiscard]] std::expected<Signature64, ValidationError>
  sign(std::span<const std::uint8_t> message) const noexcept override;
  [[nodiscard]] std::optional<std::span<const std::uint8_t, 32>>
  rawPrivateKey() const noexcept override;

  // --- IVerifier Interface ---
  [[nodiscard]] std::expected<void, ValidationError>
  verify(const PubKey32 &pubKey, std::span<const std::uint8_t> message,
         const Signature64 &signature) const noexcept override;

  // --- ICertificateEngine Interface ---
  [[nodiscard]] std::expected<std::vector<std::uint8_t>, ValidationError>
  generateCsr(const ISigner &deviceSigner, const std::string &deviceId,
              const std::string &deviceName) const noexcept override;

  [[nodiscard]] std::expected<X509Certificate, ValidationError> authorizeCsr(
      const ISigner &masterSigner, std::span<const std::uint8_t> csrDer,
      const CertificateConstraints &constraints) const noexcept override;

  [[nodiscard]] std::expected<X509Certificate, ValidationError>
  createDelegationCertificate(
      const ISigner &masterSigner, const PubKey32 &devicePublicKey,
      const CertificateConstraints &constraints) const noexcept;

  [[nodiscard]] static std::optional<PubKey32>
  publicKeyFromAnyFormat(std::string_view keyStr) noexcept;

  [[nodiscard]] std::expected<void, ValidationError>
  verifyDelegation(const X509Certificate &cert,
                   const PubKey32 &expectedMasterKey,
                   std::uint64_t currentTime) const noexcept override;

  // --- Domain Separation Signing & Verification ---
  [[nodiscard]] std::expected<Signature64, ValidationError>
  signWithDomain(std::string_view domain,
                 std::span<const std::uint8_t> message) const noexcept;

  [[nodiscard]] std::expected<void, ValidationError>
  verifyWithDomain(const PubKey32 &pubKey, std::string_view domain,
                   std::span<const std::uint8_t> message,
                   const Signature64 &signature) const noexcept;

  [[nodiscard]] std::expected<Signature64, ValidationError>
  signManifest(std::span<const std::uint8_t> manifest) const noexcept;
  [[nodiscard]] std::expected<void, ValidationError>
  verifyManifest(const PubKey32 &pubKey, std::span<const std::uint8_t> manifest,
                 const Signature64 &signature) const noexcept;

  [[nodiscard]] std::expected<Signature64, ValidationError>
  signDeviceCsr(std::span<const std::uint8_t> csrPayload) const noexcept;
  [[nodiscard]] std::expected<void, ValidationError>
  verifyDeviceCsr(const PubKey32 &pubKey,
                  std::span<const std::uint8_t> csrPayload,
                  const Signature64 &signature) const noexcept;

  [[nodiscard]] std::expected<Signature64, ValidationError>
  signDeviceRevocation(
      std::span<const std::uint8_t> revocationPayload) const noexcept;
  [[nodiscard]] std::expected<void, ValidationError>
  verifyDeviceRevocation(const PubKey32 &pubKey,
                         std::span<const std::uint8_t> revocationPayload,
                         const Signature64 &signature) const noexcept;

  [[nodiscard]] std::expected<Signature64, ValidationError>
  signPeerAuth(std::span<const std::uint8_t> authPayload) const noexcept;
  [[nodiscard]] std::expected<void, ValidationError>
  verifyPeerAuth(const PubKey32 &pubKey,
                 std::span<const std::uint8_t> authPayload,
                 const Signature64 &signature) const noexcept;

  // --- PKCS#8 Encrypted Serialization & Deserialization ---
  [[nodiscard]] std::expected<std::vector<std::uint8_t>, ValidationError>
  exportPrivateKeyPkcs8Der(std::string_view passphrase) const noexcept;

  [[nodiscard]] std::expected<std::string, ValidationError>
  exportPrivateKeyPkcs8Pem(std::string_view passphrase) const noexcept;

  StandardCryptoEngine *importPrivateKeyPkcs8Der(
      std::span<const std::uint8_t> der, std::string_view passphrase,
      std::expected<void, ValidationError> *status = nullptr) noexcept;

  StandardCryptoEngine *importPrivateKeyPkcs8Pem(
      std::string_view pem, std::string_view passphrase,
      std::expected<void, ValidationError> *status = nullptr) noexcept;

  [[nodiscard]] std::expected<void, ValidationError>
  loadPrivateKeyPkcs8Der(std::span<const std::uint8_t> der,
                         std::string_view passphrase) noexcept;

  [[nodiscard]] std::expected<void, ValidationError>
  loadPrivateKeyPkcs8Pem(std::string_view pem,
                         std::string_view passphrase) noexcept;

  // --- PEM, DER, and Base64 DER Conversions for Certificates ---
  [[nodiscard]] static std::expected<std::string, ValidationError>
  certificateToPem(const X509Certificate &cert) noexcept;

  [[nodiscard]] static std::expected<X509Certificate, ValidationError>
  certificateFromPem(std::string_view pem) noexcept;

  [[nodiscard]] static std::expected<X509Certificate, ValidationError>
  certificateFromDer(std::span<const std::uint8_t> der) noexcept;

  [[nodiscard]] static std::string
  certificateToBase64Der(const X509Certificate &cert) noexcept;

  [[nodiscard]] static std::expected<X509Certificate, ValidationError>
  certificateFromBase64Der(std::string_view b64) noexcept;

  // --- Accessors ---
  [[nodiscard]] bool hasPrivateKey() const noexcept {
    return evpKey_ != nullptr && hasPrivKeyBytes_;
  }
  [[nodiscard]] EVP_PKEY *rawEvpPkey() const noexcept { return evpKey_.get(); }

private:
  EvpPkeyPtr evpKey_{nullptr, &EVP_PKEY_free};
  PubKey32 pubKey_{};
  std::array<std::uint8_t, 32> privKeyBytes_{};
  bool hasPrivKeyBytes_{false};
  std::string defaultDomain_{};
};

} // namespace xanadu::identity

#endif // XANADU_STANDARD_CRYPTO_ENGINE_HPP
