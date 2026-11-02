#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "common/xanadu/vagent/native/ipc.hpp"
#include "common/xanadu/vagent/native/supervisor.hpp"
#include "vagent-native-host/loader.hpp"
#include "vagent-native-host/sandbox_linux.hpp"

namespace xanadu::vagent::native::test {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// Helper to compile test shared libraries
bool compileSharedLibrary(const std::string &cpp_code,
                          const std::string &output_so_path) {
  fs::create_directories(fs::path(output_so_path).parent_path());
  const std::string cpp_path = output_so_path + ".cpp";
  {
    std::ofstream out(cpp_path);
    if (!out) return false;
    out << cpp_code;
  }

  std::string cmd = "clang++ -shared -fPIC -O2 \"" + cpp_path + "\" -o \"" +
                    output_so_path + "\" 2>/dev/null";
  int rc          = std::system(cmd.c_str());
  (void)fs::remove(cpp_path);
  return rc == 0;
}

std::vector<uint8_t> readFileBytes(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

} // namespace

// ============================================================================
// Track 2 / P20.4-P20.7: IPC Framing and Ring Buffer Tests
// ============================================================================

TEST(VagentNativeHostIpc, FrameRoundTrip) {
  constexpr uint64_t ring_size = 1024 * 1024;
  std::vector<uint8_t> mem_a(ipc::kShmAHeaderSize + ring_size, 0);
  std::vector<uint8_t> mem_b(ipc::kShmBHeaderSize + ring_size, 0);

  auto *hdr_a = reinterpret_cast<ipc::ShmARegionHeader *>(mem_a.data());
  hdr_a->head_a.store(0);
  hdr_a->capacity        = ring_size;
  hdr_a->max_frame_bytes = 64 * 1024;

  auto *hdr_b = reinterpret_cast<ipc::ShmBRegionHeader *>(mem_b.data());
  hdr_b->tail_a.store(0);
  hdr_b->head_b.store(0);
  hdr_b->tail_b_published.store(0);
  hdr_b->capacity        = ring_size;
  hdr_b->max_frame_bytes = 64 * 1024;

  uint8_t *data_a = mem_a.data() + ipc::kShmAHeaderSize;

  ipc::RingProducer producer(&hdr_a->head_a, &hdr_b->tail_a, data_a, ring_size,
                             64 * 1024);
  ipc::RingConsumer consumer(&hdr_a->head_a, &hdr_b->tail_a, data_a, ring_size,
                             64 * 1024);

  // Send a StartTurn frame
  std::string payload = "test-payload-12345";
  std::span<const uint8_t> payload_span(
      reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
  uint8_t token[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

  auto write_res =
      producer.writeFrame(ipc::FrameType::StartTurn, 42, token, payload_span);
  ASSERT_TRUE(write_res.has_value()) << write_res.error();

  auto poll_res = consumer.pollFrame();
  ASSERT_TRUE(poll_res.has_value());
  ASSERT_TRUE(poll_res->has_value());

  auto &frame = **poll_res;
  EXPECT_EQ(frame.header.magic, ipc::kFrameMagic);
  EXPECT_EQ(frame.header.type, ipc::FrameType::StartTurn);
  EXPECT_EQ(frame.header.seq, 42u);
  EXPECT_EQ(std::memcmp(frame.header.token, token, 16), 0);
  EXPECT_EQ(frame.payload.size(), payload.size());
  std::string recvd_payload(
      reinterpret_cast<const char *>(frame.payload.data()),
      frame.payload.size());
  EXPECT_EQ(recvd_payload, payload);

  // Buffer should now be empty
  auto empty_poll = consumer.pollFrame();
  ASSERT_TRUE(empty_poll.has_value());
  EXPECT_FALSE(empty_poll->has_value());
}

TEST(VagentNativeHostIpc, WrapAroundMarker) {
  constexpr uint64_t ring_size =
      1024; // Small ring to trigger wrap-around quickly
  std::vector<uint8_t> mem(ipc::kShmAHeaderSize + ring_size, 0);

  auto *hdr = reinterpret_cast<ipc::ShmARegionHeader *>(mem.data());
  std::atomic<uint64_t> tail{0};
  hdr->head_a.store(0);
  hdr->capacity        = ring_size;
  hdr->max_frame_bytes = 256;

  uint8_t *data = mem.data() + ipc::kShmAHeaderSize;

  ipc::RingProducer producer(&hdr->head_a, &tail, data, ring_size, 256);
  ipc::RingConsumer consumer(&hdr->head_a, &tail, data, ring_size, 256);

  // Write several frames to advance head close to ring_size boundary
  std::string chunk(120, 'A');
  std::span<const uint8_t> chunk_span(
      reinterpret_cast<const uint8_t *>(chunk.data()), chunk.size());

  for (uint64_t seq = 1; seq <= 6; ++seq) {
    auto res =
        producer.writeFrame(ipc::FrameType::StartTurn, seq, {}, chunk_span);
    ASSERT_TRUE(res.has_value()) << res.error();

    // Consume immediately to keep tail advancing
    auto read_res = consumer.pollFrame();
    ASSERT_TRUE(read_res.has_value());
    ASSERT_TRUE(read_res->has_value());
    EXPECT_EQ((*read_res)->header.seq, seq);
  }

  // Now head is around offset 960 (close to 1024). Next 120-byte payload +
  // 32-byte header = 152 bytes which exceeds remaining bytes (64 bytes left).
  // Producer must write wrap marker and wrap to offset 0!
  auto wrap_write =
      producer.writeFrame(ipc::FrameType::StartTurn, 999, {}, chunk_span);
  ASSERT_TRUE(wrap_write.has_value()) << wrap_write.error();

  auto wrap_read = consumer.pollFrame();
  ASSERT_TRUE(wrap_read.has_value());
  ASSERT_TRUE(wrap_read->has_value());
  EXPECT_EQ((*wrap_read)->header.seq, 999u);
  EXPECT_EQ((*wrap_read)->payload.size(), chunk.size());
}

TEST(VagentNativeHostIpc, GrantDescriptorPassing) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds), 0);

  int test_evt_fd = ::eventfd(42, EFD_CLOEXEC);
  ASSERT_GE(test_evt_fd, 0);

  ipc::GrantInstall req{};
  req.seq           = 77;
  req.kind          = 1; // terminal
  uint8_t token[16] = {'T', 'O', 'K', 'E', 'N', '1', '2', '3',
                       '4', '5', '6', '7', '8', '9', '0', '!'};
  std::memcpy(req.token, token, 16);

  auto send_res = ipc::sendGrantInstall(fds[0], req, test_evt_fd);
  ASSERT_TRUE(send_res.has_value()) << send_res.error();

  ipc::GrantInstall recvd_req{};
  int recvd_fd  = -1;
  auto recv_res = ipc::recvGrantInstall(fds[1], recvd_req, recvd_fd);
  ASSERT_TRUE(recv_res.has_value()) << recv_res.error();

  EXPECT_EQ(recvd_req.seq, 77u);
  EXPECT_EQ(recvd_req.kind, 1u);
  EXPECT_EQ(std::memcmp(recvd_req.token, token, 16), 0);
  EXPECT_GE(recvd_fd, 0);

  // Verify received descriptor is functional
  uint64_t val = 0;
  EXPECT_EQ(::read(recvd_fd, &val, sizeof(val)),
            static_cast<ssize_t>(sizeof(val)));
  EXPECT_EQ(val, 42u);

  // Send Ack
  ipc::GrantAck ack{};
  ack.seq = 77;
  std::memcpy(ack.token, token, 16);
  ack.status    = 0;
  auto ack_send = ipc::sendGrantAck(fds[1], ack);
  ASSERT_TRUE(ack_send.has_value());

  ipc::GrantAck recvd_ack{};
  auto ack_recv = ipc::recvGrantAck(fds[0], recvd_ack);
  ASSERT_TRUE(ack_recv.has_value());
  EXPECT_EQ(recvd_ack.seq, 77u);
  EXPECT_EQ(recvd_ack.status, 0);

  ::close(recvd_fd);
  ::close(test_evt_fd);
  ::close(fds[0]);
  ::close(fds[1]);
}

TEST(VagentNativeHostIpc, DoorbellSignaling) {
  int ev_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ASSERT_GE(ev_fd, 0);

  // Should timeout when not signaled
  auto wait1 = ipc::waitDoorbell(ev_fd, 10);
  ASSERT_TRUE(wait1.has_value());
  EXPECT_FALSE(*wait1);

  // Ring doorbell
  auto ring_res = ipc::ringDoorbell(ev_fd);
  ASSERT_TRUE(ring_res.has_value());

  // Should immediately wake up
  auto wait2 = ipc::waitDoorbell(ev_fd, 100);
  ASSERT_TRUE(wait2.has_value());
  EXPECT_TRUE(*wait2);

  ::close(ev_fd);
}

// ============================================================================
// Track 2 / P20.4-P20.7: Sandbox Lockdown and W^X Tests
// ============================================================================

TEST(VagentNativeHostSandbox, Phase2LockdownWxorXEnforcement) {
  // Execute in a child process to isolate seccomp restrictions
  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);

  if (pid == 0) {
    // Step 5: PR_SET_NO_NEW_PRIVS
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
      ::_exit(1);
    }

    // Step 8: Phase 2 Lockdown
    auto lockdown_res = applyPhase2Lockdown();
    if (!lockdown_res) {
      ::_exit(2);
    }

    // Probe 1: Attempt to allocate executable memory via mmap
    void *exec_map = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (exec_map != MAP_FAILED) {
      ::munmap(exec_map, 4096);
      ::_exit(3); // FAIL: executable mmap was permitted!
    }
    if (errno != EPERM) {
      ::_exit(4); // FAIL: expected EPERM refusal from seccomp filter
    }

    // Probe 2: Attempt to transition existing memory to executable via mprotect
    void *rw_map = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rw_map == MAP_FAILED) {
      ::_exit(5);
    }
    if (::mprotect(rw_map, 4096, PROT_READ | PROT_EXEC) == 0) {
      ::munmap(rw_map, 4096);
      ::_exit(6); // FAIL: mprotect to executable was permitted!
    }
    if (errno != EPERM) {
      ::munmap(rw_map, 4096);
      ::_exit(7);
    }
    ::munmap(rw_map, 4096);

    // Probe 3: Attempt to create network socket
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
      ::close(s);
      ::_exit(8); // FAIL: socket creation was permitted!
    }
    if (errno != EPERM) {
      ::_exit(9);
    }

    // Probe 4: Attempt to fork
    pid_t child = ::fork();
    if (child >= 0) {
      ::_exit(10); // FAIL: fork was permitted!
    }
    if (errno != EPERM) {
      ::_exit(11);
    }

    ::_exit(0); // All lockdown checks succeeded!
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << "Child probe failed with code " << WEXITSTATUS(status);
}

// ============================================================================
// Track 2 / P20.4-P20.7: Loader and Supervisor Integration Tests
// ============================================================================

TEST(VagentNativeHostLoader, TopologicalDlopenAndGuestContext) {
  const std::string staging_dir =
      "/tmp/vagent-test-loader-" + std::to_string(::getpid());
  fs::create_directories(staging_dir + "/lib");

  // 1. Compile bundled companion library: libhelper.so
  const std::string helper_code = R"cpp(
    extern "C" int helper_compute(int x) {
      return x * 3;
    }
  )cpp";
  ASSERT_TRUE(
      compileSharedLibrary(helper_code, staging_dir + "/lib/libhelper.so"));

  // 2. Compile guest module.so using helper_compute
  const std::string module_code = R"cpp(
    #include <cstring>
    #include <cstdint>

    extern "C" int helper_compute(int x);

    extern "C" {

    struct VagentGuestContext {
      uint32_t abi_version;
      const char *staging_dir;
      const char *module_name;
      void *user_data;
    };

    int vagent_agent_manifest_v1(const VagentGuestContext *ctx, char *out_manifest, size_t max_len) {
      const char *manifest = "{\"skills\":[\"TopologicalSkill\"],\"version\":1}";
      size_t len = strlen(manifest);
      if (len >= max_len) return -1;
      memcpy(out_manifest, manifest, len + 1);
      return 0;
    }

    int vagent_agent_turn_v1(const VagentGuestContext *ctx,
                             const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t *out_len) {
      int val = 10;
      if (in_len >= 1) val = static_cast<int>(in[0]);
      int res = helper_compute(val);

      if (*out_len < sizeof(int)) return -1;
      memcpy(out, &res, sizeof(int));
      *out_len = sizeof(int);
      return 0;
    }

    }
  )cpp";
  ASSERT_TRUE(compileSharedLibrary(module_code, staging_dir + "/module.so"));

  LoaderConfig cfg;
  cfg.staging_dir     = staging_dir;
  cfg.module_filename = "module.so";
  cfg.bundled_sonames = {"libhelper.so"};

  auto load_res = loadNativeGuest(cfg);
  ASSERT_TRUE(load_res.has_value()) << load_res.error();

  auto &mod = *load_res;
  EXPECT_NE(mod.module_handle, nullptr);
  EXPECT_EQ(mod.bundled_handles.size(), 1u);
  EXPECT_NE(mod.manifest_fn, nullptr);
  EXPECT_NE(mod.turn_fn, nullptr);

  // Execute manifest
  char manifest_buf[256];
  EXPECT_EQ(
      mod.manifest_fn(&mod.guest_context, manifest_buf, sizeof(manifest_buf)),
      0);
  EXPECT_TRUE(std::string(manifest_buf).find("TopologicalSkill") !=
              std::string::npos);

  // Execute turn
  uint8_t in_byte = 7;
  uint8_t out_buf[16];
  size_t out_len = sizeof(out_buf);
  EXPECT_EQ(mod.turn_fn(&mod.guest_context, &in_byte, 1, out_buf, &out_len), 0);
  EXPECT_EQ(out_len, sizeof(int));
  int computed = 0;
  std::memcpy(&computed, out_buf, sizeof(int));
  EXPECT_EQ(computed, 21); // helper_compute(7) = 7 * 3 = 21!

  fs::remove_all(staging_dir);
}

TEST(VagentNativeHostSupervisor, EndToEndLifecycleAndTurnDispatch) {
  // Create a guest module that echoes input with prefix
  const std::string tmp_build =
      "/tmp/vagent-test-e2e-" + std::to_string(::getpid());
  fs::create_directories(tmp_build);

  const std::string module_code = R"cpp(
    #include <cstring>
    #include <cstdint>

    extern "C" {

    struct VagentGuestContext {
      uint32_t abi_version;
      const char *staging_dir;
      const char *module_name;
      void *user_data;
    };

    int vagent_agent_manifest_v1(const VagentGuestContext *ctx, char *out_manifest, size_t max_len) {
      const char *m = "{\"status\":\"ok\"}";
      size_t l = strlen(m);
      if (l >= max_len) return -1;
      memcpy(out_manifest, m, l + 1);
      return 0;
    }

    int vagent_agent_turn_v1(const VagentGuestContext *ctx,
                             const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t *out_len) {
      const char *prefix = "GUEST_REPLY:";
      size_t plen = strlen(prefix);
      if (plen + in_len > *out_len) return -1;
      memcpy(out, prefix, plen);
      memcpy(out + plen, in, in_len);
      *out_len = plen + in_len;
      return 0;
    }

    }
  )cpp";

  std::string module_path = tmp_build + "/guest.so";
  ASSERT_TRUE(compileSharedLibrary(module_code, module_path));
  std::vector<uint8_t> module_bytes = readFileBytes(module_path);
  ASSERT_FALSE(module_bytes.empty());

  SupervisorConfig sup_cfg;
  sup_cfg.staging_root         = tmp_build + "/staging_root";
  sup_cfg.host_executable_path = "build/vagent-native-host";

  NativeHostSupervisor supervisor(sup_cfg);

  // Step 1: Stage module into directory
  std::string module_bytes_str(
      reinterpret_cast<const char *>(module_bytes.data()), module_bytes.size());
  auto stage_res = supervisor.stageModule(module_bytes_str, "module.so");
  ASSERT_TRUE(stage_res.has_value()) << stage_res.error();
  std::string stage_dir = *stage_res;

  // Verify staging permissions: dir 0700, module.so 0500
  struct stat st_dir{}, st_file{};
  ASSERT_EQ(::stat(stage_dir.c_str(), &st_dir), 0);
  EXPECT_EQ(st_dir.st_mode & 0777, 0700);

  std::string mod_file = stage_dir + "/module.so";
  ASSERT_EQ(::stat(mod_file.c_str(), &st_file), 0);
  EXPECT_EQ(st_file.st_mode & 0777, 0500);

  // Step 2: Spawn host process & connect IPC
  auto spawn_res = supervisor.spawn(5000ms);
  ASSERT_TRUE(spawn_res.has_value()) << spawn_res.error();
  EXPECT_TRUE(supervisor.isRunning());
  EXPECT_GT(supervisor.getChildPid(), 0);

  // Check Ready payload
  EXPECT_TRUE(supervisor.getReadyPayload().find("ready:") != std::string::npos);

  // Step 3: Turn dispatch
  std::string turn_in = "Hello Sandboxed Host!";
  std::span<const uint8_t> in_span(
      reinterpret_cast<const uint8_t *>(turn_in.data()), turn_in.size());

  auto turn_res = supervisor.dispatchTurn(in_span, 3000ms);
  ASSERT_TRUE(turn_res.has_value()) << turn_res.error();

  std::string turn_out(reinterpret_cast<const char *>(turn_res->data()),
                       turn_res->size());
  EXPECT_EQ(turn_out, "GUEST_REPLY:Hello Sandboxed Host!");

  // Step 4: Install Grant descriptor
  int test_pipe[2];
  ASSERT_EQ(::pipe(test_pipe), 0);

  uint8_t grant_token[16] = {'T', 'E', 'R', 'M', 'I', 'N', 'A', 'L',
                             '_', 'G', 'R', 'A', 'N', 'T', '1', '!'};
  auto grant_res = supervisor.installGrant(101, grant_token, 1, test_pipe[0]);
  ASSERT_TRUE(grant_res.has_value()) << grant_res.error();

  ::close(test_pipe[0]);
  ::close(test_pipe[1]);

  // Step 5: Terminate and verify clean teardown and staging removal
  supervisor.terminate();
  EXPECT_FALSE(supervisor.isRunning());
  EXPECT_FALSE(fs::exists(stage_dir));

  fs::remove_all(tmp_build);
}

} // namespace xanadu::vagent::native::test
