/**
 * @file vagent_effects_test.cpp
 * @brief The adapter's side of effects, native skills, percepts and
 *        intention scheduling (design §4.2 drain order, §5.3, §5.6, §6.5,
 *        §6.6, §7.5), driven with hand-built outbox entries and intention
 *        cells: the compiler side comes later.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/vagent/runtime/agent_executor.hpp"
#include "common/xanadu/vagent/runtime/host_bindings.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::runtime {
namespace {

using identity::ActorPid;
using identity::Fingerprint;
using identity::MessageType;
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;
using zigzag::vortex::mesh::ActorTime;
using zigzag::vortex::mesh::MeshActorRuntime;
using namespace std::chrono_literals;

constexpr std::string_view kMotors = "Rover.Motors";
constexpr std::string_view kArm    = "Rover.Arm";

// -- A tiny assembler, as in vagent_executor_test.cpp -----------------------

struct Asm {
  WorkerContext &c;
  CellRef halt{};
  CellRef fail{};

  explicit Asm(WorkerContext &ctx) : c(ctx) {
    registerDimensions(ctx.arena);
    halt = c.vm.mintOpcode(OpcodeKind::Halt);
    fail = c.vm.mintOpcode(OpcodeKind::Halt);
  }
  CellRef scalar(std::int64_t v) { return c.arena.makeScalarCell(v); }
  CellRef add(CellRef counter) {
    const auto op = c.vm.mintOpcode(OpcodeKind::Add);
    c.core.bindInput(op, counter);
    c.core.bindInput(op, scalar(1));
    c.core.bindOutput(op, counter);
    return op;
  }
  CellRef failOpcode() { return c.vm.mintOpcode(OpcodeKind::Fail); }
  CellRef chain(std::vector<CellRef> ops) {
    const auto jump = c.vm.mintOpcode(OpcodeKind::Jump);
    c.core.bindInput(jump, scalar(static_cast<std::int64_t>(halt)));
    ops.push_back(jump);
    for (std::size_t i = 0; i + 1 < ops.size(); ++i) {
      zigzag::expectWritten(c.arena.link(ops[i], c.core.dims().spin,
                                         zigzag::DimVector::POS, ops[i + 1]));
    }
    return ops.front();
  }
  CellRef counterField(CellRef agent, std::string_view name) {
    const auto cell = scalar(0);
    setField(c.arena, valueDims(c.core), agent, name, cell);
    return cell;
  }
};

class TestProgram final : public AgentProgram {
public:
  using Builder = std::function<CompiledAgent(WorkerContext &)>;
  std::map<std::string, Builder> types;
  [[nodiscard]] std::string_view programRef() const override { return "prog"; }
  std::expected<CompiledAgent, std::string>
  instantiate(WorkerContext &ctx, std::string_view type) override {
    const auto found = types.find(std::string(type));
    if (found == types.end()) return std::unexpected("unknown type");
    return found->second(ctx);
  }
};

class FakeIssuer final : public CapabilityIssuer {
public:
  std::expected<IssuedCapability, ActorError>
  issue(const SpawnTicket &ticket) override {
    return IssuedCapability{"cap:" + ticket.childName + ":" + ticket.parameters,
                            ""};
  }
  [[nodiscard]] std::optional<std::string>
  parametersOf(std::string_view capability) const override {
    constexpr std::string_view prefix = "cap:";
    if (!capability.starts_with(prefix)) return std::nullopt;
    const auto colon = capability.find(':', prefix.size());
    if (colon == std::string_view::npos) return std::nullopt;
    return std::string(capability.substr(colon + 1));
  }
};

/// A device that answers when the test says so, and writes what it is asked
/// to a shared log so the order across devices can be checked.
class FakeDevice final : public NativeSkill {
public:
  explicit FakeDevice(std::vector<std::string> &log) : log_(log) {}
  bool completeInStart{false};
  std::map<std::string, EffectCompletion> pending;
  std::map<std::uint64_t, std::string> methodOf;

  void start(const EffectRequest &request, EffectCompletion done) override {
    log_.push_back("start:" + request.method);
    methodOf[request.token] = request.method;
    if (completeInStart) {
      done(EffectResult::success(WireValue::integer(7)));
      return;
    }
    pending[request.method] = std::move(done);
  }
  void cancel(std::uint64_t token) override {
    log_.push_back("cancel:" + methodOf[token]);
  }
  /// Report @p method finished, as a device's own thread would.
  void finish(const std::string &method, EffectResult result) {
    pending.at(method)(std::move(result));
  }

private:
  std::vector<std::string> &log_;
};

const auto wallStart = std::chrono::system_clock::time_point(1000s);

class VagentEffects : public testing::Test {
protected:
  Fingerprint owner = *Fingerprint::fromString(std::string(40, 'A'));
  std::chrono::system_clock::time_point now = wallStart;
  FakeIssuer issuer;
  TestProgram program;
  HostBindings bindings;
  std::vector<std::string> log;
  std::shared_ptr<FakeDevice> motors;
  std::shared_ptr<FakeDevice> arm;
  std::unique_ptr<MeshActorRuntime> runtime;
  std::unique_ptr<AgentExecutor> exec;
  /// The events agents were handed, in order.
  std::vector<WireRecord> heard;
  /// Everything observable in order: messages, wake turns, devices.
  std::vector<std::string> order;
  std::vector<Envelope> emitted;

  void SetUp() override {
    motors                 = std::make_shared<FakeDevice>(log);
    arm                    = std::make_shared<FakeDevice>(log);
    program.types["Plain"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Plain"));
      const auto inits = a.counterField(agent, "inits");
      const auto msgs  = a.counterField(agent, "msgs");
      return CompiledAgent{agent, a.chain({a.add(inits)}),
                           a.chain({a.add(msgs)}), a.halt, a.fail};
    };
    program.types["Worker"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Worker"));
      const auto inits = a.counterField(agent, "inits");
      const auto msgs  = a.counterField(agent, "msgs");
      const auto cont  = a.counterField(agent, "continued");
      const auto fails = a.counterField(agent, "failed_steps");
      CompiledAgent compiled{agent, a.chain({a.add(inits)}),
                             a.chain({a.add(msgs)}), a.halt, a.fail};
      compiled.continueEntry = a.chain({a.add(cont)});
      compiled.failStepEntry = a.chain({a.add(fails)});
      return compiled;
    };
    program.types["Brittle"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Brittle"));
      const auto msgs  = a.counterField(agent, "msgs");
      const auto cont  = a.counterField(agent, "continued");
      const auto fails = a.counterField(agent, "failed_steps");
      CompiledAgent compiled{agent,
                             a.chain({a.add(a.counterField(agent, "i"))}),
                             a.chain({a.add(msgs)}), a.halt, a.fail};
      compiled.continueEntry = a.chain({a.add(cont), a.failOpcode()});
      compiled.failStepEntry = a.chain({a.add(fails)});
      return compiled;
    };
    program.types["NoRecovery"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("NoRecovery"));
      CompiledAgent compiled{
          agent, a.chain({a.add(a.counterField(agent, "i"))}),
          a.chain({a.add(a.counterField(agent, "msgs"))}), a.halt, a.fail};
      compiled.continueEntry = a.chain({a.failOpcode()});
      return compiled;
    };
  }

  void build(AgentSettings settings = {}, BindingOptions motorOptions = {},
             BindingOptions armOptions = {}) {
    bindings.bind(std::string(kMotors), motors, std::move(motorOptions));
    bindings.bind(std::string(kArm), arm, std::move(armOptions));
    runtime = std::make_unique<MeshActorRuntime>("node", 1,
                                                 AgentExecutor::withFloors());
    runtime->setAuthorizer(
        [this](const Fingerprint &, const ActorMessage &request)
            -> std::expected<ActorGrant, ActorError> {
          return ActorGrant{
              request.programRef, "base", 10000, 10000, 32768, 100, 65536,
              now + 1000s,        true};
        });
    AgentClock clock{.wall   = [this] { return now; },
                     .steady = [] { return ActorTime{}; }};
    exec = std::make_unique<AgentExecutor>(*runtime, issuer, program, owner,
                                           "node", 1, settings, clock);
    exec->install();
    exec->setBindings(&bindings);
    exec->setHostSink([this](const Envelope &e) {
      emitted.push_back(e);
      order.push_back("event:" + e.type);
    });
    exec->setTurnObserver(
        [this](const ActorPid &, const WorkerContext &ctx, CellRef event) {
          const auto value = readValue(ctx.arena, valueDims(ctx.core), event);
          if (!value) return;
          if (const auto *record = std::get_if<WireRecord>(&value->data)) {
            heard.push_back(*record);
            if (record->type == "Ping") order.push_back("M");
          }
        });
  }

  [[nodiscard]] std::int64_t nowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch())
        .count();
  }

  ActorPid launch(std::string_view type) {
    const auto pid = exec->launch(type, "a");
    EXPECT_TRUE(pid);
    exec->pumpUntilIdle();
    return *pid;
  }

  WorkerContext &ctx(const ActorPid &pid) {
    auto *c = exec->context(pid);
    EXPECT_NE(c, nullptr);
    return *c;
  }
  CellRef agent(const ActorPid &pid) { return *exec->agentCell(pid); }

  std::int64_t counter(const ActorPid &pid, std::string_view name) {
    auto &c         = ctx(pid);
    const auto cell = fieldValue(c.arena, valueDims(c.core), agent(pid), name);
    EXPECT_TRUE(cell) << name;
    return cell ? c.arena.asInt64(*cell).value_or(-1) : -1;
  }

  // -- Cells the compiler will one day write

  CellRef addIntention(const ActorPid &pid, std::int64_t id,
                       std::optional<std::int64_t> ready,
                       std::optional<WireRecord> waiting = std::nullopt,
                       std::optional<CellRef> parent     = std::nullopt) {
    auto &c         = ctx(pid);
    const auto vd   = valueDims(c.core);
    const auto dim  = registerDimensions(c.arena);
    const auto cell = c.arena.makeCell(std::string_view("intention"));
    setField(c.arena, vd, cell, kIntentionId, c.arena.makeScalarCell(id));
    if (ready) {
      setField(c.arena, vd, cell, kIntentionReady,
               c.arena.makeScalarCell(*ready));
    }
    if (waiting) {
      setField(c.arena, vd, cell, kIntentionWaiting,
               writeValue(c.arena, vd, WireValue::record(*waiting)));
    }
    if (parent) {
      appendToRank(c.arena, *parent, dim.subgoal, cell);
    } else {
      appendToRank(c.arena, agent(pid), dim.intention, cell);
    }
    return cell;
  }

  static WireRecord waitingRecord(std::string_view kind, std::int64_t key,
                                  std::optional<std::int64_t> deadline) {
    WireRecord w;
    w.type = "Waiting";
    w.set(std::string(kWaitingKind), WireValue::text(std::string(kind)));
    w.set(std::string(kWaitingKey), WireValue::integer(key));
    if (deadline) {
      w.set(std::string(kWaitingDeadline), WireValue::integer(*deadline));
    }
    return w;
  }

  void stage(const ActorPid &pid, CellRef entry) {
    auto &c = ctx(pid);
    appendToRank(c.arena, agent(pid), registerDimensions(c.arena).outbox,
                 entry);
  }

  CellRef effectEntry(const ActorPid &pid, std::int64_t id,
                      std::string_view method, std::int64_t deadline,
                      std::optional<std::int64_t> intention = std::nullopt,
                      bool preempts = false, std::string_view skill = kMotors,
                      std::string_view capacity = "Drive") {
    auto &c          = ctx(pid);
    const auto vd    = valueDims(c.core);
    const auto entry = c.arena.makeCell(std::string_view(kKindEffect));
    setField(c.arena, vd, entry, kOutboxEffectId, c.arena.makeScalarCell(id));
    setField(c.arena, vd, entry, kOutboxEffectSkill, c.arena.makeCell(skill));
    setField(c.arena, vd, entry, kOutboxEffectCapacity,
             c.arena.makeCell(capacity));
    setField(c.arena, vd, entry, kOutboxEffectMethod, c.arena.makeCell(method));
    setField(c.arena, vd, entry, kOutboxEffectArgs,
             writeValue(c.arena, vd, WireValue::list({WireValue::integer(3)})));
    setField(c.arena, vd, entry, kOutboxEffectDeadline,
             c.arena.makeScalarCell(deadline));
    if (intention) {
      setField(c.arena, vd, entry, kOutboxEffectIntention,
               c.arena.makeScalarCell(*intention));
    }
    setField(c.arena, vd, entry, kOutboxEffectPreempts,
             c.arena.makeScalarCell(preempts));
    return entry;
  }

  CellRef cancelEntry(const ActorPid &pid, std::int64_t id) {
    auto &c          = ctx(pid);
    const auto vd    = valueDims(c.core);
    const auto entry = c.arena.makeCell(std::string_view(kKindEffectCancel));
    setField(c.arena, vd, entry, kOutboxEffectId, c.arena.makeScalarCell(id));
    return entry;
  }

  CellRef eventEntry(const ActorPid &pid, std::string_view type) {
    auto &c          = ctx(pid);
    const auto vd    = valueDims(c.core);
    const auto entry = c.arena.makeCell(std::string_view(kKindEvent));
    WireRecord event;
    event.type = std::string(type);
    setField(c.arena, vd, entry, kOutboxEvent,
             writeValue(c.arena, vd, WireValue::record(std::move(event))));
    setField(c.arena, vd, entry, kOutboxSpace,
             c.arena.makeCell(std::string_view(kSpaceOuter)));
    return entry;
  }

  /// Run a plain wake turn, which drains whatever was staged.
  void wake(const ActorPid &pid) { ASSERT_TRUE(runtime->wake(pid, now)); }

  std::optional<WireRecord> waitingOf(const ActorPid &pid, std::int64_t id) {
    auto &c         = ctx(pid);
    const auto vd   = valueDims(c.core);
    const auto dims = registerDimensions(c.arena);
    for (const auto top :
         zigzag::rankAfter(c.arena, agent(pid), dims.intention)) {
      const auto found = fieldValue(c.arena, vd, top, kIntentionId);
      if (!found || c.arena.asInt64(*found) != id) continue;
      const auto waiting = fieldValue(c.arena, vd, top, kIntentionWaiting);
      if (!waiting) return std::nullopt;
      const auto value = readValue(c.arena, vd, *waiting);
      if (value) {
        if (const auto *r = std::get_if<WireRecord>(&value->data)) return *r;
      }
    }
    return std::nullopt;
  }

  ActorMessage external(const ActorPid &target, std::string id) {
    Envelope e;
    e.kind    = "event";
    e.type    = "Ping";
    e.ordinal = 1;
    e.floor   = "none";
    ActorMessage m;
    m.type      = MessageType::ActorSend;
    m.requestId = std::move(id);
    m.source    = ActorPid{"node", "vagent-host", 1, 1};
    m.target    = target;
    m.value     = *encodeEnvelope(e);
    return m;
  }
};

// -- Native skills --------------------------------------------------------

TEST_F(VagentEffects, StartReturnsAtOnceAndTheResultArrivesAsAHostDelivery) {
  build();
  const auto pid = launch("Plain");
  addIntention(pid, 1, std::nullopt,
               waitingRecord(kWaitKindEffect, 7, nowMs() + 5000));
  stage(pid, effectEntry(pid, 7, "move", nowMs() + 5000, 1));
  wake(pid);
  EXPECT_EQ(log, (std::vector<std::string>{"start:move"}));
  EXPECT_EQ(exec->effectsInFlight(pid), 1U);
  EXPECT_TRUE(exec->readyIntentions(pid).empty());
  // A device answers from its own thread (here, the test's): nothing in the
  // agent has run, and the agent cannot be checkpointed until it has.
  motors->finish("move", EffectResult::success(WireValue::integer(42)));
  EXPECT_TRUE(exec->readyIntentions(pid).empty());
  EXPECT_EQ(exec->checkpoint(pid).error(),
            CheckpointRefusal::HostDeliveryPending);
  EXPECT_EQ(runtime->pending(pid), 0U) << "a result takes no mailbox slot";

  exec->pumpUntilIdle();
  EXPECT_EQ(exec->readyIntentions(pid), (std::vector<std::int64_t>{1}));
  EXPECT_EQ(exec->effectsInFlight(pid), 0U);
  const auto waiting = waitingOf(pid, 1);
  ASSERT_TRUE(waiting);
  const auto *match = waiting->find(kWaitingMatch);
  ASSERT_NE(match, nullptr);
  const auto &outcome = std::get<WireRecord>(match->data);
  EXPECT_EQ(outcome.type, kEffectOutcomeRecord);
  EXPECT_EQ(*outcome.find(kEffectOutcomeOk), WireValue::boolean(true));
  EXPECT_EQ(*outcome.find(kEffectOutcomeValue), WireValue::integer(42));
  EXPECT_EQ(waiting->find(kWaitingTimedOut), nullptr);
}

TEST_F(VagentEffects, ACompletionInsideStartIsStillOnlyPosted) {
  build();
  motors->completeInStart = true;
  const auto pid          = launch("Plain");
  addIntention(pid, 1, std::nullopt,
               waitingRecord(kWaitKindEffect, 7, nowMs() + 5000));
  stage(pid, effectEntry(pid, 7, "move", nowMs() + 5000, 1));
  wake(pid);
  EXPECT_TRUE(exec->readyIntentions(pid).empty())
      << "the binding's call must not reach the agent";
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->readyIntentions(pid), (std::vector<std::int64_t>{1}));
}

TEST_F(VagentEffects, AFailureWakesTheIntentionWithTheCause) {
  build();
  const auto pid = launch("Plain");
  addIntention(pid, 1, std::nullopt,
               waitingRecord(kWaitKindEffect, 7, nowMs() + 5000));
  stage(pid, effectEntry(pid, 7, "move", nowMs() + 5000, 1));
  wake(pid);
  motors->finish("move", EffectResult::failure("stalled"));
  exec->pumpUntilIdle();
  const auto waiting = waitingOf(pid, 1);
  ASSERT_TRUE(waiting);
  const auto &outcome =
      std::get<WireRecord>(waiting->find(kWaitingMatch)->data);
  EXPECT_EQ(*outcome.find(kEffectOutcomeOk), WireValue::boolean(false));
  EXPECT_EQ(*outcome.find(kEffectOutcomeCause), WireValue::text("stalled"));
  EXPECT_TRUE(heard.empty()) << "a plan's failure is not an event";
}

TEST_F(VagentEffects, AHandlersFailureRaisesEffectFailedAndSuccessIsSilent) {
  build();
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "move", nowMs() + 5000));
  wake(pid);
  motors->finish("move", EffectResult::success());
  exec->pumpUntilIdle();
  EXPECT_TRUE(heard.empty());
  stage(pid, effectEntry(pid, 2, "move", nowMs() + 5000));
  wake(pid);
  motors->finish("move", EffectResult::failure("jammed"));
  exec->pumpUntilIdle();
  ASSERT_EQ(heard.size(), 1U);
  EXPECT_EQ(heard[0].type, kEventEffectFailed);
  EXPECT_EQ(*heard[0].find(kEffectFailedCapacity), WireValue::text("Drive"));
  EXPECT_EQ(*heard[0].find(kEffectFailedMethod), WireValue::text("move"));
  EXPECT_EQ(*heard[0].find(kEffectFailedCause), WireValue::text("jammed"));
}

TEST_F(VagentEffects, AnUnboundSkillFailsWithCauseUnbound) {
  build();
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "scan", nowMs() + 5000, std::nullopt, false,
                         "Rover.Lidar", "Scan"));
  wake(pid);
  exec->pumpUntilIdle();
  ASSERT_EQ(heard.size(), 1U);
  EXPECT_EQ(*heard[0].find(kEffectFailedCause),
            WireValue::text(std::string(kEffectCauseUnbound)));
}

TEST_F(VagentEffects, ASharedDeviceRefusesTheSecondEffectAsBusy) {
  build();
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "move", nowMs() + 5000));
  stage(pid, effectEntry(pid, 2, "turn", nowMs() + 5000));
  wake(pid);
  EXPECT_EQ(log, (std::vector<std::string>{"start:move"}));
  exec->pumpUntilIdle();
  ASSERT_EQ(heard.size(), 1U);
  EXPECT_EQ(*heard[0].find(kEffectFailedMethod), WireValue::text("turn"));
  EXPECT_EQ(*heard[0].find(kEffectFailedCause),
            WireValue::text(std::string(kEffectCauseBusy)));
  EXPECT_EQ(exec->effectsInFlight(pid), 1U);
}

TEST_F(VagentEffects, AQueueingDeviceSerialisesInArrivalOrder) {
  BindingOptions queueing;
  queueing.contention = Contention::Queue;
  build({}, queueing);
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "move", nowMs() + 5000));
  stage(pid, effectEntry(pid, 2, "turn", nowMs() + 5000));
  wake(pid);
  EXPECT_EQ(log, (std::vector<std::string>{"start:move"}));
  EXPECT_EQ(bindings.queued(kMotors), 1U);
  motors->finish("move", EffectResult::success());
  EXPECT_EQ(log, (std::vector<std::string>{"start:move", "start:turn"}));
  exec->pumpUntilIdle();
  EXPECT_TRUE(heard.empty());
  EXPECT_EQ(exec->effectsInFlight(pid), 1U);
}

TEST_F(VagentEffects, APreemptingEffectEndsTheOneInFlightAsPreempted) {
  build();
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "move", nowMs() + 5000));
  wake(pid);
  stage(pid, effectEntry(pid, 2, "halt", nowMs() + 5000, std::nullopt, true));
  wake(pid);
  EXPECT_EQ(log, (std::vector<std::string>{"start:move", "cancel:move",
                                           "start:halt"}));
  exec->pumpUntilIdle();
  ASSERT_EQ(heard.size(), 1U);
  EXPECT_EQ(*heard[0].find(kEffectFailedMethod), WireValue::text("move"));
  EXPECT_EQ(*heard[0].find(kEffectFailedCause),
            WireValue::text(std::string(kEffectCausePreempted)));
  // The device answering the preempted effect late changes nothing.
  motors->finish("move", EffectResult::success());
  exec->pumpUntilIdle();
  EXPECT_EQ(heard.size(), 1U);
  EXPECT_EQ(exec->stat(pid, kStatEffectStale), 0U);
  EXPECT_EQ(exec->effectsInFlight(pid), 1U);
}

TEST_F(VagentEffects, ABindingCanDeclareWhichMethodsPreempt) {
  BindingOptions options;
  options.preempting = {"halt"};
  build({}, options);
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "move", nowMs() + 5000));
  wake(pid);
  EXPECT_FALSE(bindings.preempts(kArm, "halt"));
  EXPECT_TRUE(bindings.preempts(kMotors, "halt"));
  // The entry does not say `preempts`; the binding does.
  stage(pid, effectEntry(pid, 2, "halt", nowMs() + 5000));
  wake(pid);
  EXPECT_EQ(log, (std::vector<std::string>{"start:move", "cancel:move",
                                           "start:halt"}));
}

TEST_F(VagentEffects, ACancelStopsTheEffectAndItsLateResultIsIgnored) {
  build();
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "move", nowMs() + 5000));
  wake(pid);
  stage(pid, cancelEntry(pid, 1));
  wake(pid);
  EXPECT_EQ(log, (std::vector<std::string>{"start:move", "cancel:move"}));
  EXPECT_EQ(exec->effectsInFlight(pid), 0U);
  EXPECT_EQ(bindings.running(kMotors), 0U);
  motors->finish("move", EffectResult::success());
  exec->pumpUntilIdle();
  EXPECT_TRUE(heard.empty());
  EXPECT_EQ(exec->stat(pid, kStatEffectStale), 0U);
  // The device is free again.
  stage(pid, effectEntry(pid, 2, "turn", nowMs() + 5000));
  wake(pid);
  EXPECT_EQ(log.back(), "start:turn");
}

TEST_F(VagentEffects, ACancelOfAnUnknownEffectIsHarmless) {
  build();
  const auto pid = launch("Plain");
  stage(pid, cancelEntry(pid, 99));
  wake(pid);
  EXPECT_TRUE(log.empty());
  EXPECT_EQ(exec->stat(pid, kStatMalformed), 0U);
}

TEST_F(VagentEffects, AMalformedEffectEntryIsCountedAndDropped) {
  build();
  const auto pid   = launch("Plain");
  auto &c          = ctx(pid);
  const auto entry = c.arena.makeCell(std::string_view(kKindEffect));
  setField(c.arena, valueDims(c.core), entry, kOutboxEffectId,
           c.arena.makeScalarCell(std::int64_t{1}));
  stage(pid, entry);
  wake(pid);
  EXPECT_TRUE(log.empty());
  EXPECT_EQ(exec->stat(pid, kStatMalformed), 1U);
}

TEST_F(VagentEffects, AnEffectPastItsDeadlineIsCancelledAndWakesItsIntention) {
  build();
  const auto pid = launch("Plain");
  addIntention(pid, 1, std::nullopt,
               waitingRecord(kWaitKindEffect, 7, nowMs() + 1000));
  stage(pid, effectEntry(pid, 7, "move", nowMs() + 1000, 1));
  wake(pid);
  EXPECT_EQ(exec->nextDeadline(pid), nowMs() + 1000);
  now += 999ms;
  exec->tick(now);
  exec->pumpUntilIdle();
  EXPECT_TRUE(exec->readyIntentions(pid).empty());
  now += 2ms;
  exec->tick(now);
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->readyIntentions(pid), (std::vector<std::int64_t>{1}));
  EXPECT_EQ(log, (std::vector<std::string>{"start:move", "cancel:move"}));
  const auto waiting = waitingOf(pid, 1);
  ASSERT_TRUE(waiting);
  EXPECT_EQ(*waiting->find(kWaitingTimedOut), WireValue::boolean(true));
  const auto &outcome =
      std::get<WireRecord>(waiting->find(kWaitingMatch)->data);
  EXPECT_EQ(*outcome.find(kEffectOutcomeCause),
            WireValue::text(std::string(kEffectCauseDeadline)));
  // The device's late answer is dropped quietly: the effect was cancelled.
  motors->finish("move", EffectResult::success());
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->stat(pid, kStatEffectStale), 0U);
}

TEST_F(VagentEffects, AHandlersEffectPastItsDeadlineRaisesEffectFailed) {
  build();
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "move", nowMs() + 500));
  wake(pid);
  now += 501ms;
  exec->tick(now);
  exec->pumpUntilIdle();
  ASSERT_EQ(heard.size(), 1U);
  EXPECT_EQ(*heard[0].find(kEffectFailedCause),
            WireValue::text(std::string(kEffectCauseDeadline)));
  EXPECT_EQ(exec->effectsInFlight(pid), 0U);
}

TEST_F(VagentEffects, AResultForAnIntentionNoLongerWaitingIsCountedStale) {
  build();
  const auto pid = launch("Plain");
  // The intention moved on to wait for something else.
  addIntention(pid, 1, std::nullopt,
               waitingRecord(kWaitKindAwaitEvent, 99, std::nullopt));
  stage(pid, effectEntry(pid, 7, "move", nowMs() + 5000, 1));
  wake(pid);
  motors->finish("move", EffectResult::success());
  exec->pumpUntilIdle();
  EXPECT_TRUE(exec->readyIntentions(pid).empty());
  EXPECT_EQ(exec->stat(pid, kStatEffectStale), 1U);
}

TEST_F(VagentEffects, EffectsAreCancelledWhenTheirAgentStops) {
  build();
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "move", nowMs() + 5000));
  wake(pid);
  ASSERT_TRUE(exec->retire(pid));
  EXPECT_EQ(log, (std::vector<std::string>{"start:move", "cancel:move"}));
  EXPECT_EQ(bindings.running(kMotors), 0U);
}

TEST_F(VagentEffects, ACheckpointIsRefusedWhileAnEffectRuns) {
  build();
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "move", nowMs() + 5000));
  wake(pid);
  EXPECT_EQ(exec->checkpoint(pid).error(), CheckpointRefusal::EffectInFlight);
  motors->finish("move", EffectResult::success());
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->checkpoint(pid).error(), CheckpointRefusal::NoIdentity);
}

// -- The drain (§4.2 step 9) -------------------------------------------------

TEST_F(VagentEffects, DrainOrderIsMessagesThenCancelsThenPreemptingThenOthers) {
  build();
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "grip", nowMs() + 5000, std::nullopt, false,
                         kArm, "Grasp"));
  wake(pid);
  log.clear();
  order.clear();
  // Staged in the worst order: the plain effect first, the cancel last.
  stage(pid, effectEntry(pid, 2, "lift", nowMs() + 5000, std::nullopt, false,
                         kArm, "Grasp"));
  stage(pid, eventEntry(pid, "Note"));
  stage(pid, effectEntry(pid, 3, "brake", nowMs() + 5000, std::nullopt, true));
  stage(pid, cancelEntry(pid, 1));
  wake(pid);
  // Merged into one story: the event went out, then the cancel freed the arm,
  // the preempting effect started, and the plain one started last (and so
  // found the arm free, which is why the cancel comes before it).
  std::vector<std::string> story = order;
  story.insert(story.end(), log.begin(), log.end());
  EXPECT_EQ(story, (std::vector<std::string>{"event:Note", "cancel:grip",
                                             "start:brake", "start:lift"}));
  exec->pumpUntilIdle();
  EXPECT_TRUE(heard.empty()) << "the lift was not refused";
}

// -- Percepts (§7.5) ---------------------------------------------------------

TEST_F(VagentEffects, APerceptIsAHostDeliveryWithNoMailboxSlotAndNoSource) {
  build();
  const auto pid = launch("Plain");
  WireRecord fields;
  fields.set("level", WireValue::integer(55));
  exec->percept(pid, "Battery", fields);
  EXPECT_EQ(runtime->pending(pid), 0U);
  exec->pumpUntilIdle();
  ASSERT_EQ(heard.size(), 1U);
  EXPECT_EQ(heard[0].type, "Battery");
  EXPECT_EQ(*heard[0].find("level"), WireValue::integer(55));
  const auto *source = heard[0].find(kEventImplicitSource);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(*source, WireValue::list({}));
  EXPECT_EQ(counter(pid, "msgs"), 1);
}

TEST_F(VagentEffects, PerceptsAreAllDeliveredBeforeACheckpointIsAccepted) {
  build();
  const auto pid = launch("Plain");
  exec->percept(pid, "Battery");
  exec->percept(pid, "Battery");
  EXPECT_EQ(exec->checkpoint(pid).error(),
            CheckpointRefusal::HostDeliveryPending);
  exec->pumpUntilIdle();
  EXPECT_EQ(heard.size(), 2U);
  // Past the delivery check, the next thing in the way is the missing identity.
  EXPECT_EQ(exec->checkpoint(pid).error(), CheckpointRefusal::NoIdentity);
}

TEST_F(VagentEffects, APerceptPostedBeforeTheFirstTurnIsHeardAfterInitialise) {
  build();
  const auto pid = exec->launch("Plain", "a");
  ASSERT_TRUE(pid);
  exec->percept(*pid, "Battery");
  exec->tick(now);
  exec->pumpUntilIdle();
  EXPECT_EQ(heard.size(), 1U);
  EXPECT_EQ(counter(*pid, "inits"), 1);
}

TEST_F(VagentEffects, PerceptsAndFailuresKeepTheirArrivalOrder) {
  build();
  const auto pid = launch("Plain");
  stage(pid, effectEntry(pid, 1, "move", nowMs() + 5000));
  wake(pid);
  exec->percept(pid, "First");
  motors->finish("move", EffectResult::failure("jammed"));
  exec->percept(pid, "Third");
  exec->pumpUntilIdle();
  ASSERT_EQ(heard.size(), 3U);
  EXPECT_EQ(heard[0].type, "First");
  EXPECT_EQ(heard[1].type, kEventEffectFailed);
  EXPECT_EQ(heard[2].type, "Third");
}

TEST_F(VagentEffects, APerceptForAnUnknownAgentIsDropped) {
  build();
  const auto pid = launch("Plain");
  exec->percept(ActorPid{"node", "nobody", 1, 99}, "Battery");
  exec->pumpUntilIdle();
  EXPECT_TRUE(heard.empty());
  EXPECT_EQ(exec->checkpoint(pid).error(), CheckpointRefusal::NoIdentity);
}

// -- The ready list and the wake turn (§5.6, §6.5) ---------------------------

TEST_F(VagentEffects, TheReadyListIsOrderedByReadyThenByWalkOrder) {
  build();
  const auto pid = launch("Plain");
  const auto a   = addIntention(pid, 1, 9);
  addIntention(pid, 2, 5, std::nullopt, a);
  addIntention(pid, 3, 5);
  addIntention(pid, 4, std::nullopt);
  addIntention(pid, 5, 7);
  // Walk order is 1, 2 (its subgoal), 3, 4, 5. Ready 5: 2 then 3 (walk order
  // breaks the tie); then 7; then 9.
  EXPECT_EQ(exec->readyIntentions(pid),
            (std::vector<std::int64_t>{2, 3, 5, 1}));
}

TEST_F(VagentEffects, WakeTurnsContinueTheOldestReadyIntentionOnePerTurn) {
  build();
  const auto pid = launch("Worker");
  std::vector<std::int64_t> chosen;
  std::vector<std::int64_t> inTurnRecord;
  exec->setIntentionObserver([&](const ActorPid &p, const WorkerContext &c,
                                 std::int64_t id) {
    chosen.push_back(id);
    const auto vd   = valueDims(c.core);
    const auto turn = fieldValue(c.arena, vd, *exec->agentCell(p), kFieldTurn);
    ASSERT_TRUE(turn);
    const auto value = readValue(c.arena, vd, *turn);
    ASSERT_TRUE(value);
    const auto *record = std::get_if<WireRecord>(&value->data);
    ASSERT_NE(record, nullptr);
    const auto *named = record->find(kTurnIntention);
    ASSERT_NE(named, nullptr);
    inTurnRecord.push_back(std::get<std::int64_t>(named->data));
  });
  addIntention(pid, 10, 30);
  addIntention(pid, 11, 20);
  addIntention(pid, 12, 25);
  exec->pumpUntilIdle();
  EXPECT_EQ(chosen, (std::vector<std::int64_t>{11, 12, 10}));
  EXPECT_EQ(inTurnRecord, chosen) << "$turn.intention names the intention";
  EXPECT_EQ(counter(pid, "continued"), 3);
  EXPECT_TRUE(exec->readyIntentions(pid).empty())
      << "the adapter takes the ready mark before the turn";
}

TEST_F(VagentEffects, AtMostReadyPerMessageWakeTurnsRunBetweenTwoMessageTurns) {
  AgentSettings settings;
  settings.readyPerMessage = 2;
  build(settings);
  const auto pid = launch("Worker");
  exec->setIntentionObserver(
      [&](const ActorPid &, const WorkerContext &, std::int64_t id) {
        order.push_back("W" + std::to_string(id));
      });
  for (std::int64_t id = 1; id <= 5; ++id) addIntention(pid, id, id);
  for (const auto *name : {"m1", "m2", "m3"}) {
    ASSERT_TRUE(runtime->receive(owner, external(pid, name), ActorTime{}, now));
  }
  exec->pumpUntilIdle();
  EXPECT_EQ(order, (std::vector<std::string>{"M", "W1", "W2", "M", "W3", "W4",
                                             "M", "W5"}));
}

TEST_F(VagentEffects, WithNoMessagesWaitingEveryReadyIntentionRuns) {
  AgentSettings settings;
  settings.readyPerMessage = 1;
  build(settings);
  const auto pid = launch("Worker");
  for (std::int64_t id = 1; id <= 6; ++id) addIntention(pid, id, id);
  exec->pumpUntilIdle();
  EXPECT_EQ(counter(pid, "continued"), 6);
}

TEST_F(VagentEffects,
       ANonPositiveReadyPerMessageStarvesWakesWhileMessagesQueue) {
  AgentSettings settings;
  settings.readyPerMessage = 0;
  build(settings);
  const auto pid = launch("Worker");
  exec->setIntentionObserver(
      [&](const ActorPid &, const WorkerContext &, std::int64_t id) {
        order.push_back("W" + std::to_string(id));
      });
  addIntention(pid, 1, 1);
  for (const auto *name : {"m1", "m2"}) {
    ASSERT_TRUE(runtime->receive(owner, external(pid, name), ActorTime{}, now));
  }
  exec->pumpUntilIdle();
  EXPECT_EQ(order, (std::vector<std::string>{"M", "M", "W1"}));
}

TEST_F(VagentEffects, WithoutAContinueRoutineNoIntentionTurnRuns) {
  build();
  const auto pid = launch("Plain");
  addIntention(pid, 1, 1);
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->readyIntentions(pid), (std::vector<std::int64_t>{1}));
  EXPECT_EQ(counter(pid, "msgs"), 0);
}

TEST_F(VagentEffects, AHardFailureInAnIntentionTurnRunsFailStep) {
  build();
  const auto pid = launch("Brittle");
  addIntention(pid, 1, 1);
  exec->pumpUntilIdle();
  // The continue turn was undone, the recovery turn committed, and the
  // intention is not chosen again for ever.
  EXPECT_EQ(counter(pid, "continued"), 0);
  EXPECT_EQ(counter(pid, "failed_steps"), 1);
  EXPECT_EQ(exec->stat(pid, kStatDropped), 1U);
  EXPECT_TRUE(exec->readyIntentions(pid).empty());
}

TEST_F(VagentEffects, WithoutFailStepAHardFailureIsCounted) {
  build();
  const auto pid = launch("NoRecovery");
  addIntention(pid, 1, 1);
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->stat(pid, kStatIntentionFailed), 1U);
  EXPECT_TRUE(exec->readyIntentions(pid).empty());
}

// -- Deadlines (§5.6, §6.6) --------------------------------------------------

TEST_F(VagentEffects,
       AWaitingDeadlineMarksTheIntentionReadyWithATimeoutMarker) {
  build();
  const auto pid = launch("Plain");
  addIntention(pid, 1, std::nullopt,
               waitingRecord(kWaitKindAsk, 4, nowMs() + 500));
  addIntention(pid, 2, std::nullopt,
               waitingRecord(kWaitKindAwaitEvent, 5, nowMs() + 900));
  addIntention(pid, 3, std::nullopt,
               waitingRecord(kWaitKindYield, 6, std::nullopt));
  EXPECT_EQ(exec->nextDeadline(pid), nowMs() + 500);
  now += 499ms;
  exec->tick(now);
  EXPECT_TRUE(exec->readyIntentions(pid).empty());
  now += 1ms;
  exec->tick(now);
  EXPECT_EQ(exec->readyIntentions(pid), (std::vector<std::int64_t>{1}));
  const auto waiting = waitingOf(pid, 1);
  ASSERT_TRUE(waiting);
  EXPECT_EQ(*waiting->find(kWaitingTimedOut), WireValue::boolean(true));
  EXPECT_EQ(waiting->find(kWaitingMatch), nullptr)
      << "a wait that is not on an effect has no outcome to record";
  EXPECT_EQ(exec->nextDeadline(pid), nowMs() + 400);
  // An intention already ready is left alone, and one with no deadline is
  // never woken.
  now += 1s;
  exec->tick(now);
  EXPECT_EQ(exec->readyIntentions(pid), (std::vector<std::int64_t>{1, 2}));
}

TEST_F(VagentEffects, ADeadlineWakesAnIntentionThroughTheContinueRoutine) {
  build();
  const auto pid = launch("Worker");
  addIntention(pid, 1, std::nullopt,
               waitingRecord(kWaitKindAsk, 4, nowMs() + 500));
  now += 501ms;
  exec->tick(now);
  exec->pumpUntilIdle();
  EXPECT_EQ(counter(pid, "continued"), 1);
}

// -- Resume (§4.5) -----------------------------------------------------------

constexpr const char *kTrivial = R"vagent(
agent Rover {
  var x: Int := 1
}
)vagent";

class VagentEffectsResume : public testing::Test {
protected:
  std::chrono::system_clock::time_point base =
      std::chrono::floor<std::chrono::seconds>(
          std::chrono::system_clock::now());
  std::unique_ptr<LocalHost> a;
  std::unique_ptr<LocalHost> b;

  std::unique_ptr<LocalHost> makeHost() {
    auto program = CompiledProgram::fromSource(kTrivial);
    EXPECT_TRUE(program) << (program ? "" : program.error());
    LocalHost::Options options;
    options.manualClock = true;
    const auto start    = base;
    options.clock.wall  = [start] { return start; };
    return std::make_unique<LocalHost>(std::move(*program), options);
  }

  void SetUp() override {
    a = makeHost();
    b = makeHost();
    a->connect(*b);
  }

  static CellRef addIntention(LocalHost &host, const ActorPid &pid,
                              std::int64_t id,
                              std::optional<WireRecord> waiting) {
    auto &c         = *host.executor().context(pid);
    const auto vd   = valueDims(c.core);
    const auto dims = registerDimensions(c.arena);
    const auto cell = c.arena.makeCell(std::string_view("intention"));
    setField(c.arena, vd, cell, kIntentionId, c.arena.makeScalarCell(id));
    if (waiting) {
      setField(c.arena, vd, cell, kIntentionWaiting,
               writeValue(c.arena, vd, WireValue::record(*waiting)));
    }
    appendToRank(c.arena, *host.executor().agentCell(pid), dims.intention,
                 cell);
    return cell;
  }
};

TEST_F(VagentEffectsResume, IntentionsWaitingOnAnEffectFailWithCauseMigrated) {
  const auto pid = a->launch("Rover", "r");
  ASSERT_TRUE(pid);
  a->pump();
  WireRecord onEffect;
  onEffect.type = "Waiting";
  onEffect.set(std::string(kWaitingKind),
               WireValue::text(std::string(kWaitKindEffect)));
  onEffect.set(std::string(kWaitingKey), WireValue::integer(7));
  WireRecord onAsk = onEffect;
  onAsk.set(std::string(kWaitingKind),
            WireValue::text(std::string(kWaitKindAsk)));
  addIntention(*a, *pid, 1, onEffect);
  addIntention(*a, *pid, 2, onAsk);
  addIntention(*a, *pid, 3, std::nullopt);

  const auto saved = a->executor().checkpoint(*pid);
  ASSERT_TRUE(saved) << toString(saved.error());
  const auto resumed = b->executor().resume(*saved);
  ASSERT_TRUE(resumed);
  b->pump();

  // Only the intention waiting on the device is failed; the rest travelled
  // as they were.
  EXPECT_EQ(b->executor().readyIntentions(*resumed),
            (std::vector<std::int64_t>{1}));
  auto &c          = *b->executor().context(*resumed);
  const auto vd    = valueDims(c.core);
  const auto dims  = registerDimensions(c.arena);
  std::size_t seen = 0;
  for (const auto top : zigzag::rankAfter(
           c.arena, *b->executor().agentCell(*resumed), dims.intention)) {
    ++seen;
    const auto id =
        c.arena.asInt64(*fieldValue(c.arena, vd, top, kIntentionId));
    const auto waiting = fieldValue(c.arena, vd, top, kIntentionWaiting);
    if (id != 1) {
      if (waiting) {
        const auto value = readValue(c.arena, vd, *waiting);
        ASSERT_TRUE(value);
        EXPECT_EQ(std::get<WireRecord>(value->data).find(kWaitingMatch),
                  nullptr);
      }
      continue;
    }
    ASSERT_TRUE(waiting);
    const auto value = readValue(c.arena, vd, *waiting);
    ASSERT_TRUE(value);
    const auto &match = std::get<WireRecord>(
        std::get<WireRecord>(value->data).find(kWaitingMatch)->data);
    EXPECT_EQ(*match.find(kEffectOutcomeOk), WireValue::boolean(false));
    EXPECT_EQ(*match.find(kEffectOutcomeCause),
              WireValue::text(std::string(kEffectCauseMigrated)));
  }
  EXPECT_EQ(seen, 3U);
}

TEST_F(VagentEffectsResume, ReadyIntentionsKeepTheirOrderAcrossAResume) {
  const auto pid = a->launch("Rover", "r");
  ASSERT_TRUE(pid);
  a->pump();
  for (const auto [id, ready] : {std::pair{1, 30}, {2, 10}, {3, 20}}) {
    const auto cell = addIntention(*a, *pid, id, std::nullopt);
    auto &c         = *a->executor().context(*pid);
    setField(c.arena, valueDims(c.core), cell, kIntentionReady,
             c.arena.makeScalarCell(std::int64_t{ready}));
  }
  ASSERT_EQ(a->executor().readyIntentions(*pid),
            (std::vector<std::int64_t>{2, 3, 1}));
  const auto saved = a->executor().checkpoint(*pid);
  ASSERT_TRUE(saved);
  const auto resumed = b->executor().resume(*saved);
  ASSERT_TRUE(resumed);
  b->pump();
  EXPECT_EQ(b->executor().readyIntentions(*resumed),
            (std::vector<std::int64_t>{2, 3, 1}));
}

// -- LocalHost -------------------------------------------------------------

TEST_F(VagentEffectsResume, ALocalHostOffersBindingsAndPercepts) {
  struct Echo final : NativeSkill {
    void start(const EffectRequest &request, EffectCompletion done) override {
      done(EffectResult::success(
          WireValue::integer(static_cast<std::int64_t>(request.args.size()))));
    }
  };
  a->bindings().bind("Rover.Echo", std::make_shared<Echo>());
  EXPECT_TRUE(a->bindings().bound("Rover.Echo"));
  const auto pid = a->launch("Rover", "r");
  ASSERT_TRUE(pid);
  a->percept(*pid, "Battery");
  EXPECT_EQ(a->pump(), 2U) << "the first turn, then the percept's wake";
}

} // namespace
} // namespace xanadu::vagent::runtime
