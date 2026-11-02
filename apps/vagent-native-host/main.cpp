#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/xanadu/vagent/native/ipc.hpp"
#include "vagent-native-host/loader.hpp"
#include "vagent-native-host/sandbox_linux.hpp"

using namespace xanadu::vagent::native;

int main(int argc, char **argv) {
  // Parse command line arguments / environment
  std::string staging_dir;
  std::string module_name = "module.so";
  std::vector<std::string> bundled_libs;

  if (const char *env_stage = std::getenv("VAGENT_STAGING_DIR")) {
    staging_dir = env_stage;
  }
  if (const char *env_mod = std::getenv("VAGENT_MODULE")) {
    module_name = env_mod;
  }
  if (const char *env_libs = std::getenv("VAGENT_BUNDLED_LIBS")) {
    // Comma or colon separated
    std::string libs_str = env_libs;
    size_t pos           = 0;
    while (pos < libs_str.size()) {
      size_t next = libs_str.find_first_of(",:", pos);
      if (next == std::string::npos) next = libs_str.size();
      if (next > pos) {
        bundled_libs.push_back(libs_str.substr(pos, next - pos));
      }
      pos = next + 1;
    }
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--staging" && i + 1 < argc) {
      staging_dir = argv[++i];
    } else if (arg == "--module" && i + 1 < argc) {
      module_name = argv[++i];
    } else if (arg == "--bundle" && i + 1 < argc) {
      bundled_libs.push_back(argv[++i]);
    }
  }

  // Step 1: Parent-death binding
  if (const char *env_pid = std::getenv("VAGENT_SUPERVISOR_PID")) {
    pid_t sup_pid = static_cast<pid_t>(std::atoi(env_pid));
    if (auto res = bindParentDeath(sup_pid); !res) {
      std::fprintf(stderr, "vagent-native-host: bindParentDeath failed: %s\n",
                   res.error().c_str());
      return 1;
    }
  }

  // Step 2: Descriptor hygiene
  if (auto res = performDescriptorHygiene(); !res) {
    std::fprintf(stderr, "vagent-native-host: descriptor hygiene failed: %s\n",
                 res.error().c_str());
    return 2;
  }

  // Step 3: Resource limits
  if (auto res = setResourceLimits(); !res) {
    std::fprintf(stderr, "vagent-native-host: setResourceLimits failed: %s\n",
                 res.error().c_str());
    return 3;
  }

  // Step 4: Shared memory mapping (fd 4: shm_a, fd 5: shm_b)
  struct stat st_a{}, st_b{};
  if (::fstat(4, &st_a) != 0 || ::fstat(5, &st_b) != 0) {
    std::fprintf(stderr,
                 "vagent-native-host: fstat on shared memory fds failed\n");
    return 4;
  }

  void *map_a = ::mmap(nullptr, static_cast<size_t>(st_a.st_size), PROT_READ,
                       MAP_SHARED, 4, 0);
  if (map_a == MAP_FAILED) {
    std::fprintf(stderr, "vagent-native-host: mmap shm_a failed: %s\n",
                 std::strerror(errno));
    return 4;
  }

  void *map_b = ::mmap(nullptr, static_cast<size_t>(st_b.st_size),
                       PROT_READ | PROT_WRITE, MAP_SHARED, 5, 0);
  if (map_b == MAP_FAILED) {
    std::fprintf(stderr, "vagent-native-host: mmap shm_b failed: %s\n",
                 std::strerror(errno));
    return 4;
  }

  auto *hdr_a = static_cast<const ipc::ShmARegionHeader *>(map_a);
  auto *hdr_b = static_cast<ipc::ShmBRegionHeader *>(map_b);

  const uint8_t *data_a =
      static_cast<const uint8_t *>(map_a) + ipc::kShmAHeaderSize;
  uint8_t *data_b = static_cast<uint8_t *>(map_b) + ipc::kShmBHeaderSize;

  const uint64_t cap_a     = hdr_a->capacity;
  const uint64_t cap_b     = hdr_b->capacity;
  const uint32_t max_frame = hdr_a->max_frame_bytes;

  // Ring A Consumer (Host reads Ring A, updates tail_a in ShmB)
  ipc::RingConsumer consumer_a(&hdr_a->head_a, &hdr_b->tail_a, data_a, cap_a,
                               max_frame, 6);

  // Ring B Producer (Host writes Ring B, updates head_b in ShmB, checks
  // tail_b_published in ShmB)
  ipc::RingProducer producer_b(&hdr_b->head_b, &hdr_b->tail_b_published, data_b,
                               cap_b, max_frame, 7);

  // Step 5 & 6: Phase 1 Sandbox (prctl NO_NEW_PRIVS + Landlock)
  SandboxConfig sandbox_cfg;
  sandbox_cfg.staging_dir = staging_dir;
  if (auto res = applyPhase1Sandbox(sandbox_cfg); !res) {
    std::fprintf(stderr, "vagent-native-host: Phase 1 sandbox failed: %s\n",
                 res.error().c_str());
    return 6;
  }

  // Step 7: Module and library load (Topological dlopen)
  LoaderConfig loader_cfg;
  loader_cfg.staging_dir     = staging_dir;
  loader_cfg.module_filename = module_name;
  loader_cfg.bundled_sonames = bundled_libs;

  auto loaded_res = loadNativeGuest(loader_cfg);
  if (!loaded_res) {
    std::fprintf(stderr, "vagent-native-host: loadNativeGuest failed: %s\n",
                 loaded_res.error().c_str());
    return 7;
  }
  auto &loaded = *loaded_res;

  // Step 8: Phase 2 Lockdown (PR_SET_MDWE + seccomp revoking PROT_EXEC &
  // MREMAP_MAYMOVE)
  if (auto res = applyPhase2Lockdown(); !res) {
    std::fprintf(stderr, "vagent-native-host: Phase 2 lockdown failed: %s\n",
                 res.error().c_str());
    return 8;
  }

  // Step 9: Serve
  // Send Ready frame on Ring B
  std::string ready_payload = "ready:";
  ready_payload += loaded.module_info.sha256;
  for (const auto &lib : loaded.loaded_libraries) {
    ready_payload += ";";
    ready_payload += lib.soname;
    ready_payload += "=";
    ready_payload += lib.sha256;
  }

  std::span<const uint8_t> ready_span(
      reinterpret_cast<const uint8_t *>(ready_payload.data()),
      ready_payload.size());
  if (auto res =
          producer_b.writeFrame(ipc::FrameType::Ready, 0, {}, ready_span);
      !res) {
    std::fprintf(stderr, "vagent-native-host: failed to send Ready frame: %s\n",
                 res.error().c_str());
    return 9;
  }

  // Grant table: token string/bytes -> fd
  std::map<std::string, int> grants;

  // Scratch buffer for turn responses
  std::vector<uint8_t> turn_out_buf(64 * 1024);

  // Event loop
  struct pollfd pfds[2]{};
  pfds[0].fd     = 6; // doorbell control -> host
  pfds[0].events = POLLIN;
  pfds[1].fd     = 3; // control socket
  pfds[1].events = POLLIN;

  bool running = true;
  while (running) {
    // First, drain any pending frames from Ring A
    while (true) {
      auto frame_res = consumer_a.pollFrame();
      if (!frame_res) {
        std::fprintf(stderr, "vagent-native-host: pollFrame error: %s\n",
                     frame_res.error().c_str());
        running = false;
        break;
      }
      if (!frame_res->has_value()) {
        break; // No more frames
      }

      auto &frame = **frame_res;
      if (frame.header.type == ipc::FrameType::Shutdown) {
        running = false;
        break;
      }

      if (frame.header.type == ipc::FrameType::StartTurn) {
        if (loaded.turn_fn) {
          size_t out_len = turn_out_buf.size();
          int turn_rc    = loaded.turn_fn(
              &loaded.guest_context, frame.payload.data(), frame.payload.size(),
              turn_out_buf.data(), &out_len);
          if (turn_rc == 0) {
            std::span<const uint8_t> out_span(turn_out_buf.data(), out_len);
            (void)producer_b.writeFrame(ipc::FrameType::TurnDone,
                                        frame.header.seq, frame.header.token,
                                        out_span);
          } else {
            std::string err_msg =
                "Turn execution failed with code " + std::to_string(turn_rc);
            std::span<const uint8_t> err_span(
                reinterpret_cast<const uint8_t *>(err_msg.data()),
                err_msg.size());
            (void)producer_b.writeFrame(ipc::FrameType::TurnFailed,
                                        frame.header.seq, frame.header.token,
                                        err_span);
          }
        } else {
          std::string err = "No turn function exported";
          std::span<const uint8_t> err_span(
              reinterpret_cast<const uint8_t *>(err.data()), err.size());
          (void)producer_b.writeFrame(ipc::FrameType::TurnFailed,
                                      frame.header.seq, frame.header.token,
                                      err_span);
        }
      }
    }

    if (!running) break;

    // Poll for doorbell or control socket
    int rc = ::poll(pfds, 2, 500);
    if (rc < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "vagent-native-host: poll failed: %s\n",
                   std::strerror(errno));
      break;
    }

    if (pfds[0].revents & POLLIN) {
      uint64_t val = 0;
      (void)::read(6, &val, sizeof(val));
    }

    if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      ipc::GrantInstall install{};
      int received_fd = -1;
      auto res        = ipc::recvGrantInstall(3, install, received_fd);
      if (!res) {
        // EOF or error on control socket means supervisor has terminated
        break;
      }

      std::string token_str(reinterpret_cast<const char *>(install.token), 16);
      if (received_fd >= 0) {
        if (auto it = grants.find(token_str);
            it != grants.end() && it->second >= 0) {
          ::close(it->second);
        }
        grants[token_str] = received_fd;
      }

      ipc::GrantAck ack{};
      ack.seq = install.seq;
      std::memcpy(ack.token, install.token, 16);
      ack.status = 0;
      (void)ipc::sendGrantAck(3, ack);
    }
  }

  // Cleanup grants
  for (auto &[tok, fd] : grants) {
    if (fd >= 0) {
      ::close(fd);
    }
  }

  return 0;
}
