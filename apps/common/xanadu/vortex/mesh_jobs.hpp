#ifndef COMMON_XANADU_VORTEX_MESH_JOBS_HPP
#define COMMON_XANADU_VORTEX_MESH_JOBS_HPP

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "common/xanadu/identity/identity_validation.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/mesh_value.hpp"

namespace zigzag::vortex::mesh {

struct ImmutableJob {
  xanadu::InfoHash program;
  Frontend frontend{Frontend::Vortex};
  std::optional<xanadu::InfoHash> base;
  xanadu::MicroversionId parent;
  EvaluationBudget budget{};
  NodeId requester;
};

[[nodiscard]] std::string encodeJob(const ImmutableJob &job);
[[nodiscard]] std::string jobHash(const ImmutableJob &job);
[[nodiscard]] std::expected<ImmutableJob, std::string>
decodeJob(std::string_view bytes);

struct ProgramSeed {
  xanadu::MadeTorrent torrent;
  std::filesystem::path path;
};
[[nodiscard]] std::expected<ProgramSeed, std::string>
publishProgram(std::string_view source, const std::filesystem::path &seedRoot);

struct CapabilityGrant {
  std::string job;
  NodeId requester;
  NodeId worker;
  std::uint64_t expires{};
  EvaluationBudget budget{};
  xanadu::PublicKey issuer;
  xanadu::identity::HashcashStamp proof;
  xanadu::Signature signature;
};

using DelegatedKeyVerifier = std::function<std::optional<xanadu::PublicKey>(
    const xanadu::identity::Fingerprint &)>;
// Hosts supply both their issuer trust policy and compute/payment policy here.
// An absent callback refuses; possessing a signed identity alone grants no
// work.
using JobAuthorizer =
    std::function<bool(const ImmutableJob &, const CapabilityGrant &)>;

[[nodiscard]] std::string grantSigningBuffer(const CapabilityGrant &grant);
[[nodiscard]] std::string encodeGrant(const CapabilityGrant &grant);
[[nodiscard]] std::expected<CapabilityGrant, std::string>
decodeGrant(std::string_view bytes);
[[nodiscard]] std::expected<void, std::string>
verifyGrant(const ImmutableJob &job, const CapabilityGrant &grant,
            const NodeId &worker,
            const xanadu::identity::Fingerprint &authenticatedPeer,
            const DelegatedKeyVerifier &delegation,
            const JobAuthorizer &authorize, std::uint64_t now);

enum class FutureState { Queued, Running, Succeeded, Failed };
struct FutureSnapshot {
  std::string job;
  FutureState state{FutureState::Queued};
  std::optional<xanadu::InfoHash> result;
  std::optional<PublishedValue> value;
  std::string spoolBundle;
  std::string error;
};

struct JobWorkerOptions {
  NodeId node;
  xanadu::MutableKeys keys;
  std::shared_ptr<xanadu::UserPermascroll> permascroll;
  std::shared_ptr<const xanadu::ContentSource> source;
  std::filesystem::path seedRoot;
  EvaluationBudget budget{};
  ValueLimits values;
  std::size_t maxQueued{64};
  std::size_t maxRetained{1024};
  std::size_t maxProgramBytes{1024 * 1024};
  std::uint8_t hashcashDifficulty{xanadu::identity::kDefaultHashcashDifficulty};
  std::function<std::chrono::steady_clock::time_point()> monotonicNow{
      [] { return std::chrono::steady_clock::now(); }};
  DelegatedKeyVerifier delegation;
  JobAuthorizer authorize;
  std::function<std::expected<xanadu::SignedProvenance, std::string>(
      xanadu::Store &)>
      provenance;
  ValuePrimediaProvenanceSigner primediaProvenance;
  std::function<std::expected<void, std::string>(const ValueSeed &)> seed;
};

/// No caller thread fetches, evaluates, signs provenance or seeds a result.
/// Completed identities remain deduplicated until explicitly forgotten. At the
/// retained bound new jobs are refused rather than evicting dedup history.
class FutureScheduler {
public:
  explicit FutureScheduler(JobWorkerOptions options);
  ~FutureScheduler();
  FutureScheduler(const FutureScheduler &)            = delete;
  FutureScheduler &operator=(const FutureScheduler &) = delete;

  [[nodiscard]] std::expected<std::string, std::string>
  submit(const ImmutableJob &job, const CapabilityGrant &grant,
         const xanadu::identity::Fingerprint &authenticatedPeer,
         std::uint64_t now);
  [[nodiscard]] std::optional<FutureSnapshot> poll(std::string_view job) const;
  [[nodiscard]] bool forget(std::string_view job);

private:
  struct Entry {
    ImmutableJob job;
    CapabilityGrant grant;
    xanadu::identity::Fingerprint peer;
    std::uint64_t admittedWall{};
    std::chrono::steady_clock::time_point admittedAt;
    FutureSnapshot snapshot;
    std::shared_ptr<ValueSeed> seed;
  };
  [[nodiscard]] std::expected<ValueSeed, std::string>
  execute(const ImmutableJob &job);
  void work(std::stop_token stop);

  JobWorkerOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable_any ready_;
  std::deque<std::string> queue_;
  std::unordered_map<std::string, Entry> retained_;
  xanadu::identity::HashcashEngine hashcash_;
  std::jthread thread_;
};

} // namespace zigzag::vortex::mesh
#endif
