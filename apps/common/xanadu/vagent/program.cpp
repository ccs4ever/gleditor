#include "common/xanadu/vagent/program.hpp"

#include <format>
#include <random>

#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vortex/worker_context.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

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

  auto programStore = std::make_shared<xanadu::Store>();
  auto curHead      = programStore->sliceGenesis(xanadu::MicroversionId{});
  zigzag::vortex::WorkerContext stagingContext;
  Compiler compiler(stagingContext.core);
  std::map<std::string, CompiledAgent> stagingAgents;
  for (const auto &[agentName, _] : checked->agents) {
    auto compiled = compiler.compileAgent(*unit, agentName);
    if (compiled) {
      stagingAgents[agentName] = *compiled;
    }
  }

  const auto dVars = stagingContext.arena.ensureDimension("d.vars");
  auto prevCell    = stagingContext.core.home();
  for (const auto &[agentName, compiled] : stagingAgents) {
    (void)stagingContext.arena.link(prevCell, dVars, zigzag::DimVector::POS,
                                    compiled.agentCell);
    prevCell = compiled.agentCell;
  }

  const auto promoted = zigzag::promote(
      *programStore, curHead, stagingContext.arena, stagingContext.core.home());
  if (!promoted) {
    return std::unexpected(
        std::format("{}: failed to promote program store", name));
  }

  auto programManifold = std::make_shared<zigzag::Manifold>(
      programStore->rebuildManifold(promoted->version));

  std::map<std::string, CompiledAgent> persistentAgents;
  for (const auto &[agentName, stagingAgent] : stagingAgents) {
    CompiledAgent persistentAgent;
    const auto remap = [&](zigzag::CellRef cell) -> zigzag::CellRef {
      if (cell == zigzag::noCell) return zigzag::noCell;
      auto it = promoted->mapping.find(cell);
      return it != promoted->mapping.end() ? it->second : cell;
    };
    persistentAgent.agentCell     = remap(stagingAgent.agentCell);
    persistentAgent.initEntry     = remap(stagingAgent.initEntry);
    persistentAgent.dispatchEntry = remap(stagingAgent.dispatchEntry);
    persistentAgent.haltOp        = remap(stagingAgent.haltOp);
    persistentAgent.failOp        = remap(stagingAgent.failOp);
    if (stagingAgent.continueEntry) {
      persistentAgent.continueEntry = remap(*stagingAgent.continueEntry);
    }
    if (stagingAgent.failStepEntry) {
      persistentAgent.failStepEntry = remap(*stagingAgent.failStepEntry);
    }
    if (stagingAgent.answerEntry) {
      persistentAgent.answerEntry = remap(*stagingAgent.answerEntry);
    }
    persistentAgents[agentName] = persistentAgent;
  }

  return CompiledProgram(std::move(*unit), std::move(*checked), refOf(source),
                         std::move(programStore), std::move(programManifold),
                         std::move(persistentAgents));
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
  compiler.setBeliefEventDepth(beliefEventDepth_)
      ->setConditionFuel(conditionFuel_)
      ->setMaintainBackoffMs(maintainBackoffMs_);
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

constexpr std::string_view kCapPrefix = "vagent-local:";
constexpr std::size_t kMacHexDigits   = 64;
constexpr std::size_t kMacKeyBytes    = 32;
// Arena and message ceilings for a local run; the runtime's own defaults
// (128 actors, 128 queued messages) stay in force.
constexpr std::uint32_t kLocalMaxCells      = 1U << 22;
constexpr std::size_t kLocalMaxOutputBytes  = 32768; // the runtime refuses more
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
    : program_(std::move(program)),
      keys_(options.keys ? *options.keys : xanadu::createMutableKeys()),
      node_(zigzag::vortex::mesh::NodeId{options.author, keys_.publicKey.hex(),
                                         options.epoch}
                .str()),
      issuer_(std::make_unique<Issuer>()),
      runtime_(std::make_unique<MeshActorRuntime>(
          node_, options.epoch,
          AgentExecutor::runtimeOptions(options.settings))) {
  if (options.manualClock) {
    manualNow_ = std::make_shared<std::chrono::system_clock::time_point>(
        options.clock.wall());
    options.clock.wall = [now = manualNow_] { return *now; };
  }
  const auto owner = *identity::Fingerprint::fromString(kLocalOwner);
  const auto life  = options.clock.wall() + kLocalGrantLife;
  program_.setBeliefEventDepth(options.settings.beliefEventDepth)
      ->setConditionFuel(options.settings.conditionFuel)
      ->setMaintainBackoffMs(options.settings.maintainBackoff.count());
  if (program_.programManifold()) {
    runtime_->setProgramResolver(
        [this](std::string_view ref) -> const zigzag::Manifold * {
          if (ref == program_.programRef()) {
            return program_.programManifold();
          }
          return nullptr;
        });
  }
  runtime_->setAuthorizer(
      [this, instructions = options.maxInstructions,
       life](const identity::Fingerprint &, const ActorMessage &request)
          -> std::expected<ActorGrant, ActorError> {
        if (request.programRef != program_.programRef()) {
          return std::unexpected(ActorError::NotAuthorized);
        }
        // A resume request names the checkpoint its grant is based on.
        std::string base = "base";
        if (const auto parameters = issuer_->parametersOf(request.capability)) {
          if (const auto decoded = decodeValue(*parameters)) {
            if (const auto *record = std::get_if<WireRecord>(&decoded->data)) {
              if (const auto *named = record->find(kWireSpawnBase)) {
                if (const auto *hash = std::get_if<std::string>(&named->data)) {
                  base = *hash;
                }
              }
            }
          }
        }
        return ActorGrant{request.programRef,
                          base,
                          instructions,
                          kLocalMaxCells,
                          kLocalMaxOutputBytes,
                          kLocalMaxSolutions,
                          kLocalMaxMailboxBytes,
                          life,
                          true};
      });
  executor_ = std::make_unique<AgentExecutor>(
      *runtime_, *issuer_, program_, owner, node_, options.epoch,
      options.settings, std::move(options.clock));
  executor_->install()
      ->setBindings(&bindings_)
      ->setHostSink(
          [this](const Envelope &event) { emissions_.push_back(event); });
  seedRoot_ = std::filesystem::temp_directory_path() /
              ("vagent-host-" + keys_.publicKey.hex() + "-" +
               std::to_string(options.epoch));
  executor_->setCheckpointIdentity(CheckpointIdentity{
      keys_,
      zigzag::vortex::mesh::NodeId{options.author, keys_.publicKey.hex(),
                                   options.epoch},
      seedRoot_, xanadu::SignedProvenance{"vagent-local-host", "unsigned"}});
}

LocalHost::~LocalHost() {
  std::error_code ignored;
  std::filesystem::remove_all(seedRoot_, ignored);
}

void LocalHost::connect(LocalHost &peer) {
  const auto owner = *identity::Fingerprint::fromString(kLocalOwner);
  const auto link  = [&owner](LocalHost &from, LocalHost &to) {
    from.runtime_->trustNode(to.node_, owner);
    from.runtime_->setSender(
        from.executor_->loopbackSender(to.executor_->loopbackSender()));
  };
  link(*this, peer);
  link(peer, *this);
}

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

void LocalHost::percept(const identity::ActorPid &target, std::string type,
                        WireRecord fields) {
  executor_->percept(target, std::move(type), std::move(fields));
}

std::size_t LocalHost::advance(std::chrono::milliseconds by,
                               std::size_t maxTurns) {
  if (manualNow_) *manualNow_ += by;
  executor_->tick();
  return executor_->pumpUntilIdle(maxTurns);
}

std::size_t LocalHost::pump(std::size_t maxTurns) {
  return executor_->pumpUntilIdle(maxTurns);
}

} // namespace xanadu::vagent
