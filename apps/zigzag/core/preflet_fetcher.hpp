/**
 * @file preflet_fetcher.hpp
 * @brief Fetches Preflet-linked Slices over BitTorrent (libtorrent-rasterbar).
 */
#ifndef ZIGZAG_PREFLET_FETCHER_HPP
#define ZIGZAG_PREFLET_FETCHER_HPP

#include "zzstructure.hpp"

#include <memory>
#include <string>

namespace zigzag {

class PrefletFetcher {
public:
  enum class Status {
    Idle,     ///< Nothing in flight
    Fetching, ///< Metadata and/or payload in progress
    Ready,    ///< Slice path is populated and ready to load
    Failed    ///< Message explains why
  };

  struct Progress {
    Status status = Status::Idle;
    std::string message;    ///< Short human-readable state
    float fraction = 0.0F;  ///< 0..1 payload progress
    std::string slice_path; ///< Absolute path to fetched Slice, once Ready
  };

  PrefletFetcher();
  ~PrefletFetcher();

  PrefletFetcher(const PrefletFetcher &)            = delete;
  PrefletFetcher &operator=(const PrefletFetcher &) = delete;
  PrefletFetcher(PrefletFetcher &&) noexcept;
  PrefletFetcher &operator=(PrefletFetcher &&) noexcept;

  /**
   * @brief Starts fetching the Slice @p preflet points at.
   *
   * @param preflet The preflet to follow.
   * @param error Output error description if start fails.
   * @return True if download was successfully started.
   */
  bool begin(const Preflet &preflet, std::string &error);

  /**
   * @brief Drains libtorrent alerts and updates progress. Non-blocking.
   */
  void poll();

  /**
   * @brief Abandons any in-flight fetch and returns to Idle.
   */
  void cancel();

  /**
   * @brief Clears a Ready/Failed result back to Idle.
   */
  void acknowledge();

  [[nodiscard]] const Progress &progress() const;
  [[nodiscard]] bool busy() const;

  /**
   * @brief Cache directory: $XDG_CACHE_HOME/zigzag/slices, or
   *        ~/.cache/zigzag/slices.
   */
  [[nodiscard]] static std::string cacheRoot();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace zigzag

#endif // ZIGZAG_PREFLET_FETCHER_HPP
