#include "common/xanadu/evidence/effect_adapter.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace xanadu::evidence {
namespace {

EffectResult rejected(std::string detail) {
  return {.outcome = EffectOutcome::Rejected, .detail = std::move(detail)};
}

#if !defined(_WIN32)
class FileDescriptor {
public:
  explicit FileDescriptor(int value = -1) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) static_cast<void>(::close(value_));
  }
  FileDescriptor(const FileDescriptor &)            = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (value_ >= 0) static_cast<void>(::close(value_));
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  [[nodiscard]] int get() const { return value_; }
  [[nodiscard]] int release() { return std::exchange(value_, -1); }
  void reset(int value = -1) {
    if (value_ >= 0) static_cast<void>(::close(value_));
    value_ = value;
  }

private:
  int value_;
};

bool makePipe(FileDescriptor &readEnd, FileDescriptor &writeEnd) {
  int descriptors[2]{};
  if (::pipe(descriptors) != 0) return false;
  readEnd.reset(descriptors[0]);
  writeEnd.reset(descriptors[1]);
  for (const int descriptor : descriptors) {
    const int descriptorFlags = ::fcntl(descriptor, F_GETFD);
    if (descriptorFlags < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0)
      return false;
  }
  return true;
}

std::string systemError(std::string_view action, int error) {
  return std::string{action} + ": " + std::strerror(error);
}

class SigpipeBlock {
public:
  SigpipeBlock() {
    sigemptyset(&set_);
    sigaddset(&set_, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &set_, &previous_) == 0) {
      active_ = true;
      sigset_t pending{};
      if (sigpending(&pending) == 0)
        wasPending_ = sigismember(&pending, SIGPIPE) == 1;
    }
  }
  ~SigpipeBlock() {
    if (!active_) return;
    if (!wasPending_) {
      timespec noWait{};
      while (sigtimedwait(&set_, nullptr, &noWait) == SIGPIPE) {
      }
    }
    static_cast<void>(pthread_sigmask(SIG_SETMASK, &previous_, nullptr));
  }

private:
  sigset_t set_{};
  sigset_t previous_{};
  bool active_{};
  bool wasPending_{};
};

void closeChildEnds(posix_spawn_file_actions_t &actions,
                    const std::array<int, 6> &descriptors, int keepIn,
                    int keepOut, int keepErr) {
  for (const int descriptor : descriptors) {
    if (descriptor >= 0 && descriptor != keepIn && descriptor != keepOut &&
        descriptor != keepErr)
      static_cast<void>(
          posix_spawn_file_actions_addclose(&actions, descriptor));
  }
}

void signalGroup(pid_t child, int signal) {
  if (::kill(-child, signal) != 0 && errno == ESRCH)
    static_cast<void>(::kill(child, signal));
}

void appendBounded(std::string &target, const char *bytes, std::size_t count,
                   std::uint64_t limit, std::uint64_t &total, bool &truncated) {
  const auto available = total < limit ? limit - total : 0;
  const auto kept      = static_cast<std::size_t>(
      std::min<std::uint64_t>(available, static_cast<std::uint64_t>(count)));
  target.append(bytes, kept);
  total += kept;
  if (kept != count) truncated = true;
}

bool setNonblocking(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFL);
  return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

#endif
} // namespace

EffectResult executeProcess(const EffectRequest &request,
                            const EffectAuthority &authority,
                            const std::atomic_bool &cancel) {
#if defined(_WIN32)
  static_cast<void>(request);
  static_cast<void>(authority);
  static_cast<void>(cancel);
  return {.outcome = EffectOutcome::Failed,
          .detail  = "Process effects are unsupported on this platform."};
#else
  if (authority.targetFingerprint.empty())
    return rejected("The executable authority has no pinned identity.");
  if (const auto problem = effectAuthorityProblem(request, authority, cancel)) {
    if (cancel.load(std::memory_order_acquire))
      return {.outcome = EffectOutcome::Cancelled, .detail = *problem};
    return rejected(*problem);
  }

  std::string resolved;
  try {
    resolved = std::filesystem::canonical(request.target).string();
  } catch (const std::filesystem::filesystem_error &error) {
    return rejected(std::string{"Cannot resolve executable: "} + error.what());
  }
  if (resolved != authority.canonicalTarget || resolved != request.target)
    return rejected(
        "The executable does not match the authorized canonical target.");
  if (::access(resolved.c_str(), X_OK) != 0)
    return rejected(systemError("Executable is unavailable", errno));

  if (!request.cwd.empty()) {
    std::error_code error;
    if (!std::filesystem::is_directory(request.cwd, error) || error)
      return rejected("The requested working directory is unavailable.");
  }

  FileDescriptor childInput, parentInput, parentOutput, childOutput,
      parentError, childError;
  if (!makePipe(childInput, parentInput) ||
      !makePipe(parentOutput, childOutput) ||
      !makePipe(parentError, childError))
    return {.outcome        = EffectOutcome::Failed,
            .resolvedTarget = resolved,
            .detail = systemError("Cannot create process pipes", errno)};

  posix_spawn_file_actions_t actions{};
  int error = posix_spawn_file_actions_init(&actions);
  if (error != 0)
    return {.outcome        = EffectOutcome::Failed,
            .resolvedTarget = resolved,
            .detail = systemError("Cannot initialize process actions", error)};
  const std::array<int, 6> descriptors{childInput.get(),   parentInput.get(),
                                       parentOutput.get(), childOutput.get(),
                                       parentError.get(),  childError.get()};
  error = posix_spawn_file_actions_adddup2(&actions, childInput.get(),
                                           STDIN_FILENO);
  if (error == 0)
    error = posix_spawn_file_actions_adddup2(&actions, childOutput.get(),
                                             STDOUT_FILENO);
  if (error == 0)
    error = posix_spawn_file_actions_adddup2(&actions, childError.get(),
                                             STDERR_FILENO);
  if (error == 0) {
    closeChildEnds(actions, descriptors, STDIN_FILENO, STDOUT_FILENO,
                   STDERR_FILENO);
  }
#if defined(__GLIBC__) || defined(__APPLE__)
  if (error == 0 && !request.cwd.empty())
    error = posix_spawn_file_actions_addchdir_np(&actions, request.cwd.c_str());
#else
  if (error == 0 && !request.cwd.empty()) error = ENOTSUP;
#endif
#if defined(__GLIBC__)
  // Only the three reviewed streams cross into the child; unrelated store,
  // socket and credential descriptors belong to the host.
  if (error == 0) error = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#elif !defined(__APPLE__)
  if (error == 0) error = ENOTSUP;
#endif
  if (error != 0) {
    static_cast<void>(posix_spawn_file_actions_destroy(&actions));
    return {.outcome        = EffectOutcome::Failed,
            .resolvedTarget = resolved,
            .detail = systemError("Cannot configure process actions", error)};
  }

  posix_spawnattr_t attributes{};
  error = posix_spawnattr_init(&attributes);
  if (error == 0) error = posix_spawnattr_setpgroup(&attributes, 0);
  short spawnFlags = POSIX_SPAWN_SETPGROUP;
#if defined(__APPLE__)
  spawnFlags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  if (error == 0) error = posix_spawnattr_setflags(&attributes, spawnFlags);
  if (error != 0) {
    static_cast<void>(posix_spawn_file_actions_destroy(&actions));
    if (error == 0) static_cast<void>(posix_spawnattr_destroy(&attributes));
    return {.outcome        = EffectOutcome::Failed,
            .resolvedTarget = resolved,
            .detail = systemError("Cannot configure process group", error)};
  }

  std::vector<std::string> argvStorage;
  argvStorage.reserve(request.argv.size() + 1);
  argvStorage.push_back(resolved);
  argvStorage.insert(argvStorage.end(), request.argv.begin(),
                     request.argv.end());
  std::vector<char *> argv;
  argv.reserve(argvStorage.size() + 1);
  for (auto &argument : argvStorage) argv.push_back(argument.data());
  argv.push_back(nullptr);

  std::vector<std::string> environmentStorage;
  environmentStorage.reserve(request.environment.size());
  for (const auto &[key, value] : request.environment)
    environmentStorage.push_back(key + "=" + value);
  std::vector<char *> environment;
  environment.reserve(environmentStorage.size() + 1);
  for (auto &entry : environmentStorage) environment.push_back(entry.data());
  environment.push_back(nullptr);

  // Recheck host authority at the last point before the external action.
  if (const auto problem = effectAuthorityProblem(request, authority, cancel)) {
    static_cast<void>(posix_spawnattr_destroy(&attributes));
    static_cast<void>(posix_spawn_file_actions_destroy(&actions));
    if (cancel.load(std::memory_order_acquire))
      return {.outcome        = EffectOutcome::Cancelled,
              .resolvedTarget = resolved,
              .detail         = *problem};
    return rejected(*problem);
  }

  SigpipeBlock blockSigpipe;
  pid_t child{};
  error = posix_spawn(&child, resolved.c_str(), &actions, &attributes,
                      argv.data(), environment.data());
  static_cast<void>(posix_spawnattr_destroy(&attributes));
  static_cast<void>(posix_spawn_file_actions_destroy(&actions));
  if (error != 0)
    return {.outcome        = EffectOutcome::Failed,
            .resolvedTarget = resolved,
            .detail         = systemError("Cannot start process", error)};

  childInput.reset();
  childOutput.reset();
  childError.reset();
  FileDescriptor input(parentInput.release());
  FileDescriptor output(parentOutput.release());
  FileDescriptor outputError(parentError.release());
  if (!setNonblocking(input.get()) || !setNonblocking(output.get()) ||
      !setNonblocking(outputError.get())) {
    signalGroup(child, SIGKILL);
    int status{};
    static_cast<void>(::waitpid(child, &status, 0));
    return {.outcome        = EffectOutcome::Failed,
            .resolvedTarget = resolved,
            .detail = systemError("Cannot configure process pipes", errno),
            .mayHaveExecuted = true};
  }

  EffectResult result{.outcome         = EffectOutcome::Completed,
                      .resolvedTarget  = resolved,
                      .mayHaveExecuted = true};
  std::uint64_t outputBytes{};
  bool truncated{};
  bool inputClosed{};
  bool outputClosed{};
  bool errorClosed{};
  bool exited{};
  bool interrupted{};
  bool killed{};
  bool uncertainDrain{};
  int waitStatus{};
  std::size_t inputOffset{};
  const auto started = std::chrono::steady_clock::now();
  const auto deadline =
      started + std::chrono::milliseconds(std::min<std::uint64_t>(
                    request.limits.timeoutMs,
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())));
  auto terminateAt            = std::chrono::steady_clock::time_point::max();
  auto hardStopAt             = std::chrono::steady_clock::time_point::max();
  const auto beginTermination = [&](const bool wasCancelled) {
    if (interrupted) return;
    interrupted = true;
    result.outcome =
        wasCancelled ? EffectOutcome::Cancelled : EffectOutcome::Partial;
    result.detail =
        wasCancelled ? "Process cancelled." : "Process deadline exceeded.";
    signalGroup(child, SIGTERM);
    terminateAt = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(request.limits.terminateGraceMs);
  };

  if (request.stdinBytes.empty()) {
    input.reset();
    inputClosed = true;
  }
  std::array<char, 8192> buffer{};
  while (!exited || !outputClosed || !errorClosed) {
    const auto now = std::chrono::steady_clock::now();
    if (!interrupted && cancel.load(std::memory_order_acquire))
      beginTermination(true);
    if (!interrupted && now >= deadline) beginTermination(false);
    if (interrupted && !killed && now >= terminateAt) {
      signalGroup(child, SIGKILL);
      killed = true;
      hardStopAt =
          now + std::chrono::milliseconds(request.limits.terminateGraceMs);
    }
    if (killed && now >= hardStopAt &&
        (!exited || !outputClosed || !errorClosed)) {
      result.outcome = EffectOutcome::Unknown;
      result.detail =
          "The process group did not close after termination; a descendant "
          "may still be running.";
      uncertainDrain = true;
      break;
    }

    if (!inputClosed && input.get() >= 0) {
      if (inputOffset == request.stdinBytes.size()) {
        input.reset();
        inputClosed = true;
      } else {
        const auto written =
            ::write(input.get(), request.stdinBytes.data() + inputOffset,
                    request.stdinBytes.size() - inputOffset);
        if (written > 0)
          inputOffset += static_cast<std::size_t>(written);
        else if (written < 0 && errno != EINTR && errno != EAGAIN &&
                 errno != EWOULDBLOCK) {
          input.reset();
          inputClosed = true;
        }
      }
    }

    for (auto [descriptor, destination, closed] :
         {std::tuple{output.get(), &result.stdoutBytes, &outputClosed},
          std::tuple{outputError.get(), &result.stderrBytes, &errorClosed}}) {
      if (*closed || descriptor < 0) continue;
      const auto bytesRead = ::read(descriptor, buffer.data(), buffer.size());
      if (bytesRead > 0) {
        appendBounded(*destination, buffer.data(),
                      static_cast<std::size_t>(bytesRead),
                      request.limits.maxOutputBytes, outputBytes, truncated);
      } else if (bytesRead == 0) {
        if (descriptor == output.get())
          output.reset();
        else
          outputError.reset();
        *closed = true;
      } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        if (descriptor == output.get())
          output.reset();
        else
          outputError.reset();
        *closed = true;
      }
    }

    if (!exited) {
      const auto waited = ::waitpid(child, &waitStatus, WNOHANG);
      if (waited == child)
        exited = true;
      else if (waited < 0 && errno != EINTR) {
        signalGroup(child, SIGKILL);
        static_cast<void>(::waitpid(child, &waitStatus, 0));
        return {.outcome         = EffectOutcome::Unknown,
                .resolvedTarget  = resolved,
                .detail          = systemError("Cannot reap process", errno),
                .mayHaveExecuted = true};
      }
    }

    if (exited && !outputClosed && !errorClosed) {
      // Continue draining both pipes after the leader exits: descendants may
      // still hold them, so cancellation/deadline remains active above.
    }
    if (!exited || !outputClosed || !errorClosed) {
      std::array<pollfd, 3> pollDescriptors{};
      nfds_t count{};
      if (!inputClosed && input.get() >= 0 &&
          inputOffset < request.stdinBytes.size())
        pollDescriptors[count++] = {input.get(), POLLOUT, 0};
      if (!outputClosed && output.get() >= 0)
        pollDescriptors[count++] = {output.get(), POLLIN | POLLHUP, 0};
      if (!errorClosed && outputError.get() >= 0)
        pollDescriptors[count++] = {outputError.get(), POLLIN | POLLHUP, 0};
      const auto pollResult = ::poll(pollDescriptors.data(), count, 10);
      if (pollResult < 0 && errno != EINTR) {
        beginTermination(false);
      }
    }
  }
  if (!exited) {
    const auto waited = ::waitpid(child, &waitStatus, WNOHANG);
    if (waited == child) {
      exited = true;
    } else if (waited == 0 || (waited < 0 && errno == EINTR)) {
      // Do not block the owner when an escaped descendant holds inherited
      // pipes open; reap the process group leader asynchronously instead.
      std::thread([child] {
        int status{};
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
      }).detach();
    }
  }
  if (uncertainDrain) return result;
  if (WIFEXITED(waitStatus)) result.exitCode = WEXITSTATUS(waitStatus);
  if (WIFSIGNALED(waitStatus)) result.signal = WTERMSIG(waitStatus);
  if (truncated && !interrupted) {
    result.outcome = EffectOutcome::Partial;
    result.detail  = "Process output exceeded the configured byte limit.";
  }
  return result;
#endif
}

} // namespace xanadu::evidence
