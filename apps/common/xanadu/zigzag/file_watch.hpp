/**
 * @file file_watch.hpp
 * @brief FileWatch: kernel-signaled change detection under the pulled
 *        pollChanged() contract -- inotify on Linux, inert elsewhere.
 *
 * The evented half the lens taxonomy calls "push": a filesystem write
 * lands as a kernel event even when the mtime tick hasn't moved, so a
 * lens's pollChanged() can answer true inside one mtime quantum. No
 * thread -- the watch drains a nonblocking fd at the lens's ordinary
 * poll point, which stays the marks-quiesced boundary.
 */
#ifndef COMMON_XANADU_ZIGZAG_FILE_WATCH_HPP
#define COMMON_XANADU_ZIGZAG_FILE_WATCH_HPP

#include <filesystem>

namespace zigzag {

/// One watched file. Inert (never dirty) when the platform lacks
/// inotify or the watch fails to install -- the caller's stamp check
/// stays the ground truth in that case.
class FileWatch {
public:
  /// Watch @p path for modify/move/delete/attrib. Never throws.
  explicit FileWatch(const std::filesystem::path &path);
  ~FileWatch();
  FileWatch(const FileWatch &)            = delete;
  FileWatch &operator=(const FileWatch &) = delete;

  /// Any pending kernel event? Drains the queue.
  [[nodiscard]] bool dirty() noexcept;
  [[nodiscard]] bool active() const noexcept { return fd_ >= 0; }

private:
  int fd_{-1};
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_FILE_WATCH_HPP
