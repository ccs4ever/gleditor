/**
 * @file package_inspector.hpp
 * @brief ELF binary verification, shadowing enforcement, and topological
 * dependency sorting (P21 K3, K4).
 */
#ifndef COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_INSPECTOR_HPP
#define COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_INSPECTOR_HPP

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xanadu::vagent::cluster {

enum class InspectorError {
  Ok,
  MissingSonameTag,
  InvalidSonamePath,
  InsecureRpath,
  InterpForbidden,
  SystemLibraryShadowingForbidden,
  UnresolvedLibraryDependency,
  CyclicLibraryDependency,
  UnsupportedFormat,
  CorruptBinary,
  FileNotFound,
};

[[nodiscard]] std::string_view toString(InspectorError error) noexcept;

struct InspectedElf {
  std::string soname;
  std::vector<std::string> needed;
  std::vector<std::string> runpaths;
  std::vector<std::string> rpaths;
  bool hasInterp{false};
};

/// Inspects an ELF binary from a filesystem path.
[[nodiscard]] std::expected<InspectedElf, InspectorError>
inspectElf(const std::filesystem::path &path);

/// Inspects an ELF binary in-memory.
[[nodiscard]] std::expected<InspectedElf, InspectorError>
inspectElfBytes(std::span<const std::uint8_t> bytes);

/// Validates a single bundled shared library binary against the packaging
/// contract:
/// - Verifies DT_SONAME tag exists and matches expectedSoname
/// - Validates expectedSoname conforms to K3 regex allowlist & path rules
/// - Ensures soname does not shadow system libraries (libc, libm, ld-linux,
/// etc.)
/// - Verifies no PT_INTERP program header
/// - Verifies DT_RUNPATH / DT_RPATH contains strictly $ORIGIN or $ORIGIN/lib
[[nodiscard]] std::expected<InspectedElf, InspectorError>
validateBundledLibrary(const std::filesystem::path &path,
                       std::string_view expectedSoname);

[[nodiscard]] std::expected<InspectedElf, InspectorError>
validateBundledLibraryBytes(std::span<const std::uint8_t> bytes,
                            std::string_view expectedSoname);

struct LibraryDependencyNode {
  std::string soname;
  std::vector<std::string> needed;
};

/// Computes topological dependency ordering for a collection of companion
/// libraries and the primary module:
/// - Checks that bundled library sonames do not shadow system libraries
/// - Checks that all DT_NEEDED entries resolve either to another bundled
/// library
///   or to declared/standard system libraries
/// - Detects circular dependencies (Kahn's algorithm)
/// - Returns sonames in topological load order: deepest dependencies (leaf
/// nodes) first,
///   dependent libraries next, and root module last.
[[nodiscard]] std::expected<std::vector<std::string>, InspectorError>
topologicalSortLibraries(
    const std::vector<LibraryDependencyNode> &libraries,
    const std::vector<std::string> &declaredSystemLibraries = {},
    std::string_view rootModuleSoname                       = {});

} // namespace xanadu::vagent::cluster

#endif // COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_INSPECTOR_HPP
