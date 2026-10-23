/**
 * @file file_watch.cpp
 * @brief FileWatch: per-platform kernel change notification --
 *        inotify, kqueue EVFILT_VNODE, FindFirstChangeNotification.
 */

#include "common/xanadu/zigzag/file_watch.hpp"

#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||     \
    defined(__OpenBSD__) || defined(__DragonFly__)
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace zigzag {

#if defined(__linux__)

FileWatch::FileWatch(const std::filesystem::path &path) {
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
}

FileWatch::~FileWatch() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

bool FileWatch::dirty() noexcept {
  if (fd_ < 0) {
    return false;
  }
  alignas(struct inotify_event) char buf[4096];
  bool changed = false;
  for (;;) {
    const ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n <= 0) {
      return changed;
    }
    changed = true;
    if (n < static_cast<ssize_t>(sizeof buf)) {
      return true;
    }
  }
}

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||     \
    defined(__OpenBSD__) || defined(__DragonFly__)

FileWatch::FileWatch(const std::filesystem::path &path) {
  // O_EVTONLY opens for watching without touching the file; on macOS it
  // is O_EVTONLY, on the BSDs it falls back to O_RDONLY|O_NONBLOCK.
#ifdef O_EVTONLY
  fd_ = ::open(path.c_str(), O_EVTONLY | O_CLOEXEC);
#else
  fd_ = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
#endif
  if (fd_ < 0) {
    return;
  }
  kq_ = ::kqueue();
  if (kq_ < 0) {
    ::close(fd_);
    fd_ = -1;
    return;
  }
  struct kevent ev;
  EV_SET(&ev, fd_, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB, 0, nullptr);
  if (::kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
    ::close(kq_);
    ::close(fd_);
    kq_ = -1;
    fd_ = -1;
  }
}

FileWatch::~FileWatch() {
  if (kq_ >= 0) {
    ::close(kq_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

bool FileWatch::dirty() noexcept {
  if (kq_ < 0) {
    return false;
  }
  bool changed = false;
  struct kevent evs[8];
  const struct timespec noWait{0, 0};
  for (;;) {
    const int n = ::kevent(kq_, nullptr, 0, evs,
                           static_cast<int>(std::size(evs)), &noWait);
    if (n <= 0) {
      return changed;
    }
    changed = true;
    if (n < static_cast<int>(std::size(evs))) {
      return true;
    }
  }
}

#elif defined(_WIN32)

FileWatch::FileWatch(const std::filesystem::path &path) {
  const auto parent = path.parent_path();
  const auto dir    = parent.empty() ? path : parent;
  // The parent-directory watch is a superset: a sibling write marks the
  // watch dirty, and the lens's stamp decides whether ITS file moved.
  handle_ = ::FindFirstChangeNotificationW(dir.c_str(), FALSE,
                                           FILE_NOTIFY_CHANGE_FILE_NAME |
                                               FILE_NOTIFY_CHANGE_LAST_WRITE |
                                               FILE_NOTIFY_CHANGE_SIZE);
}

FileWatch::~FileWatch() {
  if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
    ::FindCloseChangeNotification(handle_);
  }
}

bool FileWatch::dirty() noexcept {
  if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool changed = false;
  // Drain each signaled state: Wait(0) true -> re-arm and look again.
  while (::WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0) {
    changed = true;
    if (!::FindNextChangeNotification(handle_)) {
      break; // the handle died -- the stamp stays the truth
    }
  }
  return changed;
}

#else // no kernel notifier on this platform -- the stamp is the signal

FileWatch::FileWatch(const std::filesystem::path &) {}
FileWatch::~FileWatch() = default;
bool FileWatch::dirty() noexcept { return false; }

#endif

} // namespace zigzag
