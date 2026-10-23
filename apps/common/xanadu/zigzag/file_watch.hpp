/**
 * @file file_watch.hpp
 * @brief FileWatch: kernel-signaled change detection under the pulled
 *        pollChanged() contract on every supported platform.
 *
 * The evented half the lens taxonomy calls "push": a filesystem write
 * lands as a kernel event even when the mtime tick hasn't moved, so a
 * lens's pollChanged() can answer true inside one mtime quantum. No
 * thread -- the watch drains its OS event queue at the lens's ordinary
 * poll point, which stays the marks-quiesced boundary.
 *
 * Backends:
 *   Linux:      inotify (nonblocking fd, per-file watch)
 *   macOS/BSD:  kqueue EVFILT_VNODE on the file's own descriptor
 *   Windows:    FindFirstChangeNotification on the parent directory
 *               (a conservative superset -- a sibling write also marks
 *               dirty; the caller's stamp decides whether the file
 *               itself moved)
 *   other:      inert -- never dirty, and the mtime+size stamp the
 *               lenses already keep is the sole signal
 *
 * All four degrade identically: inactive means "ask the stamp", never
 * a lie.
 */
#ifndef COMMON_XANADU_ZIGZAG_FILE_WATCH_HPP
#define COMMON_XANADU_ZIGZAG_FILE_WATCH_HPP

#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace zigzag {

/// One watched file. Inert (never dirty, active()==false) when the
/// platform has no notifier or the watch fails to install.
class FileWatch {
public:
  /// Watch @p path for modify/move/delete/attrib. Never throws.
  explicit FileWatch(const std::filesystem::path &path);
  ~FileWatch();
  FileWatch(const FileWatch &)            = delete;
  FileWatch &operator=(const FileWatch &) = delete;

  /// Any pending kernel event? Drains the queue.
  [[nodiscard]] bool dirty() noexcept;

  /// A waitable descriptor for an event loop's epoll/poll/select --
  /// the inotify fd on Linux/Android (EPOLLET-safe: dirty() drains to
  /// emptiness, the edge-triggered pattern the platform docs require),
  /// the kqueue fd on BSD/macOS, -1 on Windows (WaitForMultipleObjects
  /// would take the change handle, not an fd) and inert builds. The
  /// fd is not owned by the caller and outlives as long as the watch.
  [[nodiscard]] int waitableFd() const noexcept {
#if defined(_WIN32)
    return -1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||     \
    defined(__OpenBSD__) || defined(__DragonFly__)
    return kq_;
#else
    return fd_;
#endif
  }

  [[nodiscard]] bool active() const noexcept {
#if defined(_WIN32)
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
#else
    return fd_ >= 0;
#endif
  }

private:
#if defined(_WIN32)
  HANDLE handle_{nullptr}; ///< change-notification handle on the dir
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||     \
    defined(__OpenBSD__) || defined(__DragonFly__)
  int kq_{-1}; ///< the kqueue itself
  int fd_{-1}; ///< the watched file's O_EVTONLY descriptor
#else
  int fd_{-1}; ///< inotify fd (Linux) or nothing
#endif
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_FILE_WATCH_HPP
