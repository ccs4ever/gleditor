#include "common/xanadu/vagent/program.hpp"

#include <format>
#include <random>

#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vagent/parser.hpp"

namespace xanadu::vagent {

namespace {

// The format is part of the hashed text so a bump changes every ref; the
// separator is a byte no source file may contain.
constexpr char kRefSeparator = '\0';

std::string refOf(std::string_view source) {
  std::string material(kProgramFormat);
  material += kRefSeparator;
  material += source;
  return toHex32(sha256Digest(material));
}

} // namespace

std::expected<CompiledProgram, std::string>
CompiledProgram::fromSource(std::string_view source, std::string_view name) {
  auto unit = parseUnit(source);
  if (!unit) {
    return std::unexpected(
        std::format("{}:{}", name, formatParseError(unit.error())));
  }
  auto checked = check(*unit);
  if (!checked) {
    std::string message;
    for (const auto &error : checked.error()) {
      if (!message.empty()) message += '\n';
      message += std::format("{}:{}", name, formatCheckError(error));
    }
    return std::unexpected(std::move(message));
  }
  return CompiledProgram(std::move(*unit), std::move(*checked), refOf(source));
}

bool CompiledProgram::declares(std::string_view agentType) const {
  return checked_.agents.contains(std::string(agentType));
}

std::expected<CompiledAgent, std::string>
CompiledProgram::instantiate(zigzag::vortex::WorkerContext &context,
                             std::string_view agentType) {
  if (!declares(agentType)) {
    return std::unexpected(
        std::format("unknown agent type '{}' in program", agentType));
  }
  Compiler compiler(context.core);
  auto compiled = compiler.compileAgent(unit_, agentType);
  if (!compiled) {
    return std::unexpected(std::format(
        "{}:{}: {}: {}", agentType, compiled.error().loc.line,
        compileErrorName(compiled.error().code), compiled.error().detail));
  }
  return *compiled;
}

// -- LocalHost
// ---------------------------------------------------------------

namespace {

using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;
using zigzag::vortex::mesh::MeshActorRuntime;

constexpr std::string_view kLocalNode = "local";
constexpr std::uint64_t kLocalEpoch   = 1;
constexpr std::string_view kCapPrefix = "vagent-local:";
constexpr std::size_t kMacHexDigits   = 64;
constexpr std::size_t kMacKeyBytes    = 32;
// Arena and message ceilings for a local run; the runtime's own defaults
// (128 actors, 128 queued messages) stay in force.
constexpr std::uint32_t kLocalMaxCells      = 1U << 22;
constexpr std::size_t kLocalMaxOutputBytes = 32768; // the runtime refuses more
constexpr std::size_t kLocalMaxSolutions    = 1U << 12;
constexpr std::size_t kLocalMaxMailboxBytes = 1U << 20;
constexpr std::chrono::hours kLocalGrantLife{24};
// Any 40-hex fingerprint will do for the owner of a purely local mesh.
constexpr std::string_view kLocalOwner =
    "0123456789abcdef0123456789abcdef01234567";

std::string randomKey() {
  std::random_device entropy;
  std::string key;
  for (std::size_t i = 0; i < kMacKeyBytes; ++i) {
    key.push_back(static_cast<char>(entropy()));
  }
  return key;
}

} // namespace

class LocalHost::Issuer final : public CapabilityIssuer {
public:
  std::expected<IssuedCapability, ActorError>
  issue(const SpawnTicket &ticket) override {
    return IssuedCapability{std::string(kCapPrefix) + mac(ticket.parameters) +
                                ':' + ticket.parameters,
                            ""};
  }
  [[nodiscard]] std::optional<std::string>
  parametersOf(std::string_view capability) const override {
    if (!capability.starts_with(kCapPrefix)) return std::nullopt;
    capability.remove_prefix(kCapPrefix.size());
    if (capability.size() <= kMacHexDigits + 1 ||
        capability[kMacHexDigits] != ':') {
      return std::nullopt;
    }
    const auto given      = capability.substr(0, kMacHexDigits);
    const auto parameters = capability.substr(kMacHexDigits + 1);
    if (mac(parameters) != given) return std::nullopt;
    return std::string(parameters);
  }

private:
  [[nodiscard]] std::string mac(std::string_view parameters) const {
    return toHex32(sha256Digest(key_ + std::string(parameters)));
  }
  std::string key_{randomKey()};
};

LocalHost::LocalHost(CompiledProgram program)
    : LocalHost(std::move(program), Options{}) {}

LocalHost::LocalHost(CompiledProgram program, Options options)
    : program_(std::move(program)), issuer_(std::make_unique<Issuer>()),
      runtime_(std::make_unique<MeshActorRuntime>(
          std::string(kLocalNode), kLocalEpoch, AgentExecutor::withFloors())) {
  const auto owner = *identity::Fingerprint::fromString(kLocalOwner);
  const auto life  = options.clock.wall() + kLocalGrantLife;
  runtime_->setAuthorizer(
      [this, instructions = options.maxInstructions,
       life](const identity::Fingerprint &, const ActorMessage &request)
          -> std::expected<ActorGrant, ActorError> {
        if (request.programRef != program_.programRef()) {
          return std::unexpected(ActorError::NotAuthorized);
        }
        return ActorGrant{request.programRef,
                          "base",
                          instructions,
                          kLocalMaxCells,
                          kLocalMaxOutputBytes,
                          kLocalMaxSolutions,
                          kLocalMaxMailboxBytes,
                          life,
                          true};
      });
  executor_ = std::make_unique<AgentExecutor>(
      *runtime_, *issuer_, program_, owner, std::string(kLocalNode),
      kLocalEpoch, options.settings, std::move(options.clock));
  executor_->install()->setHostSink(
      [this](const Envelope &event) { emissions_.push_back(event); });
}

LocalHost::~LocalHost() = default;

std::expected<identity::ActorPid, ActorError>
LocalHost::launch(std::string_view agentType, std::string_view name,
                  WireRecord args) {
  if (!program_.declares(agentType)) {
    return std::unexpected(ActorError::InvalidMessage);
  }
  return executor_->launch(agentType, name, std::move(args));
}

std::expected<void, ActorError>
LocalHost::send(const identity::ActorPid &target, Envelope event) {
  return executor_->deliver(target, std::move(event));
}

std::size_t LocalHost::pump(std::size_t maxTurns) {
  return executor_->pumpUntilIdle(maxTurns);
}

} // namespace xanadu::vagent
