#include "common/xanadu/vagent/native/ipc.hpp"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace xanadu::vagent::native::ipc {

RingProducer::RingProducer(std::atomic<uint64_t> *head,
                           const std::atomic<uint64_t> *tail, uint8_t *data,
                           uint64_t capacity, uint32_t max_frame_bytes,
                           int doorbell_fd)
    : head_(head), tail_(tail), data_(data), capacity_(capacity),
      max_frame_bytes_(max_frame_bytes), doorbell_fd_(doorbell_fd) {}

std::expected<void, std::string>
RingProducer::writeFrame(FrameType type, uint64_t seq,
                         std::span<const uint8_t> token,
                         std::span<const uint8_t> payload) {
  if (!head_ || !tail_ || !data_ || capacity_ == 0) {
    return std::unexpected("RingProducer uninitialized");
  }

  const uint32_t total_len =
      static_cast<uint32_t>(sizeof(FrameHeader) + payload.size());
  if (total_len > max_frame_bytes_) {
    return std::unexpected("Frame size exceeds max_frame_bytes");
  }

  const uint64_t padded_len = (total_len + 7ULL) & ~7ULL;
  const uint64_t mask       = capacity_ - 1;

  uint64_t current_head = head_->load(std::memory_order_relaxed);
  uint64_t current_tail = tail_->load(std::memory_order_acquire);

  if (current_head - current_tail + padded_len > capacity_) {
    return std::unexpected("Ring buffer is full (backpressure)");
  }

  uint64_t offset     = current_head & mask;
  uint64_t bytes_left = capacity_ - offset;

  if (bytes_left < padded_len) {
    // Write wrap marker if enough space for wrap header
    if (bytes_left >= sizeof(FrameHeader)) {
      FrameHeader wrap{};
      wrap.frame_len = kWrapMarker;
      wrap.magic     = kFrameMagic;
      std::memcpy(data_ + offset, &wrap, sizeof(FrameHeader));
    }
    // Advance head to the beginning of the ring buffer
    current_head = (current_head + capacity_ - 1) & ~(capacity_ - 1);
    offset       = 0;

    // Recheck space after wrap
    if (current_head - current_tail + padded_len > capacity_) {
      return std::unexpected("Ring buffer is full after wrap");
    }
  }

  FrameHeader hdr{};
  hdr.frame_len = total_len;
  hdr.magic     = kFrameMagic;
  hdr.type      = type;
  hdr.seq       = seq;
  if (!token.empty()) {
    const size_t copy_len = std::min(token.size(), sizeof(hdr.token));
    std::memcpy(hdr.token, token.data(), copy_len);
  }

  std::memcpy(data_ + offset, &hdr, sizeof(FrameHeader));
  if (!payload.empty()) {
    std::memcpy(data_ + offset + sizeof(FrameHeader), payload.data(),
                payload.size());
  }

  // Publish updated head
  head_->store(current_head + padded_len, std::memory_order_release);

  if (doorbell_fd_ >= 0) {
    (void)ringDoorbell(doorbell_fd_);
  }

  return {};
}

RingConsumer::RingConsumer(const std::atomic<uint64_t> *head,
                           std::atomic<uint64_t> *tail, const uint8_t *data,
                           uint64_t capacity, uint32_t max_frame_bytes,
                           int doorbell_fd)
    : head_(head), tail_(tail), data_(data), capacity_(capacity),
      max_frame_bytes_(max_frame_bytes), doorbell_fd_(doorbell_fd),
      scratch_buffer_(max_frame_bytes) {}

std::expected<std::optional<DecodedFrame>, std::string>
RingConsumer::pollFrame() {
  if (!head_ || !tail_ || !data_ || capacity_ == 0) {
    return std::unexpected("RingConsumer uninitialized");
  }

  const uint64_t mask   = capacity_ - 1;
  uint64_t current_tail = tail_->load(std::memory_order_relaxed);
  uint64_t current_head = head_->load(std::memory_order_acquire);

  if (current_tail >= current_head) {
    return std::nullopt;
  }

  uint64_t offset     = current_tail & mask;
  uint64_t bytes_left = capacity_ - offset;

  // Check for wrap marker
  if (bytes_left < sizeof(FrameHeader)) {
    current_tail = (current_tail + capacity_ - 1) & ~(capacity_ - 1);
    offset       = 0;
    bytes_left   = capacity_;
  } else {
    FrameHeader peek_hdr;
    std::memcpy(&peek_hdr, data_ + offset, sizeof(FrameHeader));
    if (peek_hdr.frame_len == kWrapMarker) {
      current_tail = (current_tail + capacity_ - 1) & ~(capacity_ - 1);
      offset       = 0;
      bytes_left   = capacity_;
    }
  }

  if (current_tail >= current_head) {
    tail_->store(current_tail, std::memory_order_release);
    return std::nullopt;
  }

  FrameHeader hdr;
  std::memcpy(&hdr, data_ + offset, sizeof(FrameHeader));

  if (hdr.magic != kFrameMagic) {
    return std::unexpected("Protocol violation: invalid frame magic 0x" +
                           std::to_string(hdr.magic));
  }

  if (hdr.frame_len < sizeof(FrameHeader) || hdr.frame_len > max_frame_bytes_) {
    return std::unexpected("Protocol violation: invalid frame length " +
                           std::to_string(hdr.frame_len));
  }

  const uint64_t padded_len = (hdr.frame_len + 7ULL) & ~7ULL;
  if (padded_len > bytes_left) {
    return std::unexpected(
        "Protocol violation: frame extends beyond physical ring boundary "
        "without wrap marker");
  }
  if (current_tail + padded_len > current_head) {
    return std::unexpected("Protocol violation: frame extends beyond head");
  }

  // Copy into private scratch buffer to defeat shared-memory TOCTOU
  std::memcpy(scratch_buffer_.data(), data_ + offset, hdr.frame_len);

  // Advance consumer tail
  tail_->store(current_tail + padded_len, std::memory_order_release);

  DecodedFrame result;
  result.header            = hdr;
  const size_t payload_len = hdr.frame_len - sizeof(FrameHeader);
  if (payload_len > 0) {
    result.payload.assign(scratch_buffer_.data() + sizeof(FrameHeader),
                          scratch_buffer_.data() + hdr.frame_len);
  }

  return result;
}

std::expected<DecodedFrame, std::string>
RingConsumer::waitFrame(int timeout_ms) {
  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto frame_opt = pollFrame();
    if (!frame_opt) {
      return std::unexpected(frame_opt.error());
    }
    if (frame_opt->has_value()) {
      return std::move(**frame_opt);
    }

    if (timeout_ms == 0) {
      return std::unexpected("Timed out waiting for frame");
    }

    int remaining_ms = -1;
    if (timeout_ms > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
      if (elapsed >= timeout_ms) {
        return std::unexpected("Timed out waiting for frame");
      }
      remaining_ms = static_cast<int>(timeout_ms - elapsed);
    }

    if (doorbell_fd_ >= 0) {
      auto waited =
          waitDoorbell(doorbell_fd_, remaining_ms >= 0 ? remaining_ms : 50);
      if (!waited) {
        return std::unexpected(waited.error());
      }
    } else {
      usleep(1000);
    }
  }
}

std::expected<void, std::string> ringDoorbell(int fd) {
  if (fd < 0) {
    return std::unexpected("Invalid doorbell fd");
  }
  const uint64_t val    = 1;
  const ssize_t written = ::write(fd, &val, sizeof(val));
  if (written != sizeof(val)) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return {}; // Non-blocking doorbell already signaled
    }
    return std::unexpected("Failed to write to doorbell: " +
                           std::string(std::strerror(errno)));
  }
  return {};
}

std::expected<bool, std::string> waitDoorbell(int fd, int timeout_ms) {
  if (fd < 0) {
    return std::unexpected("Invalid doorbell fd");
  }

  struct pollfd pfd{};
  pfd.fd     = fd;
  pfd.events = POLLIN;

  const int rc = ::poll(&pfd, 1, timeout_ms);
  if (rc < 0) {
    if (errno == EINTR) {
      return false;
    }
    return std::unexpected("Poll on doorbell failed: " +
                           std::string(std::strerror(errno)));
  }
  if (rc == 0) {
    return false; // Timeout
  }

  if (pfd.revents & POLLIN) {
    uint64_t val    = 0;
    const ssize_t r = ::read(fd, &val, sizeof(val));
    (void)r;
    return true;
  }

  return false;
}

std::expected<void, std::string>
sendGrantInstall(int sock_fd, const GrantInstall &install, int fd_to_pass) {
  if (sock_fd < 0) {
    return std::unexpected("Invalid socket fd");
  }

  struct msghdr msg{};
  struct iovec iov{};

  iov.iov_base   = const_cast<GrantInstall *>(&install);
  iov.iov_len    = sizeof(install);
  msg.msg_iov    = &iov;
  msg.msg_iovlen = 1;

  char cmsg_buf[CMSG_SPACE(sizeof(int))];
  if (fd_to_pass >= 0) {
    msg.msg_control    = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level     = SOL_SOCKET;
    cmsg->cmsg_type      = SCM_RIGHTS;
    cmsg->cmsg_len       = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd_to_pass, sizeof(int));
  }

  const ssize_t sent = ::sendmsg(sock_fd, &msg, 0);
  if (sent < 0) {
    return std::unexpected("sendmsg GrantInstall failed: " +
                           std::string(std::strerror(errno)));
  }
  if (static_cast<size_t>(sent) != sizeof(install)) {
    return std::unexpected("Short write in sendmsg GrantInstall");
  }

  return {};
}

std::expected<void, std::string>
recvGrantInstall(int sock_fd, GrantInstall &install, int &received_fd) {
  if (sock_fd < 0) {
    return std::unexpected("Invalid socket fd");
  }

  received_fd = -1;

  struct msghdr msg{};
  struct iovec iov{};

  iov.iov_base   = &install;
  iov.iov_len    = sizeof(install);
  msg.msg_iov    = &iov;
  msg.msg_iovlen = 1;

  char cmsg_buf[CMSG_SPACE(sizeof(int))];
  msg.msg_control    = cmsg_buf;
  msg.msg_controllen = sizeof(cmsg_buf);

  const ssize_t recvd = ::recvmsg(sock_fd, &msg, MSG_CMSG_CLOEXEC);
  if (recvd <= 0) {
    if (recvd == 0) {
      return std::unexpected("Control socket EOF");
    }
    return std::unexpected("recvmsg GrantInstall failed: " +
                           std::string(std::strerror(errno)));
  }

  if (static_cast<size_t>(recvd) != sizeof(install)) {
    return std::unexpected("Short read in recvmsg GrantInstall");
  }

  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
       cmsg                 = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      std::memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
      break;
    }
  }

  return {};
}

std::expected<void, std::string> sendGrantAck(int sock_fd,
                                              const GrantAck &ack) {
  if (sock_fd < 0) {
    return std::unexpected("Invalid socket fd");
  }

  const ssize_t sent = ::send(sock_fd, &ack, sizeof(ack), 0);
  if (sent != sizeof(ack)) {
    return std::unexpected("Failed to send GrantAck: " +
                           std::string(std::strerror(errno)));
  }
  return {};
}

std::expected<void, std::string> recvGrantAck(int sock_fd, GrantAck &ack) {
  if (sock_fd < 0) {
    return std::unexpected("Invalid socket fd");
  }

  const ssize_t recvd = ::recv(sock_fd, &ack, sizeof(ack), MSG_WAITALL);
  if (recvd <= 0) {
    if (recvd == 0) {
      return std::unexpected("Control socket EOF waiting for GrantAck");
    }
    return std::unexpected("Failed to receive GrantAck: " +
                           std::string(std::strerror(errno)));
  }
  if (recvd != sizeof(ack)) {
    return std::unexpected("Short read for GrantAck");
  }
  return {};
}

} // namespace xanadu::vagent::native::ipc
