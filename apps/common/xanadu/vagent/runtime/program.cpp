#include "common/xanadu/vagent/runtime/program.hpp"

#include <format>
#include <random>

#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vagent/runtime/compiler.hpp"
#include "common/xanadu/vagent/runtime/parser.hpp"
#include "common/xanadu/vortex/worker_context.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::vagent::runtime {

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
  return fromUnit(std::move(*unit), name, refOf(source));
}

std::expected<CompiledProgram, std::string>
CompiledProgram::fromUnit(Unit unit, std::string_view name,
                          std::optional<std::string> ref) {
  auto checked = check(unit);
  if (!checked) {
    std::string message;
    for (const auto &error : checked.error()) {
      if (!message.empty()) message += '\n';
      message += std::format("{}:{}", name, formatCheckError(error));
    }
    return std::unexpected(std::move(message));
  }

  auto schema = makeUpgradeSchema(*checked);
  if (!schema) return std::unexpected(schema.error().message);
  auto programStore = std::make_shared<xanadu::Store>();
  auto curHead      = programStore->sliceGenesis(xanadu::MicroversionId{});
  zigzag::vortex::WorkerContext stagingContext;
  Compiler compiler(stagingContext.core);
  compiler.setPredicateSignatures(predicateSignatures(*schema));
  std::map<std::string, CompiledAgent> stagingAgents;
  for (const auto &[agentName, _] : checked->agents) {
    auto compiled = compiler.compileAgent(unit, agentName);
    if (!compiled) {
      return std::unexpected(std::format(
          "{}:{}:{}: {}: {}", name, compiled.error().loc.line,
          compiled.error().loc.column, compileErrorName(compiled.error().code),
          compiled.error().detail));
    }
    stagingAgents[agentName] = *compiled;
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
      auto it = promoted->mapping.find(cell);
      return it != promoted->mapping.end() ? it->second : cell;
    };
    for (const auto &[name, entry] : stagingAgent.fieldDefaults)
      persistentAgent.fieldDefaults.emplace(name, remap(entry));
    const auto remapContainers = [&](const auto &input, auto &output) {
      for (const auto &[name, source] : input) {
        CompiledContainer target;
        target.owner        = remap(source.owner);
        target.cell         = remap(source.cell);
        target.member       = remap(source.member);
        target.installLabel = remap(source.installLabel);
        target.upgradeLabel = remap(source.upgradeLabel);
        for (const auto member : source.members)
          target.members.push_back(remap(member));
        for (const auto inst : source.instances)
          target.instances.push_back(remap(inst));
        for (const auto &[field, entry] : source.fieldDefaults)
          target.fieldDefaults.emplace(field, remap(entry));
        if (source.upgradeEntry)
          target.upgradeEntry = remap(*source.upgradeEntry);
        output.emplace(name, std::move(target));
      }
    };
    remapContainers(stagingAgent.skills, persistentAgent.skills);
    remapContainers(stagingAgent.behaviors, persistentAgent.behaviors);
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
    if (stagingAgent.sourceChangeEntry) {
      persistentAgent.sourceChangeEntry =
          remap(*stagingAgent.sourceChangeEntry);
    }
    if (stagingAgent.settleEntry) {
      persistentAgent.settleEntry = remap(*stagingAgent.settleEntry);
    }
    if (stagingAgent.healthEntry) {
      persistentAgent.healthEntry = remap(*stagingAgent.healthEntry);
    }
    if (stagingAgent.observeRefusalEntry) {
      persistentAgent.observeRefusalEntry =
          remap(*stagingAgent.observeRefusalEntry);
    }
    if (stagingAgent.observeEntry) {
      persistentAgent.observeEntry = remap(*stagingAgent.observeEntry);
    }
    if (stagingAgent.answerEntry) {
      persistentAgent.answerEntry = remap(*stagingAgent.answerEntry);
    }
    persistentAgents[agentName] = persistentAgent;
  }

  std::string progRef = ref.value_or("");
  if (progRef.empty()) {
    progRef = toHex32(sha256Digest(std::format("{}:{}", kProgramFormat, name)));
  }

  return CompiledProgram(
      std::move(unit), std::move(*checked), std::move(progRef),
      std::move(programStore), std::move(programManifold),
      std::move(persistentAgents),
      std::make_shared<const UpgradeSchema>(std::move(*schema)));
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
  compiler.setPredicateSignatures(predicateSignatures(*schema_))
      ->setBeliefEventDepth(beliefEventDepth_)
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

LocalHost::Options
LocalHost::Options::fromModel(const xanadu::SystemStoreModel &model) {
  Options options;
  options.settings = AgentSettings::fromModel(model);
  const auto maxInst =
      model.getInt64(xanadu::settings::kVagentMaxInstructions,
                     static_cast<std::int64_t>(options.maxInstructions));
  if (maxInst > 0) {
    options.maxInstructions = static_cast<std::uint64_t>(maxInst);
  }
  return options;
}

LocalHost::Options LocalHost::Options::fromStore(const xanadu::Store &store) {
  if (store.opCount() == 0 || !store.homeCell()) {
    return Options{};
  }
  const auto model = xanadu::SystemStoreModel::fromStore(store);
  return fromModel(model);
}

LocalHost::Options
LocalHost::Options::fromManifold(const zigzag::Manifold &manifold,
                                 const std::optional<zigzag::CellRef> homeCell,
                                 const xanadu::SpanReader *reader) {
  const auto model =
      xanadu::SystemStoreModel::fromManifold(manifold, homeCell, reader);
  return fromModel(model);
}

LocalHost::LocalHost(CompiledProgram program)
    : LocalHost(std::move(program), Options{}) {}

LocalHost::LocalHost(CompiledProgram program, Options options)
    : program_(std::make_shared<CompiledProgram>(std::move(program))),
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
  maxInstructions_ = options.maxInstructions;
  settings_        = options.settings;
  clock_           = options.clock;
  programs_.emplace(std::string(program_->programRef()), program_);
  const auto owner = *identity::Fingerprint::fromString(kLocalOwner);
  const auto life  = options.clock.wall() + kLocalGrantLife;
  program_->setBeliefEventDepth(options.settings.beliefEventDepth)
      ->setConditionFuel(options.settings.conditionFuel)
      ->setMaintainBackoffMs(options.settings.maintainBackoff.count());
  if (program_->programManifold()) {
    runtime_->setProgramResolver(
        [this](std::string_view ref) -> const zigzag::Manifold * {
          const auto found = programs_.find(ref);
          return found == programs_.end() ? nullptr
                                          : found->second->programManifold();
        });
  }
  runtime_->setAuthorizer(
      [this, life](const identity::Fingerprint &, const ActorMessage &request)
          -> std::expected<ActorGrant, ActorError> {
        const auto limits = grantLimits(request.programRef);
        if (!limits) return std::unexpected(limits.error());
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
                          limits->instructions,
                          limits->maxCells,
                          kLocalMaxOutputBytes,
                          kLocalMaxSolutions,
                          kLocalMaxMailboxBytes,
                          life,
                          true};
      });
  executor_ = std::make_unique<AgentExecutor>(
      *runtime_, *issuer_, *program_, owner, node_, options.epoch,
      options.settings, std::move(options.clock));
  (void)executor_->retainProgram(program_);
  executor_->install()
      ->setResumeGrantProbe(
          [this](std::string_view ref) { return grantLimits(ref); })
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

std::shared_ptr<CompiledProgram>
LocalHost::retainProgram(CompiledProgram program) {
  const std::string ref(program.programRef());
  if (const auto found = programs_.find(ref); found != programs_.end())
    return found->second;
  program.setBeliefEventDepth(settings_.beliefEventDepth)
      ->setConditionFuel(settings_.conditionFuel)
      ->setMaintainBackoffMs(settings_.maintainBackoff.count());
  auto held = std::make_shared<CompiledProgram>(std::move(program));
  if (!executor_->retainProgram(held)) return nullptr;
  programs_.emplace(ref, held);
  return held;
}

std::expected<ResumeLimits, ActorError>
LocalHost::grantLimits(std::string_view programRef) const {
  if (!programs_.contains(programRef))
    return std::unexpected(ActorError::NotAuthorized);
  return ResumeLimits{maxInstructions_, kLocalMaxCells};
}

std::expected<void, TransferRefusal>
LocalHost::upgrade(const identity::ActorPid &pid,
                   std::shared_ptr<CompiledProgram> program,
                   std::optional<std::uint64_t> instructionLimit) {
  return upgradeWith(pid, std::move(program), instructionLimit, false);
}

std::expected<void, TransferRefusal>
LocalHost::upgradeQuiescing(const identity::ActorPid &pid,
                            std::shared_ptr<CompiledProgram> program,
                            const std::chrono::milliseconds timeout,
                            std::optional<std::uint64_t> instructionLimit) {
  constexpr auto road = TransferRoute::UpgradeSchema;
  const auto deadline = clock_.steady() + timeout;
  // An operator's own pause outlives this call: it is put back at the end,
  // where a bare `resumeAdmission` would cancel it (DIST m7).
  const auto operatorPause = executor_->admissionPausedUntil(pid);
  // The margin covers the upgrade after a drain that ends on its last tick,
  // which otherwise meets a pause that lapsed between the two.
  if (const auto paused = executor_->pauseAdmission(
          pid, timeout + std::chrono::milliseconds(kQuiescingPauseMarginMs));
      !paused)
    return std::unexpected(paused.error());
  const auto drained = [&]() -> std::expected<void, TransferRefusal> {
    for (;;) {
      const auto turns  = pump();
      const auto report = executor_->quiescence(pid);
      if (!report) return std::unexpected(report.error());
      if (report->quiet()) return {};
      // This host has one thread: a pump that ran nothing will not unblock
      // the actor by itself, so waiting for the deadline would only spin.
      if (turns == 0 || clock_.steady() >= deadline) {
        const auto busy =
            report->firstBusy(AgentExecutor::UpgradeRoute::Schema);
        return std::unexpected(TransferRefusal::make(
            TransferCode::NotQuiescent, road, ActorError::NotQuiescent,
            {.subject    = std::string(busy ? busy->first : "quiescence"),
             .savedCount = busy ? busy->second : 0}));
      }
    }
  }();
  auto result =
      drained ? upgradeWith(pid, std::move(program), instructionLimit, true)
              : drained;
  executor_->restoreAdmissionPause(pid, operatorPause);
  // The pause, not the actor, was what failed: say so, where the bare state
  // number (Open) reads as noise.
  if (!result && result.error().code == TransferCode::NotQuiescent &&
      result.error().detail.subject == "admission")
    result.error().detail.current = "the pause lapsed before the swap";
  return result;
}

std::expected<void, TransferRefusal> LocalHost::upgradeWith(
    const identity::ActorPid &pid, std::shared_ptr<CompiledProgram> program,
    std::optional<std::uint64_t> instructionLimit, const bool requirePaused) {
  if (!program || !programs_.contains(program->programRef()) ||
      programs_.at(std::string(program->programRef())).get() != program.get()) {
    return std::unexpected(TransferRefusal::make(TransferCode::Runtime,
                                                 TransferRoute::UpgradeSchema,
                                                 ActorError::NotAuthorized));
  }
  // The local host is the capability authority. The adapter validates every
  // state and schema condition before asking the runtime to install the grant.
  ActorGrant grant{std::string(program->programRef()),
                   std::string(program->programRef()),
                   instructionLimit.value_or(maxInstructions_),
                   kLocalMaxCells,
                   kLocalMaxOutputBytes,
                   kLocalMaxSolutions,
                   kLocalMaxMailboxBytes,
                   clock_.wall() + kLocalGrantLife,
                   true};
  return executor_->upgrade(pid, std::move(program), grant, requirePaused);
}

std::expected<void, TransferRefusal>
LocalHost::upgrade(const identity::ActorPid &pid, CompiledProgram program) {
  constexpr auto road = TransferRoute::UpgradeReconciler;
  // A reference already retained names one program for ever: another schema
  // under it is refused by name, not silently swapped for the held object.
  if (const auto held = programs_.find(std::string(program.programRef()));
      held != programs_.end() &&
      !sameUpgradeSchema(held->second->upgradeSchema(),
                         program.upgradeSchema()))
    return std::unexpected(TransferRefusal::make(
        TransferCode::ProgramReferenceReused, road, ActorError::NotAuthorized,
        {.subject = std::string(program.programRef())}));
  // Before `retainProgram`, so a refusal leaves both registries as they were.
  if (auto refused = executor_->preflightUpgrade(
          pid, program, AgentExecutor::UpgradeRoute::Reconciler))
    return std::unexpected(std::move(*refused));
  auto held = retainProgram(std::move(program));
  if (!held)
    return std::unexpected(TransferRefusal::make(TransferCode::Runtime, road,
                                                 ActorError::NotAuthorized));
  return executor_->upgrade(pid, *held);
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
  if (!program_->declares(agentType)) {
    return std::unexpected(ActorError::InvalidMessage);
  }
  return executor_->launch(agentType, name, std::move(args));
}

std::expected<identity::ActorPid, ActorError>
LocalHost::launch(std::shared_ptr<CompiledProgram> program,
                  std::string_view agentType, std::string_view name,
                  WireRecord args) {
  if (!program || !programs_.contains(program->programRef()) ||
      programs_.at(std::string(program->programRef())).get() != program.get() ||
      !program->declares(agentType))
    return std::unexpected(ActorError::NotAuthorized);
  return executor_->launch(agentType, name, std::move(args),
                           program->programRef());
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

} // namespace xanadu::vagent::runtime
