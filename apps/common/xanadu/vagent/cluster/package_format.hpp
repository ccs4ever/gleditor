/**
 * @file package_format.hpp
 * @brief Wire and artifact representations for native programs (.vnprog, P20)
 *        and cluster packages (.vnpkg, P21).
 */
#ifndef COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_FORMAT_HPP
#define COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_FORMAT_HPP

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanadu::vagent::cluster {

inline constexpr std::string_view kPackageMagic            = "\x89VAGNPKG\n";
inline constexpr std::string_view kNativeProgramMagic      = "\x89VAGNATV";
inline constexpr std::uint32_t kNativeProgramFormatVersion = 1;
inline constexpr std::string_view kPackageFormatVersion    = "vagent-package-1";

/// Length-prefixed encoding L(s) with 8-byte big-endian prefix.
[[nodiscard]] std::string lengthPrefixed(std::string_view s);

/// P20 N6 programRef: SHA-256(L(kProgramFormat) || L("vagent-native/1") ||
/// L(opLog) || L(moduleDigest))
[[nodiscard]] std::string
computeNativeProgramRef(std::string_view opLog, std::string_view moduleDigest);

/// P21 K1 logical programRef: SHA-256(L(kProgramFormat) ||
/// L("vagent-package/1") || L(kind) || L(canonicalBody) || L(targetsDigest))
[[nodiscard]] std::string
computePackageProgramRef(std::string_view kind, std::string_view canonicalBody,
                         std::string_view targetsDigest);

struct NativeProgramArtifact {
  std::int64_t apiVersion{1};
  std::string opLog;
  std::string moduleDigest; // 64-hex SHA-256
  std::string moduleFilename;
  std::string programRef; // 64-hex computed ref
};

[[nodiscard]] std::string encodeVnprog(const NativeProgramArtifact &artifact);
[[nodiscard]] std::expected<NativeProgramArtifact, std::string>
decodeVnprog(std::string_view bytes);

struct PackageModule {
  std::string filename;
  std::string sha256;
  std::int64_t size{0};
  std::string infoHash;
};

struct PackageBundledLib {
  std::string name;
  std::string canonicalSoname;
  std::string soname;
  std::string sha256;
  std::int64_t size{0};
  std::string infoHash;
};

struct PackageTarget {
  PackageModule module;
  std::vector<PackageBundledLib> bundledLibs;
  std::map<std::string, std::string> systemLibraryAliases;
};

struct PackageProvenance {
  std::string author;
  std::string device;
  std::int64_t timestamp{0};
  std::string publicKey;
};

struct PackageManifest {
  std::int64_t apiVersion{1};
  std::string format{"vagent-package-1"};
  std::string kind{"pure"}; // "pure", "source+native", "native"
  std::string compilerFormat{"vagent-program-3"};
  std::string programBody;
  std::map<std::string, PackageTarget> targets;
  PackageProvenance provenance;
  std::string signature;
  std::map<std::string, std::string>
      payloads; // sha256 -> binary payload (standalone .vnpkg)
};

[[nodiscard]] std::string
encodeTargets(const std::map<std::string, PackageTarget> &targets);
[[nodiscard]] std::string
computeTargetsDigest(const std::map<std::string, PackageTarget> &targets);

[[nodiscard]] std::string encodePackageManifest(const PackageManifest &manifest,
                                                bool standalone = true);

[[nodiscard]] std::expected<PackageManifest, std::string>
decodePackageManifest(std::string_view bytes);

[[nodiscard]] bool validateSoname(std::string_view soname, std::string &error);
[[nodiscard]] bool isShadowingForbidden(std::string_view soname);

[[nodiscard]] std::string formatInspectReport(const PackageManifest &manifest,
                                              std::string_view programRef);

/// Append approval event in the given authority ledger directory.
[[nodiscard]] std::expected<void, std::string>
recordApproval(std::string_view ledgerDir, std::string_view programRef,
               std::string_view kind, std::string_view author);

/// Append withdrawal event in the given authority ledger directory.
[[nodiscard]] std::expected<void, std::string>
recordWithdrawal(std::string_view ledgerDir, std::string_view programRef,
                 std::string_view kind, std::string_view author);

/// Check whether an authority approval is currently active in the given ledger
/// directory.
[[nodiscard]] bool isApproved(std::string_view ledgerDir,
                              std::string_view programRef,
                              std::string_view kind);

} // namespace xanadu::vagent::cluster

#endif // COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_FORMAT_HPP
