/**
 * @file media_manager.hpp
 * @brief Management of staged media assets, content hash deduplication, and
 * known sources collection for external media ingestion.
 */
#ifndef XUDU_MEDIA_MANAGER_HPP
#define XUDU_MEDIA_MANAGER_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "scroll.hpp"
#include <gleditor/source_grounder.hpp>

namespace xudu {

class Store;
class Swarm;

/**
 * @brief An external media asset staged locally prior to publication.
 */
struct StagedMedia {
  std::filesystem::path localPath;
  std::string mimeType;
  std::string sha256Hex;
  std::uint64_t sizeBytes{};
  std::string suggestedName;
};

/**
 * @class MediaManager
 * @brief Tracks staged media files and collects known sources for grounding.
 */
class MediaManager {
public:
  explicit MediaManager(std::filesystem::path stagingDirectory = {});
  ~MediaManager() = default;

  [[nodiscard]] const std::filesystem::path &stagingDir() const {
    return stagingDir_;
  }

  /**
   * @brief Stage an external media file into the staging area.
   */
  StagedMedia stageMediaFile(const std::filesystem::path &filePath);

  /**
   * @brief Stage raw bytes into the staging area.
   */
  StagedMedia stageMediaBytes(std::span<const std::uint8_t> bytes,
                              std::string suggestedName = "asset.bin");

  [[nodiscard]] const std::vector<StagedMedia> &stagedAssets() const {
    return staged_;
  }

  void clearStaged() { staged_.clear(); }

  /**
   * @brief Collect known sources from store scrolls and active swarms to
   * populate the grounding modal dropdown.
   */
  [[nodiscard]] std::vector<gleditor::KnownSource>
  collectKnownSources(const Store *store = nullptr,
                      const Swarm *swarm = nullptr) const;

private:
  std::filesystem::path stagingDir_;
  std::vector<StagedMedia> staged_;
};

} // namespace xudu

#endif // XUDU_MEDIA_MANAGER_HPP
