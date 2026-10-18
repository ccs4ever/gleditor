/**
 * @file store_loader.cpp
 * @brief Singular native store loading and file import pathways for Xanadu.
 */
#include "common/xanadu/store_loader.hpp"

#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/xanadu/segmented_ops_spool.hpp"
#include "common/xanadu/store_tables.hpp"

namespace xanadu {

void loadStore(Store &store, const std::filesystem::path &path) {
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error("Store path does not exist: " + path.string());
  }
  store.load(path.string());
}

std::unique_ptr<Store> loadStore(const std::filesystem::path &path,
                                 std::shared_ptr<UserPermascroll> permascroll) {
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error("Store path does not exist: " + path.string());
  }
  auto store = permascroll ? std::make_unique<Store>(std::move(permascroll))
                           : std::make_unique<Store>();
  store->load(path.string());
  return store;
}

std::unique_ptr<Store>
importFileStore(const std::filesystem::path &source,
                std::shared_ptr<UserPermascroll> permascroll,
                const std::filesystem::path &destination) {
  if (!std::filesystem::exists(source)) {
    throw std::runtime_error("Source file does not exist: " + source.string());
  }

  const auto fileSize = std::filesystem::file_size(source);
  if (fileSize > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(
        "Source file exceeds 4GB limit for native store: " + source.string());
  }

  std::ifstream stream(source, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Failed to open source file: " + source.string());
  }

  std::string content(static_cast<std::size_t>(fileSize), '\0');
  if (fileSize > 0) {
    stream.read(content.data(), static_cast<std::streamsize>(fileSize));
    if (!stream) {
      throw std::runtime_error("Failed to read source file content: " +
                               source.string());
    }
  }

  auto scroll = permascroll ? std::move(permascroll)
                            : std::make_shared<UserPermascroll>();
  auto store  = std::make_unique<Store>(scroll);

  if (!content.empty()) {
    store->insert(MicroversionId{}, 0, content);
  }

  if (!destination.empty()) {
    store->save(destination.string());
  }

  return store;
}

std::expected<ImportedStoreResult, SessionValidationError>
importStorePackage(const std::filesystem::path &packagePath,
                   const std::filesystem::path &destinationDir,
                   const ImportStorePackageOptions &options) {
  auto pkgRes = importPackage(packagePath, options.expectedMasterFingerprint,
                              options.verifySignature, options.masterPubKey,
                              options.passphrase);
  if (!pkgRes) {
    return std::unexpected(pkgRes.error());
  }
  return importStorePackage(*pkgRes, destinationDir, options);
}

std::expected<ImportedStoreResult, SessionValidationError>
importStorePackage(const SessionPackage &pkg,
                   const std::filesystem::path &destinationDir,
                   const ImportStorePackageOptions &options) {
  std::error_code ec;
  std::filesystem::create_directories(destinationDir, ec);
  if (ec) {
    return std::unexpected(SessionValidationError::IoError);
  }

  // 1. Write ops.nodes with valid 64 KiB OpsSegmentHeader
  const auto opsPath = destinationDir / "ops.nodes";
  const int fd =
      ::open(opsPath.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return std::unexpected(SessionValidationError::IoError);
  }

  const auto opsBytes  = pkg.opsNodesBytes();
  const auto nodeCount = pkg.opsNodes.size();

  OpsSegmentHeader header;
  header.signature     = opsSegmentSignature;
  header.formatVersion = opsSegmentFormatVersion;
  header.headerBytes   = opsSegmentHeaderBytes;
  header.nodeSize      = sizeof(CompactOpNode);
  header.flags         = 0;
  header.firstOpIndex  = 1;
  header.nodeCount     = nodeCount;

  if (::write(fd, &header, sizeof(header)) !=
          static_cast<ssize_t>(sizeof(header)) ||
      ::ftruncate(fd, static_cast<off_t>(opsSegmentHeaderBytes)) != 0) {
    ::close(fd);
    return std::unexpected(SessionValidationError::IoError);
  }
  if (!opsBytes.empty()) {
    if (::lseek(fd, static_cast<off_t>(opsSegmentHeaderBytes), SEEK_SET) < 0 ||
        ::write(fd, opsBytes.data(), opsBytes.size()) !=
            static_cast<ssize_t>(opsBytes.size())) {
      ::close(fd);
      return std::unexpected(SessionValidationError::IoError);
    }
  }
  ::close(fd);

  // 2. Write store.tables
  const auto tablesPath = destinationDir / "store.tables";
  if (!pkg.storeTables.empty()) {
    std::ofstream ofs(tablesPath, std::ios::binary);
    if (!ofs) {
      return std::unexpected(SessionValidationError::IoError);
    }
    ofs.write(reinterpret_cast<const char *>(pkg.storeTables.data()),
              static_cast<std::streamsize>(pkg.storeTables.size()));
    if (!ofs.good()) {
      return std::unexpected(SessionValidationError::IoError);
    }
  } else {
    StoreTables defaultTables;
    auto tableBytes = serializeStoreTables(defaultTables);
    std::ofstream ofs(tablesPath, std::ios::binary);
    if (!ofs) {
      return std::unexpected(SessionValidationError::IoError);
    }
    ofs.write(reinterpret_cast<const char *>(tableBytes.data()),
              static_cast<std::streamsize>(tableBytes.size()));
    if (!ofs.good()) {
      return std::unexpected(SessionValidationError::IoError);
    }
  }

  // 3. Load Store
  ImportedStoreResult result;
  result.package = pkg;
  result.store   = std::make_unique<Store>();
  try {
    result.store->load(destinationDir.string());
  } catch (const std::exception &) {
    return std::unexpected(SessionValidationError::CorruptOpsNodes);
  }

  // 4. Materialize scroll fragments
  struct LocalFragment {
    std::uint64_t start{0};
    std::vector<std::uint8_t> bytes;
  };
  std::vector<LocalFragment> localFrags;

  for (const auto &frag : pkg.getScrollFragments()) {
    if (frag.scrollId == localScroll) {
      localFrags.push_back({frag.start, frag.bytes});
      result.materializedFragments++;
    }
  }
  if (localFrags.empty() && !pkg.primediaSlice.empty()) {
    localFrags.push_back({pkg.primediaOffset, pkg.primediaSlice});
    result.materializedFragments++;
  }

  if (!localFrags.empty()) {
    std::string bKey =
        "pkg:" +
        (pkg.masterFingerprint.empty() ? pkg.deviceId : pkg.masterFingerprint);
    result.store->setBootstrapPermascroll(
        std::move(bKey),
        [frags =
             std::move(localFrags)](const PrimediaSpan &span) -> ResolveResult {
          for (const auto &frag : frags) {
            const auto fragEnd = frag.start + frag.bytes.size();
            const auto reqEnd  = span.start + span.length;
            if (span.start >= frag.start && reqEnd <= fragEnd) {
              const auto relOffset = span.start - frag.start;
              std::string text(
                  reinterpret_cast<const char *>(frag.bytes.data() + relOffset),
                  span.length);
              return ResolveResult{.status = ResolutionStatus::VerifiedBytes,
                                   .text   = std::move(text)};
            }
          }
          return ResolveResult{.status = ResolutionStatus::WithheldRedacted,
                               .text   = {}};
        });
  }

  for (const auto &frag : pkg.getScrollFragments()) {
    if (frag.scrollId != localScroll) {
      const auto which = result.store->scroll(frag.scrollId);
      if (which) {
        const std::string key =
            "btpk:" + which->publisher.hex() + ":" + which->salt;
        const std::string_view text(
            reinterpret_cast<const char *>(frag.bytes.data()),
            frag.bytes.size());
        result.store->setExternalLiveBytes(key, frag.start, text);
        result.materializedFragments++;
      }
    }
  }

  // 5. Adopt bundled link packages if requested
  if (options.adoptIncludedLinkPackages) {
    for (const auto &lp : pkg.getLinkPackages()) {
      try {
        const auto adoptRes =
            adoptLinkPackage(*result.store, lp, options.linkPackageTier);
        result.adoptedLinkPackages++;
        result.adoptedLinksCount += adoptRes.linksAdopted;
      } catch (...) {
        // Ignored unadoptable or invalid link package
      }
    }
  }

  return result;
}

} // namespace xanadu
