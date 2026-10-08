#ifndef XUDU_RETAINED_CONTENT_HPP
#define XUDU_RETAINED_CONTENT_HPP

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "scroll.hpp"

namespace xanadu {
struct PublicationSeed {
  InfoHash hash;
  std::string metainfo;
  std::filesystem::path savePath;
  std::uint64_t bytes{};
};

[[nodiscard]] PublicationSeed
reviewPublicationSeed(const InfoHash &hash,
                      const std::filesystem::path &directory);
/// Check a locally mounted torrent without consulting a content-source
/// fallback.
[[nodiscard]] PublicationSeed
reviewPublicationSeed(const PublicationSeed &seed);
/// Retain under <destination>/<hash>, verifying staged bytes before rename.
/// Existing corrupted targets and staging directories are preserved and
/// refused.
[[nodiscard]] PublicationSeed
retainPublicationSeed(const PublicationSeed &seed,
                      const std::filesystem::path &destination);

struct ContentRetentionStats {
  std::uint64_t copiedBytes{};
  std::size_t copiedSeeds{};
  std::size_t reusedSeeds{};
};
[[nodiscard]] ContentRetentionStats
retainScrollSeeds(std::span<const Scroll> scrolls,
                  const std::vector<std::filesystem::path> &roots,
                  const std::filesystem::path &destination,
                  const std::vector<PublicationSeed> &mounted = {});

struct ContentRetentionResult {
  std::uint64_t job{};
  std::filesystem::path destination;
  std::expected<ContentRetentionStats, std::string> outcome;
};
/// One local filesystem worker. Requests own their descriptors and paths;
/// no Store, Resolver or network content source is borrowed by the worker.
class ContentRetention {
public:
  ContentRetention();
  ~ContentRetention();
  ContentRetention(const ContentRetention &)            = delete;
  ContentRetention &operator=(const ContentRetention &) = delete;
  std::uint64_t queue(std::vector<Scroll> scrolls,
                      std::vector<std::filesystem::path> roots,
                      std::filesystem::path destination,
                      std::vector<PublicationSeed> mounted = {});
  std::vector<ContentRetentionResult> takeNotifications();
  std::vector<ContentRetentionResult> wait();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace xanadu
#endif
