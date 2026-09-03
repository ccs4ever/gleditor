/**
 * @file media_manager.cpp
 * @brief Implementation of MediaManager for staged media files and known
 * sources.
 */
#include "media_manager.hpp"

#include <fstream>
#include <gleditor/mimetype.hpp>
#include <gleditor/source_grounder.hpp>

#include "store.hpp"
#include "swarm.hpp"

namespace xudu {

MediaManager::MediaManager(std::filesystem::path stagingDirectory)
    : stagingDir_(std::move(stagingDirectory)) {
  if (stagingDir_.empty()) {
    stagingDir_ = std::filesystem::temp_directory_path() / "xudu_staged_media";
  }
  std::filesystem::create_directories(stagingDir_);
}

StagedMedia
MediaManager::stageMediaFile(const std::filesystem::path &filePath) {
  if (!std::filesystem::exists(filePath)) {
    throw std::runtime_error("Cannot stage non-existent file: " +
                             filePath.string());
  }

  const auto fileSize = std::filesystem::file_size(filePath);
  std::ifstream file(filePath, std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file for staging: " +
                             filePath.string());
  }

  std::vector<std::uint8_t> buffer((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
  const auto sha256 = gleditor::SourceGrounder::computeSha256(buffer);
  const auto detectedMime =
      gleditor::MimeDetector::detectFile(filePath.string());

  const auto destFileName =
      sha256.substr(0, 16) + "_" + filePath.filename().string();
  const auto destPath = stagingDir_ / destFileName;

  if (!std::filesystem::exists(destPath)) {
    std::filesystem::copy_file(
        filePath, destPath, std::filesystem::copy_options::overwrite_existing);
  }

  StagedMedia media{
      .localPath     = destPath,
      .mimeType      = detectedMime.essence(),
      .sha256Hex     = sha256,
      .sizeBytes     = fileSize,
      .suggestedName = filePath.filename().string(),
  };
  staged_.push_back(media);
  return media;
}

StagedMedia MediaManager::stageMediaBytes(std::span<const std::uint8_t> bytes,
                                          std::string suggestedName) {
  const auto sha256       = gleditor::SourceGrounder::computeSha256(bytes);
  const auto detectedMime = gleditor::MimeDetector::detectBuffer(bytes);

  const auto destFileName = sha256.substr(0, 16) + "_" + suggestedName;
  const auto destPath     = stagingDir_ / destFileName;

  {
    std::ofstream out(destPath, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }

  StagedMedia media{
      .localPath     = destPath,
      .mimeType      = detectedMime.essence(),
      .sha256Hex     = sha256,
      .sizeBytes     = bytes.size(),
      .suggestedName = std::move(suggestedName),
  };
  staged_.push_back(media);
  return media;
}

std::vector<gleditor::KnownSource>
MediaManager::collectKnownSources([[maybe_unused]] const Store *store,
                                  [[maybe_unused]] const Swarm *swarm) const {
  std::vector<gleditor::KnownSource> sources;

  // 1. Add locally staged assets
  for (const auto &staged : staged_) {
    sources.push_back(gleditor::KnownSource{
        .displayName  = "[Staged] " + staged.suggestedName,
        .pathOrMagnet = staged.localPath.string(),
        .mimeType     = staged.mimeType,
        .infoHash     = "",
    });
  }

  // 2. Add active documents/versions from store
  if (nullptr != store) {
    const auto allV = store->allVersions();
    for (const auto &v : allV) {
      const auto text = store->textOf(v);
      const auto label =
          text.empty() ? v.str() : v.str() + ": " + text.substr(0, 30);
      sources.push_back(gleditor::KnownSource{
          .displayName  = "[State " + label + "]",
          .pathOrMagnet = v.str(),
          .mimeType     = "text/plain",
          .infoHash     = "",
      });
    }
  }

  return sources;
}

} // namespace xudu
