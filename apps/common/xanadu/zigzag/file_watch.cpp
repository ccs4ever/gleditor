/**
 * @file file_watch.cpp
 * @brief FileWatch: inotify-backed file-change notification.
 */

#include "common/xanadu/zigzag/file_watch.hpp"

#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace zigzag {

FileWatch::FileWatch(const std::filesystem::path &path) {
#if defined(__linux__)
  fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd_ < 0) {
    return;
  }
  if (::inotify_add_watch(fd_, path.c_str(),
                          IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF |
                              IN_ATTRIB | IN_MOVE) < 0) {
    ::close(fd_);
    fd_ = -1;
  }
#else
  (void)path;
#endif
}

FileWatch::~FileWatch() {
#if defined(__linux__)
  if (fd_ >= 0) {
    ::close(fd_);
  }
#endif
}

bool FileWatch::dirty() noexcept {
#if defined(__linux__)
  if (fd_ < 0) {
    return false;
  }
  // inotify_event is variable-length; a page covers the longest queue
  // slice this drain reads.
  alignas(struct inotify_event) char buf[4096];
  bool changed = false;
  for (;;) {
    const ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n <= 0) {
      return changed;
    }
    changed = true;
    if (n < static_cast<ssize_t>(sizeof buf)) {
      return true; // drained this pass
    }
  }
#else
  return false;
#endif
}

} // namespace zigzag
