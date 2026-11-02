#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common/xanadu/vagent/native/supervisor.hpp"
#include "common/xanadu/vagent/cluster/package_format.hpp"
#include "common/xanadu/vagent/native/ipc.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace xanadu::vagent::native {

namespace fs = std::filesystem;

struct NativeHostSupervisor::Impl {
  int log_pipe_read{-1};
  int control_sock{-1};
  int memfd_a{-1};
  int memfd_b{-1};
  int doorbell_c2h{-1};
  int doorbell_h2c{-1};

  void *map_a{nullptr};
  void *map_b{nullptr};
  size_t total_a{0};
  size_t total_b{0};

  std::unique_ptr<ipc::RingProducer> producer_a;
  std::unique_ptr<ipc::RingConsumer> consumer_b;

  ~Impl() { cleanup(); }

  void cleanup() {
    producer_a.reset();
    consumer_b.reset();

    if (map_a && map_a != MAP_FAILED) {
      ::munmap(map_a, total_a);
      map_a = nullptr;
    }
    if (map_b && map_b != MAP_FAILED) {
      ::munmap(map_b, total_b);
      map_b = nullptr;
    }
    if (log_pipe_read >= 0) {
      ::close(log_pipe_read);
      log_pipe_read = -1;
    }
    if (control_sock >= 0) {
      ::close(control_sock);
      control_sock = -1;
    }
    if (memfd_a >= 0) {
      ::close(memfd_a);
      memfd_a = -1;
    }
    if (memfd_b >= 0) {
      ::close(memfd_b);
      memfd_b = -1;
    }
    if (doorbell_c2h >= 0) {
      ::close(doorbell_c2h);
      doorbell_c2h = -1;
    }
    if (doorbell_h2c >= 0) {
      ::close(doorbell_h2c);
      doorbell_h2c = -1;
    }
  }
};

NativeHostSupervisor::NativeHostSupervisor(SupervisorConfig config)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {
  if (config_.staging_root.empty()) {
    if (const char *xdg = std::getenv("XDG_RUNTIME_DIR")) {
      config_.staging_root = std::string(xdg) + "/vagent-native";
    } else {
      config_.staging_root = "/tmp/vagent-native";
    }
  }
}

NativeHostSupervisor::~NativeHostSupervisor() { terminate(); }

NativeHostSupervisor::NativeHostSupervisor(NativeHostSupervisor &&) noexcept =
    default;
NativeHostSupervisor &
NativeHostSupervisor::operator=(NativeHostSupervisor &&) noexcept = default;

bool NativeHostSupervisor::isRunning() const {
  if (child_pid_ <= 0) return false;
  int status = 0;
  pid_t res  = ::waitpid(child_pid_, &status, WNOHANG);
  return res == 0;
}

std::expected<std::string, std::string> NativeHostSupervisor::stageModule(
    std::string_view module_bytes, std::string_view module_filename,
    const std::vector<std::pair<std::string, std::string>> &bundled_libs) {
  if (module_bytes.empty()) {
    return std::unexpected("Cannot stage empty module bytes");
  }

  module_filename_ =
      module_filename.empty() ? "module.so" : std::string(module_filename);
  bundled_sonames_.clear();

  // Create staging directory structure: <staging_root>/<epoch>-<pid>-<seq>/
  const auto now = std::chrono::system_clock::now().time_since_epoch().count();
  const std::string dir_name =
      std::format("stage-{}-{}-{}", now, ::getpid(), seq_counter_++);
  fs::path stage_path = fs::path(config_.staging_root) / dir_name;

  std::error_code ec;
  fs::create_directories(stage_path, ec);
  if (ec) {
    return std::unexpected("Failed to create staging directory: " +
                           ec.message());
  }

  // Set staging directory permissions: mode 0700 (owner rwx)
  fs::permissions(stage_path, fs::perms::owner_all, fs::perm_options::replace,
                  ec);
  if (ec) {
    return std::unexpected("Failed to set staging directory permissions: " +
                           ec.message());
  }

  staging_dir_ = stage_path.string();

  // Validate module filename against soname regex and directory traversal
  std::string soname_err;
  if (!cluster::validateSoname(module_filename_, soname_err)) {
    return std::unexpected("Invalid module filename: " + soname_err);
  }

  // Write module.so: mode 0500 (owner read & execute, no write)
  fs::path mod_path = (stage_path / module_filename_).lexically_normal();
  if (mod_path.parent_path() != stage_path.lexically_normal()) {
    return std::unexpected("Path traversal attempt in module filename: " +
                           module_filename_);
  }
  {
    std::ofstream out(mod_path, std::ios::binary);
    if (!out) {
      return std::unexpected("Failed to create staged module file: " +
                             mod_path.string());
    }
    out.write(module_bytes.data(),
              static_cast<std::streamsize>(module_bytes.size()));
  }

  fs::permissions(mod_path, fs::perms::owner_read | fs::perms::owner_exec,
                  fs::perm_options::replace, ec);
  if (ec) {
    return std::unexpected("Failed to set staged module permissions: " +
                           ec.message());
  }

  // Stage bundled libraries if provided into <staging_dir>/lib/
  if (!bundled_libs.empty()) {
    fs::path lib_dir = stage_path / "lib";
    fs::create_directories(lib_dir, ec);
    fs::permissions(lib_dir, fs::perms::owner_all, fs::perm_options::replace,
                    ec);

    for (const auto &[soname, lib_bytes] : bundled_libs) {
      if (!cluster::validateSoname(soname, soname_err)) {
        return std::unexpected("Invalid bundled library soname: " + soname_err);
      }
      fs::path lib_file = (lib_dir / soname).lexically_normal();
      if (lib_file.parent_path() != lib_dir.lexically_normal()) {
        return std::unexpected(
            "Path traversal attempt in bundled library soname: " + soname);
      }
      {
        std::ofstream lib_out(lib_file, std::ios::binary);
        if (!lib_out) {
          return std::unexpected("Failed to create bundled lib file: " +
                                 lib_file.string());
        }
        lib_out.write(lib_bytes.data(),
                      static_cast<std::streamsize>(lib_bytes.size()));
      }
      fs::permissions(lib_file, fs::perms::owner_read | fs::perms::owner_exec,
                      fs::perm_options::replace, ec);
      bundled_sonames_.push_back(soname);
    }
  }

  return staging_dir_;
}

std::expected<void, std::string>
NativeHostSupervisor::spawn(std::chrono::milliseconds timeout) {
  if (staging_dir_.empty()) {
    return std::unexpected("No module staged before spawn");
  }
  if (isRunning()) {
    return std::unexpected("Native host is already running");
  }

  std::string host_path = config_.host_executable_path;
  if (host_path.empty()) {
    if (fs::exists("build/vagent-native-host")) {
      host_path = "build/vagent-native-host";
    } else if (fs::exists("./vagent-native-host")) {
      host_path = "./vagent-native-host";
    } else {
      host_path = "build/vagent-native-host"; // Will error on exec if missing
    }
  }

  // Create IPC resources
  int log_pipe[2]{-1, -1};
  if (::pipe2(log_pipe, O_CLOEXEC) != 0) {
    return std::unexpected("Failed to create log pipe: " +
                           std::string(std::strerror(errno)));
  }
  ::fcntl(log_pipe[0], F_SETFL, O_NONBLOCK);
  impl_->log_pipe_read = log_pipe[0];

  int ctrl_socks[2]{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ctrl_socks) !=
      0) {
    ::close(log_pipe[1]);
    return std::unexpected("Failed to create control socketpair: " +
                           std::string(std::strerror(errno)));
  }
  impl_->control_sock = ctrl_socks[0];

  int dev_null_r = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  int dev_null_w = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (dev_null_r < 0 || dev_null_w < 0) {
    if (dev_null_r >= 0) ::close(dev_null_r);
    if (dev_null_w >= 0) ::close(dev_null_w);
    ::close(log_pipe[1]);
    ::close(ctrl_socks[1]);
    return std::unexpected("Failed to open /dev/null: " +
                           std::string(std::strerror(errno)));
  }

  impl_->total_a = ipc::kShmAHeaderSize + config_.ring_bytes;
  impl_->total_b = ipc::kShmBHeaderSize + config_.ring_bytes;

  impl_->memfd_a =
      ::memfd_create("vagent_shm_a", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (impl_->memfd_a < 0) {
    ::close(dev_null_r);
    ::close(dev_null_w);
    ::close(log_pipe[1]);
    ::close(ctrl_socks[1]);
    return std::unexpected("memfd_create shm_a failed: " +
                           std::string(std::strerror(errno)));
  }
  (void)::ftruncate(impl_->memfd_a, static_cast<off_t>(impl_->total_a));
  (void)::fcntl(impl_->memfd_a, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);

  impl_->map_a = ::mmap(nullptr, impl_->total_a, PROT_READ | PROT_WRITE,
                        MAP_SHARED, impl_->memfd_a, 0);
  if (impl_->map_a == MAP_FAILED) {
    ::close(dev_null_r);
    ::close(dev_null_w);
    ::close(log_pipe[1]);
    ::close(ctrl_socks[1]);
    return std::unexpected("mmap shm_a failed: " +
                           std::string(std::strerror(errno)));
  }

  // Open RO fd via /proc/self/fd for the child
  char proc_ro_path[64];
  std::snprintf(proc_ro_path, sizeof(proc_ro_path), "/proc/self/fd/%d",
                impl_->memfd_a);
  int shm_a_ro = ::open(proc_ro_path, O_RDONLY | O_CLOEXEC);
  if (shm_a_ro < 0) {
    ::close(dev_null_r);
    ::close(dev_null_w);
    ::close(log_pipe[1]);
    ::close(ctrl_socks[1]);
    return std::unexpected("open RO shm_a failed: " +
                           std::string(std::strerror(errno)));
  }

  impl_->memfd_b =
      ::memfd_create("vagent_shm_b", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (impl_->memfd_b < 0) {
    ::close(dev_null_r);
    ::close(dev_null_w);
    ::close(log_pipe[1]);
    ::close(ctrl_socks[1]);
    ::close(shm_a_ro);
    return std::unexpected("memfd_create shm_b failed: " +
                           std::string(std::strerror(errno)));
  }
  (void)::ftruncate(impl_->memfd_b, static_cast<off_t>(impl_->total_b));
  (void)::fcntl(impl_->memfd_b, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);

  impl_->map_b = ::mmap(nullptr, impl_->total_b, PROT_READ | PROT_WRITE,
                        MAP_SHARED, impl_->memfd_b, 0);
  if (impl_->map_b == MAP_FAILED) {
    ::close(dev_null_r);
    ::close(dev_null_w);
    ::close(log_pipe[1]);
    ::close(ctrl_socks[1]);
    ::close(shm_a_ro);
    return std::unexpected("mmap shm_b failed: " +
                           std::string(std::strerror(errno)));
  }

  impl_->doorbell_c2h = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  impl_->doorbell_h2c = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (impl_->doorbell_c2h < 0 || impl_->doorbell_h2c < 0) {
    ::close(dev_null_r);
    ::close(dev_null_w);
    ::close(log_pipe[1]);
    ::close(ctrl_socks[1]);
    ::close(shm_a_ro);
    return std::unexpected("eventfd creation failed: " +
                           std::string(std::strerror(errno)));
  }

  // Initialize region headers
  auto *hdr_a = static_cast<ipc::ShmARegionHeader *>(impl_->map_a);
  hdr_a->head_a.store(0, std::memory_order_relaxed);
  hdr_a->capacity        = config_.ring_bytes;
  hdr_a->max_frame_bytes = config_.max_frame_bytes;

  auto *hdr_b = static_cast<ipc::ShmBRegionHeader *>(impl_->map_b);
  hdr_b->tail_a.store(0, std::memory_order_relaxed);
  hdr_b->head_b.store(0, std::memory_order_relaxed);
  hdr_b->tail_b_published.store(0, std::memory_order_relaxed);
  hdr_b->capacity        = config_.ring_bytes;
  hdr_b->max_frame_bytes = config_.max_frame_bytes;

  uint8_t *data_a = static_cast<uint8_t *>(impl_->map_a) + ipc::kShmAHeaderSize;
  const uint8_t *data_b =
      static_cast<const uint8_t *>(impl_->map_b) + ipc::kShmBHeaderSize;

  // Supervisor producer writes Ring A, reads tail_a in Ring B
  impl_->producer_a = std::make_unique<ipc::RingProducer>(
      &hdr_a->head_a, &hdr_b->tail_a, data_a, config_.ring_bytes,
      config_.max_frame_bytes, impl_->doorbell_c2h);

  // Supervisor consumer reads Ring B, updates tail_b_published in Ring B, reads
  // head_b in Ring B
  impl_->consumer_b = std::make_unique<ipc::RingConsumer>(
      &hdr_b->head_b, &hdr_b->tail_b_published, data_b, config_.ring_bytes,
      config_.max_frame_bytes, impl_->doorbell_h2c);

  // Fork and execve
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(dev_null_r);
    ::close(dev_null_w);
    ::close(log_pipe[1]);
    ::close(ctrl_socks[1]);
    ::close(shm_a_ro);
    return std::unexpected("fork failed: " + std::string(std::strerror(errno)));
  }

  if (pid == 0) {
    // Child process: wire descriptors 0..7
    ::dup2(dev_null_r, 0);
    ::dup2(dev_null_w, 1);
    ::dup2(log_pipe[1], 2);
    ::dup2(ctrl_socks[1], 3);
    ::dup2(shm_a_ro, 4);
    ::dup2(impl_->memfd_b, 5);
    ::dup2(impl_->doorbell_c2h, 6);
    ::dup2(impl_->doorbell_h2c, 7);

    // Close all descriptors 8..~0
    ::close_range(8, ~0U, 0);

    // Set child environment
    ::setenv("VAGENT_SUPERVISOR_PID", std::to_string(::getppid()).c_str(), 1);
    ::setenv("VAGENT_STAGING_DIR", staging_dir_.c_str(), 1);
    ::setenv("VAGENT_MODULE", module_filename_.c_str(), 1);
    if (!bundled_sonames_.empty()) {
      std::string joined;
      for (size_t i = 0; i < bundled_sonames_.size(); ++i) {
        if (i > 0) joined += ",";
        joined += bundled_sonames_[i];
      }
      ::setenv("VAGENT_BUNDLED_LIBS", joined.c_str(), 1);
    }

    ::execl(host_path.c_str(), host_path.c_str(), nullptr);
    ::_exit(127);
  }

  // Parent process
  child_pid_ = pid;
  ::close(dev_null_r);
  ::close(dev_null_w);
  ::close(log_pipe[1]);
  ::close(ctrl_socks[1]);
  ::close(shm_a_ro);

  // Wait for Ready frame on Ring B
  auto ready_res =
      impl_->consumer_b->waitFrame(static_cast<int>(timeout.count()));
  if (!ready_res) {
    terminate();
    return std::unexpected("Timed out or failed waiting for Ready frame: " +
                           ready_res.error());
  }

  if (ready_res->header.type != ipc::FrameType::Ready) {
    terminate();
    return std::unexpected(
        "Unexpected initial frame from guest: type=" +
        std::to_string(static_cast<uint16_t>(ready_res->header.type)));
  }

  ready_payload_ =
      std::string(reinterpret_cast<const char *>(ready_res->payload.data()),
                  ready_res->payload.size());

  return {};
}

std::expected<std::vector<uint8_t>, std::string>
NativeHostSupervisor::dispatchTurn(std::span<const uint8_t> in_turn,
                                   std::chrono::milliseconds timeout) {
  if (!isRunning() || !impl_->producer_a || !impl_->consumer_b) {
    return std::unexpected("Native host is not running");
  }

  const uint64_t seq = seq_counter_++;
  auto write_res = impl_->producer_a->writeFrame(ipc::FrameType::StartTurn, seq,
                                                 {}, in_turn);
  if (!write_res) {
    return std::unexpected("Failed to enqueue StartTurn into Ring A: " +
                           write_res.error());
  }

  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (elapsed >= timeout) {
      return std::unexpected("Turn execution timed out");
    }

    int child_status = 0;
    if (child_pid_ > 0 && ::waitpid(child_pid_, &child_status, WNOHANG) != 0) {
      return std::unexpected(
          "Native host process exited unexpectedly during turn execution");
    }

    auto frame_res = impl_->consumer_b->waitFrame(
        static_cast<int>((timeout - elapsed).count()));
    if (!frame_res) {
      if (!isRunning() ||
          (child_pid_ > 0 &&
           ::waitpid(child_pid_, &child_status, WNOHANG) != 0)) {
        return std::unexpected(
            "Native host terminated while waiting for turn result");
      }
      return std::unexpected("Error waiting for turn result: " +
                             frame_res.error());
    }

    if (frame_res->header.seq == seq) {
      if (frame_res->header.type == ipc::FrameType::TurnDone) {
        return frame_res->payload;
      }
      if (frame_res->header.type == ipc::FrameType::TurnFailed) {
        std::string err(
            reinterpret_cast<const char *>(frame_res->payload.data()),
            frame_res->payload.size());
        return std::unexpected("Turn execution failed: " + err);
      }
    }
  }
}

std::expected<void, std::string> NativeHostSupervisor::installGrant(
    uint64_t seq, std::span<const uint8_t, 16> token, uint32_t kind, int fd) {
  if (!isRunning() || impl_->control_sock < 0) {
    return std::unexpected("Native host is not running");
  }

  ipc::GrantInstall install{};
  install.seq = seq;
  std::memcpy(install.token, token.data(), 16);
  install.kind = kind;

  if (auto res = ipc::sendGrantInstall(impl_->control_sock, install, fd);
      !res) {
    return std::unexpected("sendGrantInstall failed: " + res.error());
  }

  ipc::GrantAck ack{};
  if (auto res = ipc::recvGrantAck(impl_->control_sock, ack); !res) {
    return std::unexpected("recvGrantAck failed: " + res.error());
  }

  if (ack.seq != seq || ack.status != 0) {
    return std::unexpected("Grant installation rejected by guest");
  }

  return {};
}

void NativeHostSupervisor::terminate() {
  if (child_pid_ > 0) {
    if (impl_->producer_a) {
      (void)impl_->producer_a->writeFrame(ipc::FrameType::Shutdown, 0, {}, {});
    }

    // Wait briefly for graceful exit
    int status  = 0;
    bool exited = false;
    for (int i = 0; i < 10; ++i) {
      if (::waitpid(child_pid_, &status, WNOHANG) == child_pid_) {
        exited = true;
        break;
      }
      ::usleep(10000); // 10ms
    }

    if (!exited) {
      ::kill(child_pid_, SIGTERM);
      ::usleep(20000);
      if (::waitpid(child_pid_, &status, WNOHANG) != child_pid_) {
        ::kill(child_pid_, SIGKILL);
        (void)::waitpid(child_pid_, &status, 0);
      }
    }

    child_pid_ = -1;
  }

  impl_->cleanup();

  // Recursively clean up staging directory
  if (!staging_dir_.empty()) {
    std::error_code ec;
    fs::remove_all(staging_dir_, ec);
    staging_dir_.clear();
  }
}

std::string NativeHostSupervisor::drainLogs() {
  if (impl_->log_pipe_read < 0) return {};
  std::string logs;
  char buf[4096];
  while (true) {
    ssize_t r = ::read(impl_->log_pipe_read, buf, sizeof(buf));
    if (r <= 0) break;
    logs.append(buf, static_cast<size_t>(r));
  }
  return logs;
}

} // namespace xanadu::vagent::native
