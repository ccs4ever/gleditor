#ifndef VAGENT_NATIVE_HOST_SANDBOX_LINUX_HPP
#define VAGENT_NATIVE_HOST_SANDBOX_LINUX_HPP

#include <cstdint>
#include <expected>
#include <string>
#include <sys/types.h>
#include <vector>

namespace xanadu::vagent::native {

struct SandboxConfig {
  std::string staging_dir;
  std::vector<std::string> extra_allowed_paths;
  bool allow_terminfo{true};
  bool allow_locales{true};
};

/// Step 1: Bind termination to supervisor process lifetime.
[[nodiscard]] std::expected<void, std::string>
bindParentDeath(pid_t supervisor_pid);

/// Step 2: Descriptor hygiene - close_range(8, ~0U, 0) and verify fds 0..7 are
/// open.
[[nodiscard]] std::expected<void, std::string> performDescriptorHygiene();

/// Step 3: Apply resource limits (RLIMIT_AS, RLIMIT_NOFILE, RLIMIT_CORE=0,
/// RLIMIT_FSIZE=0).
[[nodiscard]] std::expected<void, std::string>
setResourceLimits(uint64_t memory_limit_bytes = 512 * 1024 * 1024,
                  uint64_t max_fds            = 64);

/// Step 6: Phase 1 Sandbox - Landlock ruleset granting read/execute to staging
/// and system library/terminfo paths, and prctl(PR_SET_NO_NEW_PRIVS, 1).
[[nodiscard]] std::expected<void, std::string>
applyPhase1Sandbox(const SandboxConfig &config);

/// Step 8: Phase 2 Lockdown - prctl(PR_SET_MDWE, PR_MDWE_REFUSE_EXEC_GAIN) and
/// seccomp-bpf filter permanently revoking PROT_EXEC and MREMAP_MAYMOVE.
[[nodiscard]] std::expected<void, std::string> applyPhase2Lockdown();

} // namespace xanadu::vagent::native

#endif // VAGENT_NATIVE_HOST_SANDBOX_LINUX_HPP
