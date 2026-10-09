/**
 * @file pgp_fixture.hpp
 * @brief Modernized Ed25519 and X.509 v3 cryptographic identity test fixtures.
 *
 * Provides deterministic author and impostor key pairs, canonical delegation
 * certificates, and device keys for headless unit testing.
 */
#ifndef XUDU_TESTS_PGP_FIXTURE_HPP
#define XUDU_TESTS_PGP_FIXTURE_HPP

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/identity/standard_crypto_engine.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu::testing {

inline constexpr std::string_view kAuthorFingerprint =
    "2151B04ADF99D0AB9886CCD5AB83CC2E0A1D80F0";

inline constexpr std::string_view kImpostorFingerprint =
    "9D7EAE7DA54B600656115C0591F07DD3C211514A";

inline constexpr std::string_view kAuthorMessage =
    "device-delegation-canonical-bytes";

/// 64-hex Ed25519 author master public key (derived from 32 bytes of 0x42)
inline constexpr std::string_view kAuthorPublicKey =
    "2152f8d19b791d24453242e15f2eab6cb7cffa7b6a5ed30097960e069881db12";

/// 64-hex Ed25519 impostor public key
inline constexpr std::string_view kImpostorPublicKey =
    "9999999999999999999999999999999999999999999999999999999999999999";

/// Fixed Ed25519 device keypair for peer tests.
inline constexpr std::string_view kTestDevicePublicKeyHex =
    "535618716e531bea9985cb29e22e1ae86e3b78f61a4951e363b460d9ecad95c6";
inline constexpr std::string_view kTestDeviceSecretKeyHex =
    "d03cc92386a942ff91df41c6b7dc1c6a065357a4e9ddd7cd7677a78fd5c3d963"
    "7b72208256c3a8b5a37035b5f2884872416bd4eb0b7f4873ddc50171b841a7e4";

inline constexpr std::string_view kDelegationForTestDeviceKey =
    "fixture-delegation-ed25519";

inline constexpr std::string_view kDeviceDelegationSignature =
    "fixture-device-delegation-signature";

inline const identity::StandardCryptoEngine &authorMasterEngine() {
  static const auto engine = []() {
    identity::StandardCryptoEngine e;
    std::array<std::uint8_t, 32> priv;
    priv.fill(0x42);
    e.setPrivateKeyRaw(priv);
    return e;
  }();
  return engine;
}

inline DeviceDelegation
fixtureDelegation(PublicKey devicePublicKey     = PublicKey{},
                  std::string deviceName        = "thinkpad-laptop",
                  std::uint64_t issuedTimestamp = 1700000000) {
  if (devicePublicKey.isZero()) {
    devicePublicKey.bytes.fill(0x11);
  }
  DeviceDelegation cert;
  cert.masterFingerprint =
      *identity::Fingerprint::fromString(kAuthorFingerprint);
  cert.devicePublicKey = devicePublicKey;
  cert.deviceName      = std::move(deviceName);
  cert.issuedTimestamp = issuedTimestamp;

  const auto &master = authorMasterEngine();
  identity::CertificateConstraints constraints;
  constraints.serialNumber = 1001;
  constraints.notBefore    = cert.issuedTimestamp;
  constraints.notAfter     = cert.issuedTimestamp + 365 * 86400;
  constraints.deviceName   = cert.deviceName;
  constraints.deviceId     = "fixture-device";
  constraints.authorName   = "Xudu Test Author";
  constraints.authorEmail  = "author@test.invalid";

  identity::PubKey32 devPk;
  std::memcpy(devPk.bytes.data(), cert.devicePublicKey.bytes.data(), 32);
  auto certRes = master.createDelegationCertificate(master, devPk, constraints);
  if (certRes) {
    cert.certificate = std::move(*certRes);
  }
  return cert;
}

} // namespace xanadu::testing

#endif // XUDU_TESTS_PGP_FIXTURE_HPP
