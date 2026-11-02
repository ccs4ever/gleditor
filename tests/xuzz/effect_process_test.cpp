#include "common/xanadu/evidence/effect_adapter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using xanadu::evidence::EffectAuthority;
using xanadu::evidence::EffectKind;
using xanadu::evidence::EffectOutcome;
using xanadu::evidence::EffectRequest;

#if !defined(_WIN32)
std::string executable(std::string_view name) {
  for (const auto &path : {std::filesystem::path{"/usr/bin"} / name,
                           std::filesystem::path{"/bin"} / name}) {
    std::error_code error;
    const auto resolved = std::filesystem::canonical(path, error);
    if (!error && ::access(resolved.c_str(), X_OK) == 0)
      return resolved.string();
  }
  return {};
}

EffectRequest request(std::string target, std::vector<std::string> argv = {}) {
  EffectRequest value;
  value.kind   = EffectKind::Process;
  value.target = std::move(target);
  value.argv   = std::move(argv);
  value.cwd    = std::filesystem::current_path().string();
  return value;
}

EffectAuthority authorityFor(const EffectRequest &request) {
  return {.operation       = EffectKind::Process,
          .canonicalTarget = request.target,
          .revoked         = false,
          .targetFingerprint =
              xanadu::evidence::effectTargetFingerprint(request)};
}
#endif

TEST(ProcessEffectTest, PreservesStreamsAndDoesNotInvokeAnImplicitShell) {
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX process adapter test";
#else
  const auto printfPath = executable("printf");
  ASSERT_FALSE(printfPath.empty());
  auto requestValue =
      request(printfPath, {"%s", "$HOME; touch /tmp/effect-shell-probe"});
  const auto authority = authorityFor(requestValue);
  std::atomic_bool cancel{};
  const auto result =
      xanadu::evidence::executeProcess(requestValue, authority, cancel);
  EXPECT_EQ(result.outcome, EffectOutcome::Completed);
  EXPECT_EQ(result.exitCode, 0);
  EXPECT_EQ(result.stdoutBytes, "$HOME; touch /tmp/effect-shell-probe");
  EXPECT_TRUE(result.stderrBytes.empty());
  EXPECT_TRUE(result.mayHaveExecuted);
#endif
}

TEST(ProcessEffectTest, UsesOwnedWorkingDirectoryEnvironmentAndStdin) {
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX process adapter test";
#else
  const auto shellPath = executable("sh");
  const auto catPath   = executable("cat");
  ASSERT_FALSE(shellPath.empty());
  ASSERT_FALSE(catPath.empty());
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("gleditor-effect-cwd-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  ASSERT_TRUE(std::filesystem::create_directories(directory));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};

  auto requestValue =
      request(shellPath,
              {"-c", "printf '%s|%s|' \"$ONLY_THIS\" \"$PWD\"; exec \"$CAT\""});
  requestValue.cwd                      = directory.string();
  requestValue.environment["ONLY_THIS"] = "owned-environment";
  requestValue.environment["CAT"]       = catPath;
  requestValue.stdinBytes               = "input bytes";
  const auto authority                  = authorityFor(requestValue);
  std::atomic_bool cancel{};
  const auto result =
      xanadu::evidence::executeProcess(requestValue, authority, cancel);
  EXPECT_EQ(result.outcome, EffectOutcome::Completed);
  EXPECT_EQ(result.stdoutBytes,
            "owned-environment|" + directory.string() + "|input bytes");
  EXPECT_TRUE(result.stderrBytes.empty());
#endif
}

TEST(ProcessEffectTest, DrainsBothPipesAndReportsOutputTruncation) {
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX process adapter test";
#else
  const auto shellPath = executable("sh");
  ASSERT_FALSE(shellPath.empty());
  auto requestValue =
      request(shellPath, {"-c", "i=0; while [ $i -lt 30000 ]; do printf x; "
                                "printf y >&2; i=$((i+1)); done"});
  requestValue.limits.maxOutputBytes = 4096;
  const auto authority               = authorityFor(requestValue);
  std::atomic_bool cancel{};
  const auto result =
      xanadu::evidence::executeProcess(requestValue, authority, cancel);
  EXPECT_EQ(result.outcome, EffectOutcome::Partial);
  EXPECT_TRUE(result.mayHaveExecuted);
  EXPECT_LE(result.stdoutBytes.size() + result.stderrBytes.size(), 4096U);
  EXPECT_FALSE(result.stdoutBytes.empty());
  EXPECT_FALSE(result.stderrBytes.empty());
#endif
}

TEST(ProcessEffectTest, BoundsOutputDrainWhenDescendantEscapesProcessGroup) {
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX process adapter test";
#else
  const auto shellPath  = executable("sh");
  const auto setsidPath = executable("setsid");
  const auto sleepPath  = executable("sleep");
  if (shellPath.empty() || setsidPath.empty() || sleepPath.empty())
    GTEST_SKIP() << "Requires POSIX sh, setsid, and sleep executables";

  const auto pidFile =
      std::filesystem::temp_directory_path() /
      ("gleditor-effect-escaped-child-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    std::filesystem::path pidFile;
    ~Cleanup() {
      std::ifstream input(pidFile);
      pid_t child{};
      if (input >> child) static_cast<void>(::kill(child, SIGKILL));
      std::filesystem::remove(pidFile);
    }
  } cleanup{pidFile};

  auto requestValue = request(
      shellPath, {"-c", "\"$SETSID\" \"$SH\" -c 'echo $$ > \"$PID_FILE\"; "
                        "exec \"$SLEEP\" 20' & wait"});
  requestValue.environment["SETSID"]   = setsidPath;
  requestValue.environment["SH"]       = shellPath;
  requestValue.environment["SLEEP"]    = sleepPath;
  requestValue.environment["PID_FILE"] = pidFile.string();
  requestValue.limits.timeoutMs        = 50;
  const auto authority                 = authorityFor(requestValue);
  std::atomic_bool cancel{};
  const auto started = std::chrono::steady_clock::now();
  const auto result =
      xanadu::evidence::executeProcess(requestValue, authority, cancel);

  EXPECT_EQ(result.outcome, EffectOutcome::Unknown);
  EXPECT_TRUE(result.mayHaveExecuted);
  EXPECT_NE(result.detail.find("descendant"), std::string::npos);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds{2});
#endif
}

TEST(ProcessEffectTest, DeadlineIsObservedWhileBothStreamsStayBusy) {
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX process adapter test";
#else
  const auto shellPath = executable("sh");
  ASSERT_FALSE(shellPath.empty());
  auto requestValue =
      request(shellPath, {"-c", "while :; do printf x; printf y >&2; done"});
  requestValue.limits.timeoutMs      = 50;
  requestValue.limits.maxOutputBytes = 4096;
  const auto authority               = authorityFor(requestValue);
  std::atomic_bool cancel{};
  const auto started = std::chrono::steady_clock::now();
  const auto result =
      xanadu::evidence::executeProcess(requestValue, authority, cancel);

  EXPECT_EQ(result.outcome, EffectOutcome::Partial);
  EXPECT_TRUE(result.mayHaveExecuted);
  EXPECT_LE(result.stdoutBytes.size() + result.stderrBytes.size(), 4096U);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds{2});
#endif
}

TEST(ProcessEffectTest, DeadlineTerminatesAndReapsTheProcessGroup) {
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX process adapter test";
#else
  const auto shellPath = executable("sh");
  const auto sleepPath = executable("sleep");
  ASSERT_FALSE(shellPath.empty());
  ASSERT_FALSE(sleepPath.empty());
  auto requestValue = request(shellPath, {"-c", "exec \"$SLEEP\" 5"});
  requestValue.environment["SLEEP"] = sleepPath;
  requestValue.limits.timeoutMs     = 50;
  const auto authority              = authorityFor(requestValue);
  std::atomic_bool cancel{};
  const auto started = std::chrono::steady_clock::now();
  const auto result =
      xanadu::evidence::executeProcess(requestValue, authority, cancel);
  EXPECT_EQ(result.outcome, EffectOutcome::Partial);
  EXPECT_TRUE(result.mayHaveExecuted);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds{2});
#endif
}

TEST(ProcessEffectTest, CancellationTerminatesTheProcessGroup) {
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX process adapter test";
#else
  const auto shellPath = executable("sh");
  const auto sleepPath = executable("sleep");
  ASSERT_FALSE(shellPath.empty());
  ASSERT_FALSE(sleepPath.empty());
  auto requestValue = request(shellPath, {"-c", "exec \"$SLEEP\" 5"});
  requestValue.environment["SLEEP"] = sleepPath;
  const auto authority              = authorityFor(requestValue);
  std::atomic_bool cancel{};
  std::jthread canceler([&cancel] {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    cancel.store(true, std::memory_order_release);
  });
  const auto started = std::chrono::steady_clock::now();
  const auto result =
      xanadu::evidence::executeProcess(requestValue, authority, cancel);
  EXPECT_EQ(result.outcome, EffectOutcome::Cancelled);
  EXPECT_TRUE(result.mayHaveExecuted);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds{2});
#endif
}

TEST(ProcessEffectTest, RevokedAuthorityPreventsTheExternalAction) {
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX process adapter test";
#else
  const auto printfPath = executable("printf");
  ASSERT_FALSE(printfPath.empty());
  auto requestValue = request(printfPath, {"should-not-run"});
  auto authority    = authorityFor(requestValue);
  authority.revoked.store(true, std::memory_order_release);
  std::atomic_bool cancel{};
  const auto result =
      xanadu::evidence::executeProcess(requestValue, authority, cancel);
  EXPECT_EQ(result.outcome, EffectOutcome::Rejected);
  EXPECT_FALSE(result.mayHaveExecuted);
  EXPECT_TRUE(result.stdoutBytes.empty());
#endif
}

TEST(ProcessEffectTest, ChangedExecutableBytesInvalidatePinnedAuthority) {
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX process adapter test";
#else
  const auto printfPath = executable("printf");
  ASSERT_FALSE(printfPath.empty());
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("gleditor-effect-target-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  ASSERT_TRUE(std::filesystem::create_directories(directory));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  const auto copied = directory / "printf-copy";
  std::filesystem::copy_file(printfPath, copied);
  std::filesystem::permissions(copied,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write |
                                   std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::replace);
  auto requestValue =
      request(std::filesystem::canonical(copied).string(), {"should-not-run"});
  const auto authority = authorityFor(requestValue);
  {
    std::ofstream changed(copied, std::ios::binary | std::ios::app);
    ASSERT_TRUE(changed.good());
    changed << "changed executable bytes";
  }
  std::atomic_bool cancel{};
  const auto result =
      xanadu::evidence::executeProcess(requestValue, authority, cancel);
  EXPECT_EQ(result.outcome, EffectOutcome::Rejected);
  EXPECT_FALSE(result.mayHaveExecuted);
  EXPECT_TRUE(result.stdoutBytes.empty());
#endif
}

TEST(ProcessEffectTest, MissingPinnedIdentityIsRejected) {
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX process adapter test";
#else
  const auto printfPath = executable("printf");
  ASSERT_FALSE(printfPath.empty());
  auto requestValue = request(printfPath, {"should-not-run"});
  auto authority    = authorityFor(requestValue);
  authority.targetFingerprint.clear();
  std::atomic_bool cancel{};
  const auto result =
      xanadu::evidence::executeProcess(requestValue, authority, cancel);
  EXPECT_EQ(result.outcome, EffectOutcome::Rejected);
  EXPECT_FALSE(result.mayHaveExecuted);
#endif
}
} // namespace

#if defined(__linux__)
TEST(ProcessEffectTest, DoesNotInheritUnreviewedHostDescriptors) {
  const int descriptor = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(descriptor, 0);
  struct CloseDescriptor {
    int descriptor;
    ~CloseDescriptor() { ::close(descriptor); }
  } cleanup{descriptor};
  ASSERT_EQ(::fcntl(descriptor, F_GETFD) & FD_CLOEXEC, 0);
  auto effect = request(executable("readlink"),
                        {"/proc/self/fd/" + std::to_string(descriptor)});
  ASSERT_FALSE(effect.target.empty());
  auto authority = authorityFor(effect);
  std::atomic_bool cancelled{};
  auto result = xanadu::evidence::executeProcess(effect, authority, cancelled);
  EXPECT_TRUE(result.stdoutBytes.empty());
  EXPECT_EQ(result.exitCode, 1);
}
#endif
