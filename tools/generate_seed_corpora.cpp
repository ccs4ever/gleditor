/**
 * @file generate_seed_corpora.cpp
 * @brief Tool to generate initial valid seed corpora for session package
 *        and X.509 delegation libFuzzer engines.
 */
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/identity/identity_engine.hpp"
#include "common/xanadu/identity/standard_crypto_engine.hpp"
#include "common/xanadu/link_package.hpp"
#include "common/xanadu/session_container.hpp"
#include "common/xanadu/store.hpp"

namespace {

std::string toHex(std::span<const std::uint8_t> data) {
  static constexpr char hexChars[] = "0123456789abcdef";
  std::string s;
  s.reserve(data.size() * 2);
  for (const auto b : data) {
    s.push_back(hexChars[(b >> 4) & 0x0F]);
    s.push_back(hexChars[b & 0x0F]);
  }
  return s;
}

void writeFile(const std::filesystem::path &path,
               std::span<const std::uint8_t> data) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Failed to open " + path.string() +
                             " for writing");
  }
  out.write(reinterpret_cast<const char *>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

void writeFile(const std::filesystem::path &path, std::string_view str) {
  writeFile(path, std::span<const std::uint8_t>(
                      reinterpret_cast<const std::uint8_t *>(str.data()),
                      str.size()));
}

xanadu::CompactOpNode makeOp(std::uint32_t at, std::uint64_t spanStart,
                             std::uint64_t spanLength) {
  xanadu::CompactOpNode op{};
  op.kind       = xanadu::OpKind::Insert;
  op.at         = at;
  op.spanStart  = spanStart;
  op.spanLength = spanLength;
  op.scrollId   = 0;
  return op;
}

} // namespace

int main() {
  using namespace xanadu;
  using namespace xanadu::identity;

  const std::filesystem::path sessionDir = "tests/fuzz/corpus/session_package";
  const std::filesystem::path x509Dir    = "tests/fuzz/corpus/x509_delegation";

  // 1. Generate master and device key engines
  auto masterEngineRes = StandardCryptoEngine::generate();
  auto devEngineRes    = StandardCryptoEngine::generate();
  if (!masterEngineRes || !devEngineRes) {
    std::cerr << "Failed to generate crypto engines\n";
    return 1;
  }
  const auto &masterEngine = *masterEngineRes;
  const auto &devEngine    = *devEngineRes;

  const auto masterPrivOpt = masterEngine.rawPrivateKey();
  const auto devPrivOpt    = devEngine.rawPrivateKey();
  if (!masterPrivOpt || !devPrivOpt) {
    std::cerr << "Failed to extract raw private keys\n";
    return 1;
  }
  const auto masterPriv = *masterPrivOpt;
  const auto devPriv    = *devPrivOpt;
  const auto devPub     = devEngine.publicKey().bytes;

  const auto masterFp =
      StandardCryptoEngine::computeFingerprintSha256(masterEngine.publicKey());
  const auto devKeyHex = toHex(devPub);

  // 2. Generate X.509 delegation certificate and CSR
  CertificateConstraints constraints;
  constraints.serialNumber = 1001;
  constraints.notBefore    = 1700000000;
  constraints.notAfter     = 1700000000 + 365 * 86400;
  constraints.deviceId     = "fuzz-device";
  constraints.deviceName   = "Fuzz Test Device";
  constraints.authorName   = "Fuzz Test Author";
  constraints.authorEmail  = "author@fuzz.invalid";

  auto certRes = masterEngine.createDelegationCertificate(
      masterEngine, devEngine.publicKey(), constraints);
  if (!certRes) {
    std::cerr << "Failed to create delegation cert\n";
    return 1;
  }
  const auto &cert = *certRes;

  auto csrDerRes =
      devEngine.generateCsr(devEngine, "fuzz-device", "Fuzz Test Device");
  if (!csrDerRes) {
    std::cerr << "Failed to generate CSR\n";
    return 1;
  }
  const auto &csrDer = *csrDerRes;

  // -------------------------------------------------------------------------
  // Populate tests/fuzz/corpus/x509_delegation/
  // -------------------------------------------------------------------------
  // Seed 1: Valid DER-encoded RFC 8410 X.509 v3 delegation certificate
  writeFile(x509Dir / "01_delegation_cert.der", cert.der);

  // Seed 2: Valid PKCS#10 CSR DER bytes
  writeFile(x509Dir / "02_device_csr.der", csrDer);

  // Seed 3: Valid Base64-encoded certificate string
  const std::string b64Cert =
      StandardCryptoEngine::certificateToBase64Der(cert);
  writeFile(x509Dir / "03_delegation_cert_base64.txt", b64Cert);

  // Seed 4: Valid PEM-encoded certificate string
  auto pemCertRes = StandardCryptoEngine::certificateToPem(cert);
  if (pemCertRes) {
    writeFile(x509Dir / "04_delegation_cert_pem.crt", *pemCertRes);
  }

  // Seed 5: Alternate valid delegation certificate (different constraints)
  CertificateConstraints constraintsAlt;
  constraintsAlt.serialNumber = 2002;
  constraintsAlt.notBefore    = 1000;
  constraintsAlt.notAfter     = 2000000000;
  constraintsAlt.deviceId     = "alternate-device";
  constraintsAlt.deviceName   = "Alternate Fuzz Device";
  auto certAltRes             = masterEngine.createDelegationCertificate(
      masterEngine, devEngine.publicKey(), constraintsAlt);
  if (certAltRes) {
    writeFile(x509Dir / "05_delegation_cert_alt.der", certAltRes->der);
  }

  std::cout << "Generated X.509 delegation seeds in " << x509Dir << "\n";

  // -------------------------------------------------------------------------
  // Populate tests/fuzz/corpus/session_package/
  // -------------------------------------------------------------------------
  const std::string primediaText =
      "The quick brown fox jumps over the lazy dog";
  const std::vector<std::uint8_t> primediaBytes(primediaText.begin(),
                                                primediaText.end());

  CompactOpNode op1 = makeOp(0, 1048576, 20);
  CompactOpNode op2 = makeOp(20, 1048596, primediaText.size() - 20);

  // Seed 1: Clear valid .xuzzpkg
  {
    SessionPackage clearPkg;
    clearPkg.setManifestVersion(1)
        ->setMasterFingerprint(masterFp)
        ->setDeviceId("fuzz-device")
        ->setDeviceKey(devKeyHex)
        ->setBaseVersion("0")
        ->setHeadVersion("0a1")
        ->setPrimediaOffset(1048576)
        ->setPrimediaLength(primediaBytes.size())
        ->setTimestamp(1791480000)
        ->setPrimediaSlice(primediaBytes)
        ->setOpsNodes(std::vector<CompactOpNode>{op1, op2})
        ->setDeviceCert(b64Cert);

    auto signRes = clearPkg.signWithDeviceKey(devPriv);
    if (!signRes) {
      std::cerr << "Failed to sign clear package\n";
      return 1;
    }
    auto exportRes = exportPackage(clearPkg);
    if (!exportRes) {
      std::cerr << "Failed to export clear package\n";
      return 1;
    }
    writeFile(sessionDir / "01_clear_valid.xuzzpkg", *exportRes);
  }

  // Seed 2: Encrypted valid .xuzzpkg (ChaCha20-Poly1305 with Argon2id
  // salt/header)
  {
    SessionPackage encPkg;
    encPkg.setManifestVersion(1)
        ->setMasterFingerprint(masterFp)
        ->setDeviceId("fuzz-device")
        ->setDeviceKey(devKeyHex)
        ->setBaseVersion("0")
        ->setHeadVersion("0a1")
        ->setPrimediaOffset(0)
        ->setPrimediaLength(primediaBytes.size())
        ->setTimestamp(1791480000)
        ->setPrimediaSlice(primediaBytes)
        ->setOpsNodes(std::vector<CompactOpNode>{op1})
        ->setDeviceCert(b64Cert)
        ->setEncrypted(true)
        ->setPassphrase("xuzz_passphrase");

    auto signRes = encPkg.signWithDeviceKey(devPriv);
    if (!signRes) {
      std::cerr << "Failed to sign encrypted package\n";
      return 1;
    }
    auto exportRes = exportPackage(encPkg);
    if (!exportRes) {
      std::cerr << "Failed to export encrypted package\n";
      return 1;
    }
    writeFile(sessionDir / "02_encrypted_valid.xuzzpkg", *exportRes);
  }

  // Seed 3: Minimal manifest-only TSV
  {
    SessionManifest manifest;
    manifest.manifestVersion   = 1;
    manifest.masterFingerprint = masterFp;
    manifest.deviceId          = "fuzz-device";
    manifest.deviceKey         = devKeyHex;
    manifest.baseVersion       = "0";
    manifest.headVersion       = "0a1";
    manifest.primediaOffset    = 0;
    manifest.primediaLength    = 0;
    manifest.timestamp         = 1791480000;
    const std::string tsv      = manifest.serialize();
    writeFile(sessionDir / "03_minimal_manifest.tsv", tsv);
  }

  // Seed 4: Fat valid package with store tables, fragments, link packages
  {
    Store store;
    auto v1 =
        store.insert(MicroversionId{}, 0, "Hello Fuzz Fat Store Package!");
    LinkPackage lp;
    lp.curator = PublicKey{devPub};
    lp.title   = "Fuzz Curated Links";
    lp.salt    = "fuzz_salt_123";
    GlobalLink glink;
    glink.type  = LinkType::Quotation;
    glink.owner = "curator";
    glink.left.push_back(GlobalSpan{.scroll = "0", .start = 0, .length = 5});
    glink.right.push_back(GlobalSpan{.scroll = "0", .start = 6, .length = 4});
    lp.links.push_back(glink);

    auto fatPkgRes = createFatStorePackage(
        store, "fuzz-device", devPriv, masterFp, "0", v1.str(),
        /*encrypted=*/false, /*passphrase=*/"", {&lp, 1},
        /*existingDeviceCert=*/b64Cert,
        /*masterPrivateKey=*/masterPriv);
    if (fatPkgRes) {
      auto exportRes = exportPackage(*fatPkgRes);
      if (exportRes) {
        writeFile(sessionDir / "04_fat_valid.xuzzpkg", *exportRes);
      }
    }
  }

  // Seed 5: Small package with minimal payload
  {
    SessionPackage smallPkg;
    smallPkg.setManifestVersion(1)
        ->setMasterFingerprint(masterFp)
        ->setDeviceId("fuzz-device")
        ->setDeviceKey(devKeyHex)
        ->setBaseVersion("0")
        ->setHeadVersion("0")
        ->setPrimediaOffset(0)
        ->setPrimediaLength(1)
        ->setTimestamp(1791480000)
        ->setPrimediaSlice(std::string_view("X"))
        ->setOpsNodes(std::vector<CompactOpNode>{})
        ->setDeviceCert(b64Cert);

    auto signRes = smallPkg.signWithDeviceKey(devPriv);
    if (signRes) {
      auto exportRes = exportPackage(smallPkg);
      if (exportRes) {
        writeFile(sessionDir / "05_small_package.xuzzpkg", *exportRes);
      }
    }
  }

  std::cout << "Generated session package seeds in " << sessionDir << "\n";
  return 0;
}
