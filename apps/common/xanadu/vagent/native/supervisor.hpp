#ifndef COMMON_XANADU_VAGENT_NATIVE_SUPERVISOR_HPP
#define COMMON_XANADU_VAGENT_NATIVE_SUPERVISOR_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace xanadu::vagent::native {

struct SupervisorConfig {
  std::string staging_root;
  std::string host_executable_path;
  uint64_t ring_bytes{1024 * 1024};
  uint32_t max_frame_bytes{64 * 1024};
  uint64_t memory_limit_bytes{512 * 1024 * 1024};
  uint64_t max_fds{64};
};

class NativeHostSupervisor {
public:
  explicit NativeHostSupervisor(SupervisorConfig config = {});
  ~NativeHostSupervisor();

  NativeHostSupervisor(const NativeHostSupervisor &)            = delete;
  NativeHostSupervisor &operator=(const NativeHostSupervisor &) = delete;
  NativeHostSupervisor(NativeHostSupervisor &&) noexcept;
  NativeHostSupervisor &operator=(NativeHostSupervisor &&) noexcept;

  /// Stages a module and optional bundled libraries into a fresh staging
  /// directory with directory mode 0700 and binary mode 0500.
  [[nodiscard]] std::expected<std::string, std::string>
  stageModule(std::string_view module_bytes,
              std::string_view module_filename = "module.so",
              const std::vector<std::pair<std::string, std::string>>
                  &bundled_libs = {});

  /// Spawns the vagent-native-host process, passes fds 0..7, maps shared memory
  /// rings, and waits for the Ready frame on Ring B.
  [[nodiscard]] std::expected<void, std::string>
  spawn(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

  /// Dispatches a turn to the guest host process and awaits completion.
  [[nodiscard]] std::expected<std::vector<uint8_t>, std::string> dispatchTurn(
      std::span<const uint8_t> in_turn,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

  /// Installs an authority descriptor grant into the guest host over control
  /// socket fd 3.
  [[nodiscard]] std::expected<void, std::string>
  installGrant(uint64_t seq, std::span<const uint8_t, 16> token, uint32_t kind,
               int fd);

  /// Shuts down the guest process, reaps it, and cleans up the staging
  /// directory.
  void terminate();

  [[nodiscard]] bool isRunning() const;
  [[nodiscard]] pid_t getChildPid() const { return child_pid_; }
  [[nodiscard]] const std::string &getStagingDir() const {
    return staging_dir_;
  }
  [[nodiscard]] const std::string &getReadyPayload() const {
    return ready_payload_;
  }

  /// Drain log messages sent by the guest over fd 2 (non-blocking log pipe).
  [[nodiscard]] std::string drainLogs();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  SupervisorConfig config_;
  std::string staging_dir_;
  std::string module_filename_{"module.so"};
  std::vector<std::string> bundled_sonames_;
  pid_t child_pid_{-1};
  uint64_t seq_counter_{1};
  std::string ready_payload_;
};

} // namespace xanadu::vagent::native

#endif // COMMON_XANADU_VAGENT_NATIVE_SUPERVISOR_HPP
