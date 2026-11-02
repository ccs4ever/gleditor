#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "vagent-native-host/sandbox_linux.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <seccomp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>

#include <linux/landlock.h>

// Forward-compatibility definitions for prctl MDWE
#ifndef PR_SET_MDWE
#define PR_SET_MDWE 65
#define PR_MDWE_REFUSE_EXEC_GAIN (1UL << 0)
#endif

// Landlock syscall wrappers
#ifndef landlock_create_ruleset
static inline int
landlock_create_ruleset(const struct landlock_ruleset_attr *attr, size_t size,
                        __u32 flags) {
  return static_cast<int>(
      syscall(__NR_landlock_create_ruleset, attr, size, flags));
}
#endif

#ifndef landlock_add_rule
static inline int landlock_add_rule(int ruleset_fd,
                                    enum landlock_rule_type rule_type,
                                    const void *rule_attr, __u32 flags) {
  return static_cast<int>(
      syscall(__NR_landlock_add_rule, ruleset_fd, rule_type, rule_attr, flags));
}
#endif

#ifndef landlock_restrict_self
static inline int landlock_restrict_self(int ruleset_fd, __u32 flags) {
  return static_cast<int>(
      syscall(__NR_landlock_restrict_self, ruleset_fd, flags));
}
#endif

namespace xanadu::vagent::native {

std::expected<void, std::string> bindParentDeath(pid_t supervisor_pid) {
  if (supervisor_pid <= 0) {
    return std::unexpected("Invalid supervisor pid: " +
                           std::to_string(supervisor_pid));
  }

  if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
    return std::unexpected("Failed to set PR_SET_PDEATHSIG: " +
                           std::string(std::strerror(errno)));
  }

  // Catch parent death occurring before registration
  if (::getppid() != supervisor_pid) {
    return std::unexpected("Supervisor died before parent death binding");
  }

  return {};
}

std::expected<void, std::string> performDescriptorHygiene() {
  // Close all descriptors above fd 7
  ::close_range(8, ~0U, 0);

  // Verify descriptors 0..7 are valid and open
  for (int fd = 0; fd < 8; ++fd) {
    if (::fcntl(fd, F_GETFD) == -1) {
      return std::unexpected(
          "Required descriptor " + std::to_string(fd) +
          " is not open: " + std::string(std::strerror(errno)));
    }
  }

  return {};
}

std::expected<void, std::string> setResourceLimits(uint64_t memory_limit_bytes,
                                                   uint64_t max_fds) {
  struct rlimit rl{};

  // Memory limit (virtual address space)
  rl.rlim_cur = memory_limit_bytes;
  rl.rlim_max = memory_limit_bytes;
  if (::setrlimit(RLIMIT_AS, &rl) != 0) {
    return std::unexpected("Failed to set RLIMIT_AS: " +
                           std::string(std::strerror(errno)));
  }

  // Max open file descriptors
  rl.rlim_cur = max_fds;
  rl.rlim_max = max_fds;
  if (::setrlimit(RLIMIT_NOFILE, &rl) != 0) {
    return std::unexpected("Failed to set RLIMIT_NOFILE: " +
                           std::string(std::strerror(errno)));
  }

  // Core dumps disabled
  rl.rlim_cur = 0;
  rl.rlim_max = 0;
  if (::setrlimit(RLIMIT_CORE, &rl) != 0) {
    return std::unexpected("Failed to set RLIMIT_CORE: " +
                           std::string(std::strerror(errno)));
  }

  // File size creation limit 0
  rl.rlim_cur = 0;
  rl.rlim_max = 0;
  if (::setrlimit(RLIMIT_FSIZE, &rl) != 0) {
    return std::unexpected("Failed to set RLIMIT_FSIZE: " +
                           std::string(std::strerror(errno)));
  }

  return {};
}

static int addNetworkAndProcessDenialRules(scmp_filter_ctx ctx) {
  int rc = 0;
  // Network denial
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(socket), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(socketpair), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(connect), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(bind), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(listen), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(accept), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(accept4), 0);

  // Process spawning denial
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(execve), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(execveat), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(fork), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(vfork), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(clone3), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(clone), 1,
                         SCMP_CMP(0, SCMP_CMP_MASKED_EQ, CLONE_THREAD, 0));

  // Tracing, debugging and async kernel bypass denial
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ptrace), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(process_vm_readv),
                         0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM),
                         SCMP_SYS(process_vm_writev), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(io_uring_setup),
                         0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(io_uring_enter),
                         0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS),
                         SCMP_SYS(io_uring_register), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(perf_event_open),
                         0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(bpf), 0);
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(userfaultfd), 0);
  return rc;
}

std::expected<void, std::string>
applyPhase1Sandbox(const SandboxConfig &config) {
  // Step 5: Privilege lock and non-dumpable
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    return std::unexpected("Failed to set PR_SET_NO_NEW_PRIVS: " +
                           std::string(std::strerror(errno)));
  }

  if (::prctl(PR_SET_DUMPABLE, 0) != 0) {
    // Non-fatal, but good practice
  }

  // Optional unprivileged namespace unsharing
  (void)::unshare(CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWNS);

  // Step 6: Landlock sandbox configuration
  const int abi =
      landlock_create_ruleset(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 0) {
    // Landlock not supported on this kernel
    return std::unexpected("Landlock not supported by kernel: " +
                           std::string(std::strerror(errno)));
  }

  __u64 handled_fs =
      LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
      LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
      LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
      LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
      LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
      LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
      LANDLOCK_ACCESS_FS_MAKE_SYM;

  if (abi >= 2) {
    handled_fs |= LANDLOCK_ACCESS_FS_REFER;
  }
  if (abi >= 3) {
    handled_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
  }

  struct landlock_ruleset_attr attr{};
  attr.handled_access_fs = handled_fs;

  const int ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
  if (ruleset_fd < 0) {
    return std::unexpected("landlock_create_ruleset failed: " +
                           std::string(std::strerror(errno)));
  }

  auto add_path_rule = [&](const std::string &path, __u64 access_rights) {
    if (path.empty()) return;
    int fd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
    if (fd < 0) {
      return; // Skip non-existent paths
    }

    struct landlock_path_beneath_attr path_attr{};
    path_attr.allowed_access = access_rights;
    path_attr.parent_fd      = fd;

    (void)landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr,
                            0);
    ::close(fd);
  };

  const __u64 read_exec = LANDLOCK_ACCESS_FS_READ_FILE |
                          LANDLOCK_ACCESS_FS_READ_DIR |
                          LANDLOCK_ACCESS_FS_EXECUTE;
  const __u64 read_only =
      LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;

  // Staging directory (and lib/ inside it)
  add_path_rule(config.staging_dir, read_exec);
  add_path_rule(config.staging_dir + "/lib", read_exec);

  // Standard library search paths and ld.so.cache
  add_path_rule("/lib", read_exec);
  add_path_rule("/usr/lib", read_exec);
  add_path_rule("/lib64", read_exec);
  add_path_rule("/usr/lib64", read_exec);
  add_path_rule("/etc/ld.so.cache", read_only);

  // Terminfo paths
  if (config.allow_terminfo) {
    add_path_rule("/usr/share/terminfo", read_only);
    add_path_rule("/etc/terminfo", read_only);
    add_path_rule("/lib/terminfo", read_only);
  }

  // Locale paths
  if (config.allow_locales) {
    add_path_rule("/usr/lib/locale", read_only);
    add_path_rule("/usr/share/locale", read_only);
  }

  for (const auto &p : config.extra_allowed_paths) {
    add_path_rule(p, read_exec);
  }

  if (landlock_restrict_self(ruleset_fd, 0) != 0) {
    ::close(ruleset_fd);
    return std::unexpected("landlock_restrict_self failed: " +
                           std::string(std::strerror(errno)));
  }

  ::close(ruleset_fd);

  // Install Phase 1 Seccomp filter prior to dlopen / foreign constructor
  // execution
  scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
  if (!ctx) {
    return std::unexpected("seccomp_init failed for Phase 1");
  }

  (void)seccomp_attr_set(ctx, SCMP_FLTATR_ACT_BADARCH, SCMP_ACT_KILL_PROCESS);

  int rc = addNetworkAndProcessDenialRules(ctx);

  if (rc != 0) {
    seccomp_release(ctx);
    return std::unexpected("seccomp_rule_add failed in Phase 1");
  }

  if (seccomp_load(ctx) != 0) {
    seccomp_release(ctx);
    return std::unexpected("seccomp_load failed in Phase 1: " +
                           std::string(std::strerror(errno)));
  }

  seccomp_release(ctx);
  return {};
}

std::expected<void, std::string> applyPhase2Lockdown() {
  // Step 8: PR_SET_MDWE (Memory Deny Write Execute)
  (void)::prctl(PR_SET_MDWE, PR_MDWE_REFUSE_EXEC_GAIN, 0, 0, 0);

  // Seccomp-bpf filter: permanently revoke PROT_EXEC on mmap/mprotect,
  // MREMAP_MAYMOVE on mremap, and revoke terminal injection (TIOCSTI) and memfd
  // creation.
  scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
  if (!ctx) {
    return std::unexpected("seccomp_init failed");
  }

  (void)seccomp_attr_set(ctx, SCMP_FLTATR_ACT_BADARCH, SCMP_ACT_KILL_PROCESS);

  int rc = addNetworkAndProcessDenialRules(ctx);

  // Revoke PROT_EXEC in mmap: arg 2 is prot
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(mmap), 1,
                         SCMP_CMP(2, SCMP_CMP_MASKED_EQ, PROT_EXEC, PROT_EXEC));

  // Revoke PROT_EXEC in mprotect: arg 2 is prot
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(mprotect), 1,
                         SCMP_CMP(2, SCMP_CMP_MASKED_EQ, PROT_EXEC, PROT_EXEC));

  // Revoke MREMAP_MAYMOVE in mremap: arg 3 is flags
  rc |= seccomp_rule_add(
      ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(mremap), 1,
      SCMP_CMP(3, SCMP_CMP_MASKED_EQ, MREMAP_MAYMOVE, MREMAP_MAYMOVE));

  // Revoke TIOCSTI terminal injection
  rc |=
      seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ioctl), 1,
                       SCMP_CMP(1, SCMP_CMP_MASKED_EQ, 0xFFFFFFFFULL, TIOCSTI));

  // Revoke memfd_create
  rc |= seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(memfd_create), 0);

  if (rc != 0) {
    seccomp_release(ctx);
    return std::unexpected("seccomp_rule_add failed in Phase 2");
  }

  if (seccomp_load(ctx) != 0) {
    seccomp_release(ctx);
    return std::unexpected("seccomp_load failed in Phase 2: " +
                           std::string(std::strerror(errno)));
  }

  seccomp_release(ctx);
  return {};
}

} // namespace xanadu::vagent::native
