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

  /// Events agents emitted `in outer`, in arrival order.
  [[nodiscard]] const std::vector<Envelope> &emissions() const noexcept {
    return emissions_;
  }
  [[nodiscard]] AgentExecutor &executor() noexcept { return *executor_; }
  [[nodiscard]] zigzag::vortex::mesh::MeshActorRuntime &runtime() noexcept {
    return *runtime_;
  }
  [[nodiscard]] CompiledProgram &program() noexcept { return program_; }

private:
  class Issuer;
  CompiledProgram program_;
  std::unique_ptr<Issuer> issuer_;
  std::unique_ptr<zigzag::vortex::mesh::MeshActorRuntime> runtime_;
  std::unique_ptr<AgentExecutor> executor_;
  std::vector<Envelope> emissions_;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_PROGRAM_HPP
