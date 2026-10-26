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

namespace {
// Drain inotify records in bounded reads until the fd is empty.
constexpr std::size_t kInotifyReadBufferBytes{4096};
} // namespace

FileWatch::FileWatch(const std::filesystem::path &path) {
  const int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd < 0) {
    return;
  }
  fd_ = fd;
  if (::inotify_add_watch(fd, path.c_str(),
                          IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF |
                              IN_ATTRIB | IN_MOVE) < 0) {
    ::close(fd);
    fd_.reset();
  }
}

FileWatch::~FileWatch() {
  if (fd_) {
    ::close(*fd_);
  }
}

bool FileWatch::dirty() noexcept {
  if (!fd_) {
    return false;
  }
  alignas(struct inotify_event) char buf[kInotifyReadBufferBytes];
  bool changed = false;
  for (;;) {
    const ssize_t n = ::read(*fd_, buf, sizeof buf);
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

namespace {
// Small batches keep kqueue draining bounded while allowing repeated polls.
constexpr std::size_t kKqueueEventBatchSize{8};
} // namespace

FileWatch::FileWatch(const std::filesystem::path &path) {
  // O_EVTONLY opens for watching without touching the file; on macOS it
  // is O_EVTONLY, on the BSDs it falls back to O_RDONLY|O_NONBLOCK.
#ifdef O_EVTONLY
  const int fd = ::open(path.c_str(), O_EVTONLY | O_CLOEXEC);
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
#endif
  if (fd < 0) {
    return;
  }
  fd_          = fd;
  const int kq = ::kqueue();
  if (kq < 0) {
    ::close(fd);
    fd_.reset();
    return;
  }
  kq_ = kq;
  struct kevent ev;
  EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB, 0, nullptr);
  if (::kevent(kq, &ev, 1, nullptr, 0, nullptr) < 0) {
    ::close(kq);
    ::close(fd);
    kq_.reset();
    fd_.reset();
  }
}

FileWatch::~FileWatch() {
  if (kq_) {
    ::close(*kq_);
  }
  if (fd_) {
    ::close(*fd_);
  }
}

bool FileWatch::dirty() noexcept {
  if (!kq_) {
    return false;
  }
  bool changed = false;
  struct kevent evs[kKqueueEventBatchSize];
  const struct timespec noWait{0, 0};
  for (;;) {
    const int n = ::kevent(*kq_, nullptr, 0, evs,
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
  const HANDLE handle = ::FindFirstChangeNotificationW(
      dir.c_str(), FALSE,
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
          FILE_NOTIFY_CHANGE_SIZE);
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
    handle_ = handle;
  }
}

FileWatch::~FileWatch() {
  if (handle_) {
    ::FindCloseChangeNotification(*handle_);
  }
}

bool FileWatch::dirty() noexcept {
  if (!handle_) {
    return false;
  }
  bool changed = false;
  // Drain each signaled state: Wait(0) true -> re-arm and look again.
  for (;;) {
    const DWORD waitResult = ::WaitForSingleObject(*handle_, 0);
    if (waitResult == WAIT_FAILED) {
      ::FindCloseChangeNotification(*handle_);
      handle_.reset();
      break;
    }
    if (waitResult != WAIT_OBJECT_0) {
      break;
    }
    changed = true;
    if (!::FindNextChangeNotification(*handle_)) {
      ::FindCloseChangeNotification(*handle_);
      handle_.reset();
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
