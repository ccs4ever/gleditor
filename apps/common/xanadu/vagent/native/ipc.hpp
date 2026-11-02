#ifndef VAGENT_NATIVE_HOST_IPC_HPP
#define VAGENT_NATIVE_HOST_IPC_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xanadu::vagent::native::ipc {

inline constexpr uint16_t kFrameMagic       = 0x564E; // "VN"
inline constexpr uint32_t kWrapMarker       = 0xFFFFFFFFU;
inline constexpr uint64_t kDefaultRingBytes = 1024 * 1024; // 1 MiB (power of 2)
inline constexpr uint32_t kDefaultFrameBytes = 64 * 1024;  // 64 KiB

enum class FrameType : uint16_t {
  StartEffect  = 1,
  CancelEffect = 2,
  Revoke       = 3,
  ArmReadable  = 4,
  Shutdown     = 5,
  StartTurn    = 6,

  Ready        = 100,
  EffectDone   = 101,
  EffectFailed = 102,
  Readable     = 103,
  Log          = 104,
  TurnDone     = 105,
  TurnFailed   = 106,
};

struct alignas(8) FrameHeader {
  uint32_t frame_len{0}; // total bytes including FrameHeader, or kWrapMarker
  uint16_t magic{kFrameMagic};
  FrameType type{FrameType::Ready};
  uint64_t seq{0};
  uint8_t token[16]{0};
};
static_assert(sizeof(FrameHeader) == 32);

struct alignas(8) GrantInstall {
  uint64_t seq{0};
  uint8_t token[16]{0};
  uint32_t kind{0};
  uint32_t flags{0};
};
static_assert(sizeof(GrantInstall) == 32);

struct alignas(8) GrantAck {
  uint64_t seq{0};
  uint8_t token[16]{0};
  int32_t status{0};
  uint32_t flags{0};
};
static_assert(sizeof(GrantAck) == 32);

struct alignas(128) ShmARegionHeader {
  alignas(128) std::atomic<uint64_t> head_a{0};
  uint8_t pad0[128 - sizeof(std::atomic<uint64_t>)]{0};
  uint64_t capacity{kDefaultRingBytes};
  uint32_t max_frame_bytes{kDefaultFrameBytes};
  uint32_t reserved{0};
  uint8_t pad1[128 - 16]{0};
};
static_assert(sizeof(ShmARegionHeader) == 256);

struct alignas(128) ShmBRegionHeader {
  alignas(128) std::atomic<uint64_t> tail_a{0};
  uint8_t pad0[128 - sizeof(std::atomic<uint64_t>)]{0};
  alignas(128) std::atomic<uint64_t> head_b{0};
  uint8_t pad1[128 - sizeof(std::atomic<uint64_t>)]{0};
  alignas(128) std::atomic<uint64_t> tail_b_published{0};
  uint8_t pad2[128 - sizeof(std::atomic<uint64_t>)]{0};
  uint64_t capacity{kDefaultRingBytes};
  uint32_t max_frame_bytes{kDefaultFrameBytes};
  uint32_t reserved{0};
  uint8_t pad3[128 - 16]{0};
};
static_assert(sizeof(ShmBRegionHeader) == 512);

inline constexpr size_t kShmAHeaderSize = sizeof(ShmARegionHeader);
inline constexpr size_t kShmBHeaderSize = sizeof(ShmBRegionHeader);

struct DecodedFrame {
  FrameHeader header;
  std::vector<uint8_t> payload;
};

class RingProducer {
public:
  RingProducer(std::atomic<uint64_t> *head, const std::atomic<uint64_t> *tail,
               uint8_t *data, uint64_t capacity, uint32_t max_frame_bytes,
               int doorbell_fd = -1);

  [[nodiscard]] std::expected<void, std::string>
  writeFrame(FrameType type, uint64_t seq, std::span<const uint8_t> token,
             std::span<const uint8_t> payload);

  RingProducer *setDoorbell(int fd) noexcept {
    doorbell_fd_ = fd;
    return this;
  }

private:
  std::atomic<uint64_t> *head_{nullptr};
  const std::atomic<uint64_t> *tail_{nullptr};
  uint8_t *data_{nullptr};
  uint64_t capacity_{0};
  uint32_t max_frame_bytes_{0};
  int doorbell_fd_{-1};
};

class RingConsumer {
public:
  RingConsumer(const std::atomic<uint64_t> *head, std::atomic<uint64_t> *tail,
               const uint8_t *data, uint64_t capacity, uint32_t max_frame_bytes,
               int doorbell_fd = -1);

  [[nodiscard]] std::expected<std::optional<DecodedFrame>, std::string>
  pollFrame();

  [[nodiscard]] std::expected<DecodedFrame, std::string>
  waitFrame(int timeout_ms = -1);

  RingConsumer *setDoorbell(int fd) noexcept {
    doorbell_fd_ = fd;
    return this;
  }

private:
  const std::atomic<uint64_t> *head_{nullptr};
  std::atomic<uint64_t> *tail_{nullptr};
  const uint8_t *data_{nullptr};
  uint64_t capacity_{0};
  uint32_t max_frame_bytes_{0};
  int doorbell_fd_{-1};
  std::vector<uint8_t> scratch_buffer_;
};

// Doorbell helpers
[[nodiscard]] std::expected<void, std::string> ringDoorbell(int fd);
[[nodiscard]] std::expected<bool, std::string> waitDoorbell(int fd,
                                                            int timeout_ms);

// Descriptor passing helpers over control socket (fd 3)
[[nodiscard]] std::expected<void, std::string>
sendGrantInstall(int sock_fd, const GrantInstall &install, int fd_to_pass);

[[nodiscard]] std::expected<void, std::string>
recvGrantInstall(int sock_fd, GrantInstall &install, int &received_fd);

[[nodiscard]] std::expected<void, std::string>
sendGrantAck(int sock_fd, const GrantAck &ack);

[[nodiscard]] std::expected<void, std::string> recvGrantAck(int sock_fd,
                                                            GrantAck &ack);

} // namespace xanadu::vagent::native::ipc

#endif // VAGENT_NATIVE_HOST_IPC_HPP
