/**
 * @file program.hpp
 * @brief A vagent source text as an `AgentProgram`: parsed and checked once,
 *        compiled into each agent's own arena on that agent's first turn
 *        (design §4.4, §11.1).
 *
 * The sealed program artifact of §11.1 does not exist yet, so `programRef` is
 * a hash of the source text and a format version: two hosts that hold the same
 * text agree on what a spawn names. When the artifact lands the ref becomes
 * the artifact's hash and `fromSource` becomes one way to make one.
 */
#ifndef COMMON_XANADU_VAGENT_PROGRAM_HPP
#define COMMON_XANADU_VAGENT_PROGRAM_HPP

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/agent_executor.hpp"
#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/checker.hpp"

namespace xanadu::vagent {

/// Bumped when the compiled shape of a program changes meaning, so a ref made
/// by an older compiler never names a program this one would build differently.
inline constexpr std::string_view kProgramFormat = "vagent-program-1";

class CompiledProgram final : public AgentProgram {
public:
  /// Parse and check @p source. Every parse or check error comes back as one
  /// `name:line:col` line per error; warnings do not refuse.
  [[nodiscard]] static std::expected<CompiledProgram, std::string>
  fromSource(std::string_view source, std::string_view name = "<source>");

  [[nodiscard]] std::string_view programRef() const override { return ref_; }
  [[nodiscard]] std::expected<CompiledAgent, std::string>
  instantiate(zigzag::vortex::WorkerContext &context,
              std::string_view agentType) override;

  /// `vagent.belief_event_depth` for agents instantiated afterwards.
  CompiledProgram *setBeliefEventDepth(std::size_t rounds) noexcept {
    beliefEventDepth_ = rounds;
    return this;
  }
  [[nodiscard]] const Unit &unit() const noexcept { return unit_; }
  [[nodiscard]] const CheckedUnit &checked() const noexcept { return checked_; }
  [[nodiscard]] bool declares(std::string_view agentType) const;

private:
  CompiledProgram(Unit unit, CheckedUnit checked, std::string ref)
      : unit_(std::move(unit)), checked_(std::move(checked)),
        ref_(std::move(ref)) {}

  Unit unit_;
  CheckedUnit checked_;
  std::string ref_;
  std::size_t beliefEventDepth_{kDefaultBeliefEventDepth};
};

/// Everything a single-process host needs to run agents: a mesh runtime that
/// trusts only this node, an issuer, the adapter, and a sink that collects what
/// agents emit to the launcher's space. The `vagent run` command and the
/// end-to-end tests both build one.
///
/// The issuer is a local stand-in for the host's delegated device key: it
/// "signs" a ticket's parameters with a SHA-256 MAC under a random per-host
/// key. That authenticates nothing across hosts (the key never leaves the
/// process) but makes `parametersOf` refuse a capability this host did not
/// issue, which is the property the adapter relies on.
class LocalHost {
public:
  struct Options {
    AgentSettings settings{};
    /// Per-turn instruction budget granted to every agent. Compiled routines
    /// are long straight-line opcode graphs, so the default is generous.
    std::uint64_t maxInstructions{10'000'000};
    /// Optional test hooks.
    AgentClock clock{AgentClock::system()};
    /// Replace `clock` with one that starts at the system time and moves only
    /// when `advance` is called, so timers run deterministically.
    bool manualClock{false};
    /// The node's name is `author:device:epoch`, the shape `resume` demands of
    /// a checkpoint's origin, with the device named by its public key (the
    /// key that signs checkpoints). Unset `keys` mint a new device; passing a
    /// host's `deviceKeys()` with a larger epoch stands for that device
    /// restarted.
    std::string author{"local"};
    std::optional<xanadu::MutableKeys> keys;
    std::uint64_t epoch{1};
  };

  explicit LocalHost(CompiledProgram program);
  LocalHost(CompiledProgram program, Options options);
  ~LocalHost();
  LocalHost(const LocalHost &)            = delete;
  LocalHost &operator=(const LocalHost &) = delete;

  [[nodiscard]] std::expected<identity::ActorPid,
                              zigzag::vortex::mesh::ActorError>
  launch(std::string_view agentType, std::string_view name,
         WireRecord args = {});
  [[nodiscard]] std::expected<void, zigzag::vortex::mesh::ActorError>
  send(const identity::ActorPid &target, Envelope event);
  std::size_t pump(std::size_t maxTurns = 100000);
  /// Post an event of type @p type to @p target as a percept (§7.5), as a
  /// native skill's driver does; run `pump` to deliver it. Takes no mailbox
  /// slot and is heard before a checkpoint is accepted.
  void percept(const identity::ActorPid &target, std::string type,
               WireRecord fields = {});
  /// The native skills this host offers its agents (§5.3), keyed by qualified
  /// skill name. Bind before launching an agent that calls one.
  [[nodiscard]] HostBindings &bindings() noexcept { return bindings_; }

  /// Moves a manual clock forward, wakes agents whose timers fell due and
  /// pumps. Without `Options::manualClock` there is no clock to move, so it
  /// only ticks and pumps. Returns the turns run.
  std::size_t advance(std::chrono::milliseconds by,
                      std::size_t maxTurns = 100000);

  /// Events agents emitted `in outer`, in arrival order.
  [[nodiscard]] const std::vector<Envelope> &emissions() const noexcept {
    return emissions_;
  }
  [[nodiscard]] AgentExecutor &executor() noexcept { return *executor_; }
  [[nodiscard]] zigzag::vortex::mesh::MeshActorRuntime &runtime() noexcept {
    return *runtime_;
  }
  [[nodiscard]] CompiledProgram &program() noexcept { return program_; }
  [[nodiscard]] const std::string &node() const noexcept { return node_; }
  [[nodiscard]] const xanadu::MutableKeys &deviceKeys() const noexcept {
    return keys_;
  }

  /// Let this host and @p peer deliver to each other, so an agent resumed on
  /// one can talk to its parent and children on the other. Both must outlive
  /// the connection.
  void connect(LocalHost &peer);

private:
  class Issuer;
  CompiledProgram program_;
  xanadu::MutableKeys keys_;
  std::string node_;
  std::filesystem::path seedRoot_;
  std::unique_ptr<Issuer> issuer_;
  /// Outlives the executor, whose in-flight effects it still holds.
  HostBindings bindings_;
  std::unique_ptr<zigzag::vortex::mesh::MeshActorRuntime> runtime_;
  std::unique_ptr<AgentExecutor> executor_;
  std::vector<Envelope> emissions_;
  std::shared_ptr<std::chrono::system_clock::time_point> manualNow_;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_PROGRAM_HPP
