/**
 * @file vagent_delegation_test.cpp
 * @brief The adapter's side of questions, delegation and awaiting (design
 *        §6.6, §7.1, §8.6, §12.4), driven with hand-built outbox entries and
 *        intention cells: the cells the compiler writes, in the shapes of the
 *        questions block of `runtime_abi.hpp`.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/vagent/agent_executor.hpp"
#include "common/xanadu/vagent/wire.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent {
namespace {

using identity::ActorPid;
using identity::Fingerprint;
using identity::MessageType;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;
using zigzag::vortex::mesh::ActorTime;
using zigzag::vortex::mesh::MeshActorRuntime;
using namespace std::chrono_literals;

// -- A tiny assembler, as in vagent_executor_test.cpp -----------------------

struct Asm {
  WorkerContext &c;
  AgentDims dims;
  CellRef halt{};
  CellRef fail{};

  explicit Asm(WorkerContext &ctx)
      : c(ctx), dims(registerDimensions(ctx.arena)) {
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
  CellRef linkTo(CellRef from, DimRef dim, CellRef to) {
    const auto op = c.vm.mintOpcode(OpcodeKind::Link);
    c.core.bindInput(op, from);
    c.core.bindInput(op, scalar(static_cast<std::int64_t>(dim)));
    c.core.bindInput(op, scalar(1));
    c.core.bindInput(op, to);
    return op;
  }
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
  CellRef spawnEntry(std::string_view type, std::string_view name) {
    const auto vd    = valueDims(c.core);
    const auto entry = c.arena.makeCell(std::string_view(kKindSpawn));
    setField(c.arena, vd, entry, kOutboxAgentType, c.arena.makeCell(type));
    setField(c.arena, vd, entry, kOutboxChildName, c.arena.makeCell(name));
    setField(c.arena, vd, entry, kOutboxArgs,
             writeValue(c.arena, vd, WireValue::record({})));
    return entry;
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

const auto wallStart = std::chrono::system_clock::time_point(1000s);

constexpr std::string_view kBoss      = "b";
constexpr std::string_view kAnswerer  = "b/Answerer#1";
constexpr std::string_view kMute      = "b/Mute#2";
constexpr std::string_view kFailing   = "b/Failing#3";
constexpr std::string_view kFetcher   = "b/Fetcher#4";
constexpr std::string_view kBystander = "b/Bystander#5";

/// An event an agent was handed: its type, who sent it and its fields.
struct Heard {
  ActorPid pid;
  WireRecord record;
};

class VagentDelegation : public testing::Test {
protected:
  Fingerprint owner = *Fingerprint::fromString(std::string(40, 'A'));
  std::chrono::system_clock::time_point now = wallStart;
  FakeIssuer issuer;
  TestProgram program;
  std::unique_ptr<MeshActorRuntime> runtime;
  std::unique_ptr<AgentExecutor> exec;
  std::vector<Heard> heard;
  /// The counters an `Answerer` bumps inside its answer turn.
  std::optional<ActorPid> boss;

  void SetUp() override {
    program.types["Boss"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Boss"));
      const auto s1    = a.spawnEntry("Answerer", kAnswerer);
      const auto s2    = a.spawnEntry("Mute", kMute);
      const auto s3    = a.spawnEntry("Failing", kFailing);
      const auto s4    = a.spawnEntry("Fetcher", kFetcher);
      const auto s5    = a.spawnEntry("Fetcher", kBystander);
      return CompiledAgent{agent,
                           a.chain({a.linkTo(agent, a.dims.outbox, s1),
                                    a.linkTo(s1, a.dims.outbox, s2),
                                    a.linkTo(s2, a.dims.outbox, s3),
                                    a.linkTo(s3, a.dims.outbox, s4),
                                    a.linkTo(s4, a.dims.outbox, s5)}),
                           a.chain({a.add(a.counterField(agent, "msgs"))}),
                           a.halt, a.fail};
    };
    // Answers 1 (the first `Add`) and writes a second counter, which a
    // read-only answer must not be able to keep.
    program.types["Answerer"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent  = c.arena.makeCell(std::string_view("Answerer"));
      const auto answer = a.counterField(agent, std::string(kFieldAnswer));
      const auto writes = a.counterField(agent, "writes");
      CompiledAgent compiled{
          agent, a.chain({a.add(a.counterField(agent, "inits"))}),
          a.chain({a.add(a.counterField(agent, "msgs"))}), a.halt, a.fail};
      compiled.answerEntry = a.chain({a.add(answer), a.add(writes)});
      return compiled;
    };
    // Has an answer routine that finds no `answer` for the question.
    program.types["Mute"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Mute"));
      CompiledAgent compiled{
          agent, a.chain({a.add(a.counterField(agent, "inits"))}),
          a.chain({a.add(a.counterField(agent, "msgs"))}), a.halt, a.fail};
      compiled.answerEntry =
          a.chain({a.add(a.counterField(agent, "answer_runs"))});
      return compiled;
    };
    program.types["Failing"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Failing"));
      CompiledAgent compiled{
          agent, a.chain({a.add(a.counterField(agent, "inits"))}),
          a.chain({a.add(a.counterField(agent, "msgs"))}), a.halt, a.fail};
      compiled.answerEntry = a.chain({a.failOpcode()});
      return compiled;
    };
    // Has no answer routine at all, so it is also the delegation target.
    program.types["Fetcher"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Fetcher"));
      return CompiledAgent{
          agent, a.chain({a.add(a.counterField(agent, "inits"))}),
          a.chain({a.add(a.counterField(agent, "msgs"))}), a.halt, a.fail};
    };
    // A delegate whose dispatcher fails on an `achieve`.
    program.types["Brittle"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Brittle"));
      return CompiledAgent{agent,
                           a.chain({a.add(a.counterField(agent, "inits"))}),
                           a.chain({a.failOpcode()}), a.halt, a.fail};
    };
    program.types["BrittleBoss"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("BrittleBoss"));
      const auto s     = a.spawnEntry("Brittle", "bb/Brittle#1");
      return CompiledAgent{agent, a.chain({a.linkTo(agent, a.dims.outbox, s)}),
                           a.chain({a.add(a.counterField(agent, "msgs"))}),
                           a.halt, a.fail};
    };
  }

  void build(AgentSettings settings = {}) {
    runtime = std::make_unique<MeshActorRuntime>("node", 1,
                                                 AgentExecutor::withFloors());
    runtime->setAuthorizer(
        [this](const Fingerprint &, const ActorMessage &request)
            -> std::expected<ActorGrant, ActorError> {
          return ActorGrant{
              request.programRef, "base", 10000, 10000, 32768, 100, 65536,
              now + 100000s,      true};
        });
    AgentClock clock{.wall   = [this] { return now; },
                     .steady = [] { return ActorTime{}; }};
    exec = std::make_unique<AgentExecutor>(*runtime, issuer, program, owner,
                                           "node", 1, settings, clock);
    exec->install();
    exec->setTurnObserver(
        [this](const ActorPid &pid, const WorkerContext &ctx, CellRef event) {
          const auto value = readValue(ctx.arena, valueDims(ctx.core), event);
          if (!value) return;
          if (const auto *record = std::get_if<WireRecord>(&value->data)) {
            heard.push_back(Heard{pid, *record});
          }
        });
  }

  /// Launch a boss and let it spawn its children.
  ActorPid launchBoss() {
    const auto pid = exec->launch("Boss", kBoss);
    EXPECT_TRUE(pid);
    boss = *pid;
    exec->pumpUntilIdle();
    return *pid;
  }

  [[nodiscard]] std::int64_t nowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch())
        .count();
  }

  WorkerContext &ctx(const ActorPid &pid) {
    auto *c = exec->context(pid);
    EXPECT_NE(c, nullptr);
    return *c;
  }
  CellRef agent(const ActorPid &pid) { return *exec->agentCell(pid); }

  ActorPid child(std::string_view name) {
    auto &c          = ctx(*boss);
    const auto vd    = valueDims(c.core);
    const auto peers = fieldValue(c.arena, vd, agent(*boss), kFieldPeers);
    EXPECT_TRUE(peers);
    const auto cell = fieldValue(c.arena, vd, *peers, name);
    EXPECT_TRUE(cell) << name;
    return *pidFromText(c.arena.textOf(*cell));
  }

  std::int64_t counter(const ActorPid &pid, std::string_view name) {
    auto &c         = ctx(pid);
    const auto cell = fieldValue(c.arena, valueDims(c.core), agent(pid), name);
    EXPECT_TRUE(cell) << name;
    return cell ? c.arena.asInt64(*cell).value_or(-1) : -1;
  }

  // -- Cells the compiler will one day write

  CellRef addIntention(const ActorPid &pid, std::int64_t id,
                       std::optional<WireRecord> waiting,
                       std::optional<WireRecord> delegator = std::nullopt) {
    auto &c         = ctx(pid);
    const auto vd   = valueDims(c.core);
    const auto dim  = registerDimensions(c.arena);
    const auto cell = c.arena.makeCell(std::string_view("intention"));
    setField(c.arena, vd, cell, kIntentionId, c.arena.makeScalarCell(id));
    if (waiting) {
      setField(c.arena, vd, cell, kIntentionWaiting,
               writeValue(c.arena, vd, WireValue::record(*waiting)));
    }
    if (delegator) {
      setField(c.arena, vd, cell, kIntentionDelegator,
               writeValue(c.arena, vd, WireValue::record(*delegator)));
    }
    appendToRank(c.arena, agent(pid), dim.intention, cell);
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

  static WireRecord delegatorRecord(std::string_view peer,
                                    std::string_view correlation) {
    WireRecord d;
    d.type = std::string(kDelegatorRecord);
    d.set(std::string(kDelegatorPeer), WireValue::text(std::string(peer)));
    d.set(std::string(kDelegatorCorr),
          WireValue::text(std::string(correlation)));
    return d;
  }

  void stage(const ActorPid &pid, CellRef entry) {
    auto &c = ctx(pid);
    appendToRank(c.arena, agent(pid), registerDimensions(c.arena).outbox,
                 entry);
  }

  CellRef requestEntry(const ActorPid &pid, std::string_view kind,
                       std::int64_t id, std::string_view to,
                       std::string_view name, std::int64_t deadline,
                       std::int64_t intention) {
    auto &c          = ctx(pid);
    const auto vd    = valueDims(c.core);
    const auto entry = c.arena.makeCell(kind);
    setField(c.arena, vd, entry, kOutboxReqId, c.arena.makeScalarCell(id));
    setField(c.arena, vd, entry, kOutboxReqTo, c.arena.makeCell(to));
    setField(c.arena, vd, entry, kOutboxReqName, c.arena.makeCell(name));
    WireRecord args;
    args.set(positionalName(0), WireValue::text("bolts"));
    args.set(positionalName(1), WireValue::integer(3));
    setField(c.arena, vd, entry, kOutboxReqArgs,
             writeValue(c.arena, vd, WireValue::record(std::move(args))));
    setField(c.arena, vd, entry, kOutboxReqDeadline,
             c.arena.makeScalarCell(deadline));
    setField(c.arena, vd, entry, kOutboxReqIntention,
             c.arena.makeScalarCell(intention));
    return entry;
  }

  CellRef answerEntry(const ActorPid &pid, std::string_view kind,
                      std::string_view to, std::string_view correlation,
                      std::string_view status = {},
                      std::string_view cause  = {}) {
    auto &c          = ctx(pid);
    const auto vd    = valueDims(c.core);
    const auto entry = c.arena.makeCell(kind);
    setField(c.arena, vd, entry, kOutboxReqTo, c.arena.makeCell(to));
    setField(c.arena, vd, entry, kOutboxCorrelation,
             c.arena.makeCell(correlation));
    if (kind == kKindOutcome) {
      setField(c.arena, vd, entry, kOutboxOutcomeStatus,
               c.arena.makeCell(status));
      if (!cause.empty()) {
        setField(c.arena, vd, entry, kOutboxOutcomeCause,
                 c.arena.makeCell(cause));
      }
    }
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

  /// `waiting.match` of intention @p id, which must be an outcome record.
  WireRecord matchOf(const ActorPid &pid, std::int64_t id) {
    const auto waiting = waitingOf(pid, id);
    EXPECT_TRUE(waiting);
    const auto *match = waiting ? waiting->find(kWaitingMatch) : nullptr;
    EXPECT_NE(match, nullptr);
    if (match == nullptr) return {};
    return std::get<WireRecord>(match->data);
  }

  [[nodiscard]] static std::string causeOf(const WireRecord &outcome) {
    const auto *cause = outcome.find(kEffectOutcomeCause);
    return cause ? std::get<std::string>(cause->data) : std::string{};
  }
  [[nodiscard]] static bool okOf(const WireRecord &outcome) {
    return std::get<bool>(outcome.find(kEffectOutcomeOk)->data);
  }

  [[nodiscard]] std::vector<Heard> heardBy(const ActorPid &pid,
                                           std::string_view type) const {
    std::vector<Heard> out;
    for (const auto &h : heard) {
      if (h.pid == pid && h.record.type == type) out.push_back(h);
    }
    return out;
  }

  /// A child's delegation goal arrives as this internal event.
  static std::string achieveType(std::string_view goal) {
    return std::string(kAchieveEventPrefix) + std::string(goal);
  }

  /// Stage a delegation to @p to and let the `achieve` reach it. Returns the
  /// correlation the adapter wrote into the waiting record.
  std::string delegate(std::string_view to, std::int64_t intention,
                       std::int64_t key, std::int64_t withinMs = 5000) {
    addIntention(*boss, intention,
                 waitingRecord(kWaitKindDelegate, key, nowMs() + withinMs));
    stage(*boss, requestEntry(*boss, kKindAchieve, key, to, "Fetch",
                              nowMs() + withinMs, intention));
    wake(*boss);
    exec->pumpUntilIdle();
    const auto waiting = waitingOf(*boss, intention);
    EXPECT_TRUE(waiting);
    const auto *corr = waiting ? waiting->find(kWaitingCorrelation) : nullptr;
    EXPECT_NE(corr, nullptr);
    return corr ? std::get<std::string>(corr->data) : std::string{};
  }

  ActorMessage crafted(const ActorPid &from, const ActorPid &to, std::string id,
                       Envelope envelope) {
    ActorMessage m;
    m.type      = MessageType::ActorSend;
    m.requestId = std::move(id);
    m.source    = from;
    m.target    = to;
    m.value     = *encodeEnvelope(envelope);
    return m;
  }
};

// -- Codec -------------------------------------------------------------------

TEST(VagentDelegationWire, ARecordAnswerIsNotMistakenForAFault) {
  WireRecord answer;
  answer.type = "Fault";
  answer.set("cause", WireValue::text("looks like one"));
  const auto bytes = encodeReply(Reply{WireValue::record(answer), {}});
  ASSERT_TRUE(bytes);
  const auto back = decodeReply(*bytes);
  ASSERT_TRUE(back);
  ASSERT_TRUE(back->value);
  EXPECT_EQ(*back->value, WireValue::record(answer));
  EXPECT_TRUE(back->fault.empty());

  const auto fault = decodeReply(*encodeReply(Reply{std::nullopt, "boom"}));
  ASSERT_TRUE(fault);
  EXPECT_FALSE(fault->value);
  EXPECT_EQ(fault->fault, "boom");
  EXPECT_FALSE(decodeReply(*encodeValue(WireValue::integer(1))));
}

// -- ask / answer ------------------------------------------------------------

TEST_F(VagentDelegation, AnAskIsAnsweredByTheReceiversTurnResultAndReadOnly) {
  build();
  launchBoss();
  const auto answerer = child(kAnswerer);
  addIntention(*boss, 1, waitingRecord(kWaitKindAsk, 7, nowMs() + 5000));
  stage(*boss, requestEntry(*boss, kKindAsk, 7, kAnswerer, "stock",
                            nowMs() + 5000, 1));
  wake(*boss);
  EXPECT_TRUE(exec->readyIntentions(*boss).empty())
      << "the question is only sent at the drain";
  exec->pumpUntilIdle();

  EXPECT_EQ(exec->readyIntentions(*boss), (std::vector<std::int64_t>{1}));
  const auto outcome = matchOf(*boss, 1);
  EXPECT_EQ(outcome.type, kEffectOutcomeRecord);
  EXPECT_TRUE(okOf(outcome));
  EXPECT_EQ(*outcome.find(kEffectOutcomeValue), WireValue::integer(1));
  // The answer turn ran and was undone: its second write did not survive,
  // and the receiver was not handed an event.
  EXPECT_EQ(counter(answerer, "writes"), 0);
  EXPECT_TRUE(heardBy(answerer, "stock").empty());
  EXPECT_EQ(counter(answerer, "msgs"), 0);
}

TEST_F(VagentDelegation, AnAnswerOfAnotherKindOfValueRidesTheSameWay) {
  build();
  launchBoss();
  // A second ask is answered afresh: the first answer left nothing behind.
  for (std::int64_t i = 1; i <= 2; ++i) {
    addIntention(*boss, i, waitingRecord(kWaitKindAsk, 10 + i, nowMs() + 5000));
    stage(*boss, requestEntry(*boss, kKindAsk, 10 + i, kAnswerer, "stock",
                              nowMs() + 5000, i));
    wake(*boss);
    exec->pumpUntilIdle();
    const auto outcome = matchOf(*boss, i);
    EXPECT_TRUE(okOf(outcome)) << i;
    EXPECT_EQ(*outcome.find(kEffectOutcomeValue), WireValue::integer(1)) << i;
  }
}

TEST_F(VagentDelegation, AReceiverWithNoAnswerRepliesUnanswered) {
  build();
  launchBoss();
  for (const auto &[who, key] :
       {std::pair{kMute, 21}, std::pair{kFetcher, 22}}) {
    addIntention(*boss, key, waitingRecord(kWaitKindAsk, key, nowMs() + 5000));
    stage(*boss, requestEntry(*boss, kKindAsk, key, who, "stock",
                              nowMs() + 5000, key));
    wake(*boss);
    exec->pumpUntilIdle();
    const auto outcome = matchOf(*boss, key);
    EXPECT_FALSE(okOf(outcome)) << who;
    EXPECT_EQ(causeOf(outcome), kFaultUnanswered) << who;
  }
  EXPECT_EQ(counter(child(kMute), "answer_runs"), 0)
      << "the routine ran, and its write was undone";
}

TEST_F(VagentDelegation, AReceiverThatFailsHardGivesARemoteFaultAndSurvives) {
  build();
  launchBoss();
  const auto failing = child(kFailing);
  addIntention(*boss, 1, waitingRecord(kWaitKindAsk, 7, nowMs() + 5000));
  stage(*boss,
        requestEntry(*boss, kKindAsk, 7, kFailing, "stock", nowMs() + 5000, 1));
  wake(*boss);
  exec->pumpUntilIdle();
  const auto outcome = matchOf(*boss, 1);
  EXPECT_FALSE(okOf(outcome));
  EXPECT_EQ(causeOf(outcome), kFaultRemote);
  EXPECT_EQ(exec->stat(failing, std::string(kStatFailedPrefix) + "fail"), 1U);
  EXPECT_TRUE(exec->context(failing) != nullptr) << "the receiver lives on";
}

TEST_F(VagentDelegation, AnAskToAnUnknownNameFailsAtOnce) {
  build();
  launchBoss();
  addIntention(*boss, 1, waitingRecord(kWaitKindAsk, 7, nowMs() + 5000));
  stage(*boss, requestEntry(*boss, kKindAsk, 7, "nobody/Here#9", "stock",
                            nowMs() + 5000, 1));
  wake(*boss);
  EXPECT_EQ(exec->readyIntentions(*boss), (std::vector<std::int64_t>{1}));
  const auto outcome = matchOf(*boss, 1);
  EXPECT_EQ(causeOf(outcome), kCauseUnroutable);
  EXPECT_EQ(exec->stat(*boss, kStatUnroutable), 1U);
}

TEST_F(VagentDelegation,
       AnAskPastItsDeadlineFailsTheStatementAndDropsALateReply) {
  build();
  launchBoss();
  addIntention(*boss, 1, waitingRecord(kWaitKindAsk, 7, nowMs() + 2000));
  stage(*boss, requestEntry(*boss, kKindAsk, 7, kAnswerer, "stock",
                            nowMs() + 2000, 1));
  wake(*boss);
  // The question is queued; the answerer has not yet run.
  now += 3s;
  exec->tick(now);
  EXPECT_EQ(exec->readyIntentions(*boss), (std::vector<std::int64_t>{1}));
  auto waiting = waitingOf(*boss, 1);
  ASSERT_TRUE(waiting);
  EXPECT_EQ(*waiting->find(kWaitingTimedOut), WireValue::boolean(true));
  const auto outcome = matchOf(*boss, 1);
  EXPECT_FALSE(okOf(outcome));
  EXPECT_EQ(causeOf(outcome), kEffectCauseDeadline);

  exec->pumpUntilIdle();
  // Whatever arrives afterwards changes nothing.
  EXPECT_EQ(causeOf(matchOf(*boss, 1)), kEffectCauseDeadline);
  EXPECT_EQ(exec->readyIntentions(*boss), (std::vector<std::int64_t>{1}));
}

TEST_F(VagentDelegation, AnAbandonedAskFailsTheWaitWithTheDeliveryCause) {
  AgentSettings settings;
  settings.sendRetries       = 1;
  settings.sendRetryInterval = 1s;
  build(settings);
  launchBoss();
  addIntention(*boss, 1, waitingRecord(kWaitKindAsk, 7, nowMs() + 600000));
  stage(*boss, requestEntry(*boss, kKindAsk, 7, kAnswerer, "stock",
                            nowMs() + 600000, 1));
  wake(*boss);
  // Never pumped, so never answered: redelivered once, then abandoned.
  now += 1s;
  exec->tick(now);
  now += 2s;
  exec->tick(now);
  EXPECT_EQ(exec->readyIntentions(*boss), (std::vector<std::int64_t>{1}));
  EXPECT_EQ(causeOf(matchOf(*boss, 1)), kCauseDelivery);
}

// -- achieve ... by ----------------------------------------------------------

TEST_F(VagentDelegation, AnAchieveReachesTheDelegateAsAnInternalEvent) {
  build();
  launchBoss();
  const auto fetcher = child(kFetcher);
  const auto corr    = delegate(kFetcher, 1, 5);
  ASSERT_FALSE(corr.empty());
  EXPECT_TRUE(isRequestId(corr)) << "the correlation is the request id";
  const auto events = heardBy(fetcher, achieveType("Fetch"));
  ASSERT_EQ(events.size(), 1U);
  const auto &record = events[0].record;
  EXPECT_EQ(*record.find(positionalName(0)), WireValue::text("bolts"));
  EXPECT_EQ(*record.find(positionalName(1)), WireValue::integer(3));
  EXPECT_EQ(*record.find(kEventCorrelation), WireValue::text(corr));
  // The sender is the implicit `source`, an Opt of its name.
  const auto *source = record.find(kEventImplicitSource);
  ASSERT_NE(source, nullptr);
  // Acknowledging the achieve is not the outcome: the wait goes on.
  EXPECT_TRUE(exec->readyIntentions(*boss).empty());
  EXPECT_EQ(counter(fetcher, "msgs"), 1);
}

TEST_F(VagentDelegation, AnOutcomeFromTheDelegateResumesTheDelegatingWait) {
  build();
  launchBoss();
  const auto fetcher = child(kFetcher);
  const auto corr    = delegate(kFetcher, 1, 5);
  // The delegate's compiled code adopts the goal, runs it and, at its end,
  // stages the outcome.
  addIntention(fetcher, 100, std::nullopt, delegatorRecord(kBoss, corr));
  stage(fetcher,
        answerEntry(fetcher, kKindOutcome, kBoss, corr, kOutcomeSucceeded));
  wake(fetcher);
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->readyIntentions(*boss), (std::vector<std::int64_t>{1}));
  const auto outcome = matchOf(*boss, 1);
  EXPECT_TRUE(okOf(outcome));
  const auto waiting = waitingOf(*boss, 1);
  EXPECT_EQ(waiting->find(kWaitingTimedOut), nullptr);
}

TEST_F(VagentDelegation, AFailedOutcomeCarriesItsCause) {
  build();
  launchBoss();
  const auto fetcher = child(kFetcher);
  const auto corr    = delegate(kFetcher, 1, 5);
  stage(fetcher, answerEntry(fetcher, kKindOutcome, kBoss, corr, kOutcomeFailed,
                             "no_stock"));
  wake(fetcher);
  exec->pumpUntilIdle();
  const auto outcome = matchOf(*boss, 1);
  EXPECT_FALSE(okOf(outcome));
  EXPECT_EQ(causeOf(outcome), "no_stock");
}

TEST_F(VagentDelegation, ARefusalFailsTheDelegatingWaitAtOnce) {
  build();
  launchBoss();
  const auto fetcher = child(kFetcher);
  const auto corr    = delegate(kFetcher, 1, 5);
  stage(fetcher, answerEntry(fetcher, kKindRefuse, kBoss, corr));
  wake(fetcher);
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->readyIntentions(*boss), (std::vector<std::int64_t>{1}));
  EXPECT_EQ(causeOf(matchOf(*boss, 1)), kCauseRefused);
}

TEST_F(VagentDelegation, AnOutcomeNobodyIsWaitingForIsDroppedAndCounted) {
  build();
  launchBoss();
  const auto fetcher = child(kFetcher);
  const auto corr    = delegate(kFetcher, 1, 5);
  // An outcome naming a correlation the delegator never sent.
  stage(fetcher, answerEntry(fetcher, kKindOutcome, kBoss, "nonsense",
                             kOutcomeSucceeded));
  wake(fetcher);
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->stat(*boss, kStatReplyStale), 1U);
  EXPECT_TRUE(exec->readyIntentions(*boss).empty());
  // The right one still works, and a second copy is stale.
  stage(fetcher,
        answerEntry(fetcher, kKindOutcome, kBoss, corr, kOutcomeSucceeded));
  stage(fetcher, answerEntry(fetcher, kKindOutcome, kBoss, corr, kOutcomeFailed,
                             "late"));
  wake(fetcher);
  exec->pumpUntilIdle();
  EXPECT_TRUE(okOf(matchOf(*boss, 1)));
  EXPECT_EQ(exec->stat(*boss, kStatReplyStale), 2U);
}

TEST_F(VagentDelegation, OnlyTheDelegateCanEndTheWait) {
  build();
  launchBoss();
  const auto fetcher   = child(kFetcher);
  const auto bystander = child(kBystander);
  const auto corr      = delegate(kFetcher, 1, 5);
  stage(bystander,
        answerEntry(bystander, kKindOutcome, kBoss, corr, kOutcomeSucceeded));
  wake(bystander);
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->stat(*boss, kStatReplyStale), 1U);
  EXPECT_TRUE(exec->readyIntentions(*boss).empty());
  (void)fetcher;
}

TEST_F(VagentDelegation, ADelegationPastItsDeadlineFailsTheWait) {
  build();
  launchBoss();
  const auto corr = delegate(kFetcher, 1, 5, 2000);
  ASSERT_FALSE(corr.empty());
  now += 3s;
  exec->tick(now);
  EXPECT_EQ(exec->readyIntentions(*boss), (std::vector<std::int64_t>{1}));
  const auto waiting = waitingOf(*boss, 1);
  EXPECT_EQ(*waiting->find(kWaitingTimedOut), WireValue::boolean(true));
  EXPECT_EQ(causeOf(matchOf(*boss, 1)), kEffectCauseDeadline);
  // A late outcome is dropped.
  const auto fetcher = child(kFetcher);
  stage(fetcher,
        answerEntry(fetcher, kKindOutcome, kBoss, corr, kOutcomeSucceeded));
  wake(fetcher);
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->stat(*boss, kStatReplyStale), 1U);
  EXPECT_EQ(causeOf(matchOf(*boss, 1)), kEffectCauseDeadline);
}

TEST_F(VagentDelegation, ADelegateThatFailsHardTellsTheDelegatorAtOnce) {
  build();
  const auto bb = exec->launch("BrittleBoss", "bb");
  ASSERT_TRUE(bb);
  boss = *bb;
  exec->pumpUntilIdle();
  auto &c          = ctx(*bb);
  const auto vd    = valueDims(c.core);
  const auto peers = fieldValue(c.arena, vd, agent(*bb), kFieldPeers);
  ASSERT_TRUE(peers);
  addIntention(*bb, 1, waitingRecord(kWaitKindDelegate, 5, nowMs() + 5000));
  stage(*bb, requestEntry(*bb, kKindAchieve, 5, "bb/Brittle#1", "Fetch",
                          nowMs() + 5000, 1));
  wake(*bb);
  exec->pumpUntilIdle();
  EXPECT_EQ(exec->readyIntentions(*bb), (std::vector<std::int64_t>{1}));
  EXPECT_EQ(causeOf(matchOf(*bb, 1)), kFaultRemote);
}

// -- cancel ------------------------------------------------------------------

TEST_F(VagentDelegation, ACancelAfterArrivalDropsTheAdoptedIntention) {
  build();
  launchBoss();
  const auto fetcher = child(kFetcher);
  const auto corr    = delegate(kFetcher, 1, 5);
  addIntention(fetcher, 100, std::nullopt, delegatorRecord(kBoss, corr));
  // The delegating routine, resumed early, stages the cancel.
  stage(*boss, answerEntry(*boss, kKindCancel, kFetcher, corr));
  wake(*boss);
  exec->pumpUntilIdle();
  const auto drops = heardBy(fetcher, kCancelEvent);
  ASSERT_EQ(drops.size(), 1U);
  EXPECT_EQ(*drops[0].record.find(kCancelIntention), WireValue::integer(100));
  EXPECT_TRUE(heardBy(fetcher, kCancelEvent).size() == 1U);
}

TEST_F(VagentDelegation, ACancelFromAnotherAgentIsRefused) {
  build();
  launchBoss();
  const auto fetcher   = child(kFetcher);
  const auto bystander = child(kBystander);
  const auto corr      = delegate(kFetcher, 1, 5);
  addIntention(fetcher, 100, std::nullopt, delegatorRecord(kBoss, corr));
  // A sibling cannot reach the fetcher by name, so it tries through the
  // protocol it does have: a crafted message from its own pid.
  Envelope cancel;
  cancel.kind        = std::string(kKindCancel);
  cancel.type        = std::string(kCancelType);
  cancel.from        = std::string(kBystander);
  cancel.floor       = "none";
  cancel.correlation = corr;
  ASSERT_TRUE(runtime->receive(
      owner, crafted(bystander, fetcher, std::string(36, 'c'), cancel),
      ActorTime{}, now));
  exec->pumpUntilIdle();
  EXPECT_TRUE(heardBy(fetcher, kCancelEvent).empty());
  EXPECT_EQ(exec->stat(fetcher, kStatReplyStale), 1U);
}

TEST_F(VagentDelegation,
       ACancelBeforeTheAchieveArrivesIsRememberedThenExpires) {
  AgentSettings settings;
  settings.sendRetries       = 3;
  settings.sendRetryInterval = 2s;
  build(settings);
  launchBoss();
  const auto fetcher = child(kFetcher);
  const std::string early(36, 'a');
  const std::string late(36, 'b');

  Envelope cancel;
  cancel.kind        = std::string(kKindCancel);
  cancel.type        = std::string(kCancelType);
  cancel.from        = std::string(kBoss);
  cancel.floor       = "none";
  const auto boss0   = *boss;
  cancel.correlation = early;
  ASSERT_TRUE(runtime->receive(
      owner, crafted(boss0, fetcher, std::string(36, '1'), cancel), ActorTime{},
      now));
  cancel.correlation = late;
  ASSERT_TRUE(runtime->receive(
      owner, crafted(boss0, fetcher, std::string(36, '2'), cancel), ActorTime{},
      now));
  exec->pumpUntilIdle();
  EXPECT_EQ(counter(fetcher, "msgs"), 0) << "a cancel is not an event";

  // The first id's achieve arrives inside the window and is ignored.
  Envelope achieve;
  achieve.kind  = std::string(kKindAchieve);
  achieve.type  = "Fetch";
  achieve.from  = std::string(kBoss);
  achieve.floor = "none";
  ASSERT_TRUE(runtime->receive(owner, crafted(boss0, fetcher, early, achieve),
                               ActorTime{}, now));
  exec->pumpUntilIdle();
  EXPECT_TRUE(heardBy(fetcher, achieveType("Fetch")).empty());
  EXPECT_EQ(exec->stat(fetcher, kStatCancelledEarly), 1U);

  // `send_retries x send_retry_interval` = 6 s later the other id is
  // forgotten, so an achieve with it is an ordinary one.
  now += 7s;
  ASSERT_TRUE(runtime->receive(owner, crafted(boss0, fetcher, late, achieve),
                               ActorTime{}, now));
  exec->pumpUntilIdle();
  EXPECT_EQ(heardBy(fetcher, achieveType("Fetch")).size(), 1U);
  EXPECT_EQ(exec->stat(fetcher, kStatCancelledEarly), 1U);
}

// -- await -------------------------------------------------------------------

TEST_F(VagentDelegation, AnAwaitPastItsDeadlineIsMarkedTimedOutWithNoMatch) {
  build();
  launchBoss();
  for (const auto &[id, kind] : {std::pair{1, kWaitKindAwaitBelief},
                                 std::pair{2, kWaitKindAwaitEvent}}) {
    auto waiting = waitingRecord(kind, id, nowMs() + 2000);
    waiting.set(std::string(kind == kWaitKindAwaitBelief ? kWaitingPattern
                                                         : kWaitingEventType),
                WireValue::text("delivered"));
    addIntention(*boss, id, waiting);
  }
  EXPECT_TRUE(exec->readyIntentions(*boss).empty());
  EXPECT_EQ(exec->nextDeadline(*boss), nowMs() + 2000);
  now += 3s;
  exec->tick(now);
  EXPECT_EQ(exec->readyIntentions(*boss), (std::vector<std::int64_t>{1, 2}));
  for (const std::int64_t id : {1, 2}) {
    const auto waiting = waitingOf(*boss, id);
    ASSERT_TRUE(waiting);
    EXPECT_EQ(*waiting->find(kWaitingTimedOut), WireValue::boolean(true));
    EXPECT_EQ(waiting->find(kWaitingMatch), nullptr);
  }
}

// -- tell beyond $peers ------------------------------------------------------

TEST_F(VagentDelegation, ATellReachesASiblingThroughTheSpaceTheyShare) {
  build();
  launchBoss();
  const auto answerer  = child(kAnswerer);
  const auto fetcher   = child(kFetcher);
  const auto bystander = child(kBystander);
  auto &c              = ctx(answerer);
  const auto vd        = valueDims(c.core);
  const auto entry     = c.arena.makeCell(std::string_view(kKindTell));
  WireRecord fact;
  fact.type = "delivered";
  fact.set(positionalName(0), WireValue::text("order-9"));
  setField(c.arena, vd, entry, kOutboxFact,
           writeValue(c.arena, vd, WireValue::record(fact)));
  setField(
      c.arena, vd, entry, kOutboxFor,
      writeValue(c.arena, vd,
                 WireValue::list({WireValue::text(std::string(kFetcher))})));
  stage(answerer, entry);
  wake(answerer);
  exec->pumpUntilIdle();
  const auto told =
      heardBy(fetcher, std::string(kTellEventPrefix) + "delivered");
  ASSERT_EQ(told.size(), 1U);
  EXPECT_EQ(*told[0].record.find(positionalName(0)),
            WireValue::text("order-9"));
  EXPECT_TRUE(
      heardBy(bystander, std::string(kTellEventPrefix) + "delivered").empty())
      << "only the named sibling hears it";
  EXPECT_TRUE(
      heardBy(*boss, std::string(kTellEventPrefix) + "delivered").empty());
}

TEST_F(VagentDelegation, ATellToAnUnknownNameIsUnroutable) {
  build();
  launchBoss();
  const auto answerer = child(kAnswerer);
  auto &c             = ctx(answerer);
  const auto vd       = valueDims(c.core);
  const auto entry    = c.arena.makeCell(std::string_view(kKindTell));
  WireRecord fact;
  fact.type = "delivered";
  fact.set(positionalName(0), WireValue::text("order-9"));
  setField(c.arena, vd, entry, kOutboxFact,
           writeValue(c.arena, vd, WireValue::record(fact)));
  setField(c.arena, vd, entry, kOutboxFor,
           writeValue(c.arena, vd,
                      WireValue::list({WireValue::text("elsewhere/X#1")})));
  stage(answerer, entry);
  wake(answerer);
  exec->pumpUntilIdle();
  for (const auto &h : heard) {
    EXPECT_NE(h.record.type, std::string(kTellEventPrefix) + "delivered");
  }
}

} // namespace
} // namespace xanadu::vagent
