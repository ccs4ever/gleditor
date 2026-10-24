#include <gtest/gtest.h>

#include "common/xanadu/vagent/agent_executor.hpp"
#include "common/xanadu/vagent/wire.hpp"

#include "common/xanadu/zigzag/cell_views.hpp"

#include <functional>

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

// -- Wire
// ----------------------------------------------------------------------

WireValue sampleRecord() {
  WireRecord r;
  r.type = "Reading";
  r.set("level", WireValue::real(0.1));
  r.set("ok", WireValue::boolean(true));
  r.set("nothing", WireValue::unit());
  r.set("tags",
        WireValue::list({WireValue::text("a"), WireValue::integer(-7)}));
  return WireValue::record(std::move(r));
}

TEST(VagentExecutorWire, WireRoundTripsEveryValueKind) {
  const std::vector<WireValue> values{
      WireValue::integer(0),
      WireValue::integer(-123456789012LL),
      WireValue::real(3.141592653589793),
      WireValue::real(-0.0),
      WireValue::boolean(false),
      WireValue::boolean(true),
      WireValue::unit(),
      WireValue::text("hello"),
      WireValue::text(""),
      WireValue::list({}),
      WireValue::list(
          {WireValue::list({WireValue::integer(1)}), WireValue::unit()}),
      sampleRecord(),
  };
  for (const auto &value : values) {
    const auto bytes = encodeValue(value);
    ASSERT_TRUE(bytes);
    const auto back = decodeValue(*bytes);
    ASSERT_TRUE(back) << *bytes;
    // A record decodes with its fields in name order, so compare re-encodings.
    EXPECT_EQ(encodeValue(*back), bytes);
  }
  EXPECT_EQ(*encodeValue(WireValue::real(0.1)), "d1:f3:0.1e");
  EXPECT_EQ(*encodeValue(WireValue::boolean(true)), "d1:bi1ee");
  EXPECT_EQ(*encodeValue(WireValue::unit()), "d1:ui0ee");
}

TEST(VagentExecutorWire, WireRefusesMalformedDictionaries) {
  const std::vector<std::pair<std::string, WireError>> bad{
      {"i03e", WireError::NotCanonical},
      {"i-0e", WireError::NotCanonical},
      {"i1ex", WireError::Malformed},
      {"03:abc", WireError::NotCanonical},
      {"5:abc", WireError::Malformed},
      {"d1:bi1e1:ai0ee", WireError::NotCanonical},
      {"d1:ai0e1:ai0ee", WireError::NotCanonical},
      {"d1:xi0ee", WireError::UnknownKey},
      {"d1:bi2ee", WireError::NotCanonical},
      {"d1:ui1ee", WireError::NotCanonical},
      {"d1:f3:nane", WireError::BadNumber},
      {"d1:fi1ee", WireError::WrongType},
      {"d1:rd4:type1:Tee", WireError::UnknownKey},
      {"d1:rd6:fieldsde4:typei1eee", WireError::WrongType},
      {"l", WireError::Malformed},
      {"", WireError::Malformed},
  };
  for (const auto &[text, error] : bad) {
    const auto decoded = decodeValue(text);
    ASSERT_FALSE(decoded) << text;
    EXPECT_EQ(decoded.error(), error) << text;
  }
  EXPECT_EQ(decodeValue(std::string(100, 'l')).error(), WireError::TooDeep);
  EXPECT_FALSE(encodeValue(WireValue::real(std::nan(""))));
}

TEST(VagentExecutorWire, EnvelopeRoundTripsAndRefusesForgedImplicitFields) {
  Envelope e;
  e.kind    = "event";
  e.type    = "Ping";
  e.from    = "a/B#1";
  e.ordinal = 42;
  e.floor   = requestId(senderPrefix("a/B#1"), 3, 0);
  e.fields.set("n", WireValue::integer(1));
  e.correlation    = "c1";
  e.relaySpace     = "inner";
  e.relayFor       = {"x", "y"};
  const auto bytes = encodeEnvelope(e);
  ASSERT_TRUE(bytes);
  const auto back = decodeEnvelope(*bytes);
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, e);

  Envelope forged = e;
  forged.fields.set("source", WireValue::text("mallory"));
  EXPECT_EQ(encodeEnvelope(forged).error(), WireError::ImplicitField);
  EXPECT_FALSE(decodeEnvelope("d4:fromi1ee"));
  EXPECT_FALSE(decodeEnvelope("de"));
  EXPECT_FALSE(decodeEnvelope("i1e"));
  // relay_for without relay is refused.
  Envelope orphan = e;
  orphan.relaySpace.reset();
  EXPECT_FALSE(encodeEnvelope(orphan));
}

TEST(VagentExecutorWire, RequestIdsAreSixteenSixteenFourHex) {
  const auto prefix = senderPrefix("p/Child#1");
  EXPECT_EQ(prefix.size(), kRequestIdNameDigits);
  const auto id = requestId(prefix, 0x2a, 7);
  EXPECT_EQ(id.size(), kRequestIdLength);
  EXPECT_TRUE(isRequestId(id));
  EXPECT_EQ(id.substr(0, 16), prefix);
  EXPECT_EQ(id.substr(16, 16), "000000000000002a");
  EXPECT_EQ(id.substr(32), "0007");
  EXPECT_LT(requestId(prefix, 1, 9), requestId(prefix, 2, 0));
  EXPECT_FALSE(isRequestId("not-an-id"));
  EXPECT_NE(senderPrefix("a"), senderPrefix("b"));
}

TEST(VagentExecutorWire, PidTextRoundTrips) {
  const ActorPid pid{"node:1", std::string(64, 'c'), 9, 4};
  EXPECT_EQ(pidFromText(pidToText(pid)), pid);
  EXPECT_FALSE(pidFromText("a\x1f"
                           "b\x1f"
                           "0\x1f"
                           "1"));
  EXPECT_FALSE(pidFromText("garbage"));
}

// -- Cells
// ---------------------------------------------------------------------

TEST(VagentExecutorWire, ValuesRoundTripThroughCells) {
  WorkerContext ctx;
  const auto vd = valueDims(ctx.core);
  const std::vector<WireValue> values{
      WireValue::integer(-5),
      WireValue::real(2.5),
      WireValue::boolean(true),
      WireValue::unit(),
      WireValue::text("text"),
      WireValue::list({}),
      WireValue::list({WireValue::integer(1), WireValue::text("two"),
                       WireValue::list({WireValue::boolean(false)})}),
      sampleRecord(),
  };
  for (const auto &value : values) {
    const auto cell = writeValue(ctx.arena, vd, value);
    const auto back = readValue(ctx.arena, vd, cell);
    ASSERT_TRUE(back);
    EXPECT_EQ(encodeValue(*back), encodeValue(value));
  }
}

// -- Executor fixture
// ----------------------------------------------------------

/// A tiny assembler for hand-built routines.
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
  // Spin links give an opcode one successor and a cell one predecessor, so
  // routines cannot share the halt opcode as a successor; each ends in a jump
  // to it.
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
  CellRef eventEntry(std::string_view type, std::string_view space) {
    const auto vd    = valueDims(c.core);
    const auto entry = c.arena.makeCell(std::string_view(kKindEvent));
    WireRecord event;
    event.type = std::string(type);
    event.set("n", WireValue::integer(1));
    setField(c.arena, vd, entry, kOutboxEvent,
             writeValue(c.arena, vd, WireValue::record(std::move(event))));
    setField(c.arena, vd, entry, kOutboxSpace, c.arena.makeCell(space));
    return entry;
  }
};

class TestProgram final : public AgentProgram {
public:
  using Builder = std::function<CompiledAgent(WorkerContext &)>;
  std::map<std::string, Builder> types;
  int instantiations{};
  [[nodiscard]] std::string_view programRef() const override { return "prog"; }
  std::expected<CompiledAgent, std::string>
  instantiate(WorkerContext &ctx, std::string_view type) override {
    ++instantiations;
    const auto found = types.find(std::string(type));
    if (found == types.end()) return std::unexpected("unknown type");
    return found->second(ctx);
  }
};

class FakeIssuer final : public CapabilityIssuer {
public:
  std::vector<SpawnTicket> tickets;
  /// Refuses any ticket whose child name contains this text.
  std::string refuse;
  std::expected<IssuedCapability, ActorError>
  issue(const SpawnTicket &ticket) override {
    if (!refuse.empty() && ticket.childName.contains(refuse)) {
      return std::unexpected(ActorError::NotAuthorized);
    }
    tickets.push_back(ticket);
    return IssuedCapability{"cap:" + ticket.childName + ":" + ticket.parameters,
                            ""};
  }
  [[nodiscard]] std::optional<std::string>
  parametersOf(std::string_view capability) const override {
    // "cap:<name>:<bencode>"; a name holds no colon.
    constexpr std::string_view prefix = "cap:";
    if (!capability.starts_with(prefix)) return std::nullopt;
    const auto colon = capability.find(':', prefix.size());
    if (colon == std::string_view::npos) return std::nullopt;
    return std::string(capability.substr(colon + 1));
  }
};

const auto wallStart = std::chrono::system_clock::time_point(1000s);

class VagentExecutor : public testing::Test {
protected:
  Fingerprint owner = *Fingerprint::fromString(std::string(40, 'A'));
  std::chrono::system_clock::time_point now = wallStart;
  std::uint64_t maxInstructions             = 10000;
  FakeIssuer issuer;
  TestProgram program;
  std::unique_ptr<MeshActorRuntime> runtime;
  std::unique_ptr<AgentExecutor> exec;
  std::vector<ActorMessage> sent;
  /// Lease length by the capability text (which carries the child's name).
  std::function<std::chrono::seconds(const std::string &)> leaseOf =
      [](const std::string &) { return 1000s; };

  void SetUp() override {
    program.types["Counter"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Counter"));
      const auto inits = a.counterField(agent, "inits");
      const auto msgs  = a.counterField(agent, "msgs");
      return CompiledAgent{agent, a.chain({a.add(inits)}),
                           a.chain({a.add(msgs)}), a.halt, a.fail};
    };
    program.types["FailDispatch"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("FailDispatch"));
      const auto msgs  = a.counterField(agent, "msgs");
      return CompiledAgent{agent, a.chain({a.add(a.counterField(agent, "i"))}),
                           a.chain({a.add(msgs), a.failOpcode()}), a.halt,
                           a.fail};
    };
    program.types["Spin"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Spin"));
      const auto msgs  = a.counterField(agent, "msgs");
      const auto x     = a.add(msgs);
      const auto y     = a.add(msgs);
      zigzag::expectWritten(
          c.arena.link(x, c.core.dims().spin, zigzag::DimVector::POS, y));
      zigzag::expectWritten(
          c.arena.link(y, c.core.dims().spin, zigzag::DimVector::POS, x));
      return CompiledAgent{agent, a.chain({a.add(a.counterField(agent, "i"))}),
                           x, a.halt, a.fail};
    };
    program.types["InitFail"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("InitFail"));
      return CompiledAgent{agent, a.chain({a.failOpcode()}),
                           a.chain({a.add(a.counterField(agent, "msgs"))}),
                           a.halt, a.fail};
    };
    program.types["Child"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Child"));
      const auto msgs  = a.counterField(agent, "msgs");
      const auto ping  = a.eventEntry("Ping", kSpaceOuter);
      return CompiledAgent{agent,
                           a.chain({a.linkTo(agent, a.dims.outbox, ping)}),
                           a.chain({a.add(msgs)}), a.halt, a.fail};
    };
    program.types["Parent"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Parent"));
      const auto msgs  = a.counterField(agent, "msgs");
      const auto s1    = a.spawnEntry("Child", "p/Child#1");
      const auto s2    = a.spawnEntry("Child", "p/Child#2");
      return CompiledAgent{agent,
                           a.chain({a.linkTo(agent, a.dims.outbox, s1),
                                    a.linkTo(s1, a.dims.outbox, s2)}),
                           a.chain({a.add(msgs)}), a.halt, a.fail};
    };
    // Spawns a child and at once emits into its inner space.
    program.types["Talker"] = [](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view("Talker"));
      const auto s     = a.spawnEntry("Counter", "t/Counter#1");
      const auto e     = a.eventEntry("Ping", kSpaceInner);
      return CompiledAgent{agent,
                           a.chain({a.linkTo(agent, a.dims.outbox, s),
                                    a.linkTo(s, a.dims.outbox, e)}),
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
          if (request.programRef != "prog") {
            return std::unexpected(ActorError::NotAuthorized);
          }
          return ActorGrant{request.programRef,
                            "base",
                            maxInstructions,
                            10000,
                            32768,
                            100,
                            65536,
                            now + leaseOf(request.capability),
                            true};
        });
    AgentClock clock{.wall   = [this] { return now; },
                     .steady = [] { return ActorTime{}; }};
    exec = std::make_unique<AgentExecutor>(*runtime, issuer, program, owner,
                                           "node", 1, settings, clock);
    exec->install();
  }

  std::int64_t intField(const ActorPid &pid, std::string_view name) {
    auto *ctx        = exec->context(pid);
    const auto agent = exec->agentCell(pid);
    EXPECT_TRUE(ctx && agent);
    if (!ctx || !agent) return -1;
    const auto cell =
        fieldValue(ctx->arena, valueDims(ctx->core), *agent, name);
    EXPECT_TRUE(cell) << name;
    return cell ? ctx->arena.asInt64(*cell).value_or(-1) : -1;
  }

  std::optional<ActorPid> peer(const ActorPid &parent, std::string_view name) {
    auto *ctx        = exec->context(parent);
    const auto agent = exec->agentCell(parent);
    if (!ctx || !agent) return std::nullopt;
    const auto vd    = valueDims(ctx->core);
    const auto peers = fieldValue(ctx->arena, vd, *agent, kFieldPeers);
    if (!peers) return std::nullopt;
    const auto cell = fieldValue(ctx->arena, vd, *peers, name);
    return cell ? pidFromText(ctx->arena.textOf(*cell)) : std::nullopt;
  }

  ActorMessage external(const ActorPid &target, std::string id,
                        std::string type = "Ping") {
    Envelope e;
    e.kind    = "event";
    e.type    = std::move(type);
    e.ordinal = 1;
    e.floor   = "none";
    e.fields.set("n", WireValue::integer(1));
    ActorMessage m;
    m.type      = MessageType::ActorSend;
    m.requestId = std::move(id);
    m.source    = ActorPid{"node", "vagent-host", 1, 1};
    m.target    = target;
    m.value     = *encodeEnvelope(e);
    return m;
  }

  std::expected<void, ActorError> deliver(const ActorMessage &m) {
    return runtime->receive(owner, m, ActorTime{}, now);
  }
};

TEST_F(VagentExecutor, InitTurnRunsOnceAndSetsInitialized) {
  build();
  const auto pid = exec->launch("Counter", "c");
  ASSERT_TRUE(pid);
  exec->pumpUntilIdle();
  EXPECT_EQ(intField(*pid, "inits"), 1);
  EXPECT_EQ(intField(*pid, "msgs"), 0);
  auto *ctx       = exec->context(*pid);
  const auto flag = fieldValue(ctx->arena, valueDims(ctx->core),
                               *exec->agentCell(*pid), kFieldInitialized);
  ASSERT_TRUE(flag);
  EXPECT_TRUE(ctx->arena.asBool(*flag).value_or(false));
  // Later wakes dispatch and never initialise again.
  ASSERT_TRUE(runtime->wake(*pid, now));
  ASSERT_TRUE(runtime->wake(*pid, now));
  EXPECT_EQ(intField(*pid, "inits"), 1);
  EXPECT_EQ(intField(*pid, "msgs"), 2);
  EXPECT_EQ(program.instantiations, 1);
  EXPECT_EQ(intField(*pid, std::string(kFieldTurnCount)), 3);
}

TEST_F(VagentExecutor, MessageTurnInitialisesFirstThenWritesAField) {
  build();
  const auto pid = exec->launch("Counter", "c");
  ASSERT_TRUE(pid);
  ASSERT_TRUE(deliver(external(*pid, "m1")));
  ASSERT_EQ(runtime->pumpOne(now), true);
  EXPECT_EQ(intField(*pid, "inits"), 1);
  EXPECT_EQ(intField(*pid, "msgs"), 1);
  ASSERT_TRUE(deliver(external(*pid, "m2")));
  ASSERT_EQ(runtime->pumpOne(now), true);
  EXPECT_EQ(intField(*pid, "inits"), 1);
  EXPECT_EQ(intField(*pid, "msgs"), 2);
  EXPECT_EQ(exec->stat(*pid, kStatDropped), 0U);
}

TEST_F(VagentExecutor, MalformedEnvelopeIsDroppedAndCounted) {
  build();
  const auto pid = exec->launch("Counter", "c");
  ASSERT_TRUE(pid);
  auto bad  = external(*pid, "m1");
  bad.value = "d1:xi0ee";
  ASSERT_TRUE(deliver(bad));
  ASSERT_EQ(runtime->pumpOne(now), true);
  EXPECT_EQ(intField(*pid, "msgs"), 0);
  EXPECT_EQ(exec->stat(*pid, kStatMalformed), 1U);
}

TEST_F(VagentExecutor, FailingDispatchUndoesItsWritesAndIsCounted) {
  build();
  const auto pid = exec->launch("FailDispatch", "f");
  ASSERT_TRUE(pid);
  ASSERT_TRUE(deliver(external(*pid, "m1")));
  ASSERT_EQ(runtime->pumpOne(now), true);
  EXPECT_EQ(intField(*pid, "msgs"), 0);
  EXPECT_EQ(intField(*pid, "i"), 1);
  EXPECT_EQ(exec->stat(*pid, kStatDropped), 1U);
  EXPECT_EQ(exec->stat(*pid, std::string(kStatFailedPrefix) + "fail"), 1U);
  EXPECT_EQ(runtime->actorCount(), 1U);
  auto *ctx = exec->context(*pid);
  EXPECT_EQ(ctx->vm.choiceDepth(), 0U);
  EXPECT_EQ(ctx->arena.outstandingMarks(), 0U);
  // The actor is alive and keeps serving, counting each loss.
  ASSERT_TRUE(deliver(external(*pid, "m2")));
  ASSERT_EQ(runtime->pumpOne(now), true);
  EXPECT_EQ(exec->stat(*pid, kStatDropped), 2U);
}

TEST_F(VagentExecutor, CycleExhaustionIsAHardFailure) {
  maxInstructions = 60;
  build();
  const auto pid = exec->launch("Spin", "s");
  ASSERT_TRUE(pid);
  ASSERT_TRUE(deliver(external(*pid, "m1")));
  ASSERT_EQ(runtime->pumpOne(now), true);
  EXPECT_EQ(intField(*pid, "msgs"), 0);
  EXPECT_EQ(exec->stat(*pid, kStatDropped), 1U);
  EXPECT_EQ(runtime->actorCount(), 1U);
}

TEST_F(VagentExecutor, AFailureInTheInitialisingTurnStopsTheActor) {
  build();
  const auto pid = exec->launch("InitFail", "i");
  ASSERT_TRUE(pid);
  ASSERT_TRUE(runtime->wake(*pid, now).error() == ActorError::ExecutionFailed);
  EXPECT_EQ(runtime->actorCount(), 0U);
  EXPECT_EQ(runtime->wake(*pid, now).error(), ActorError::UnknownProcess);
  EXPECT_EQ(exec->context(*pid), nullptr);
}

TEST_F(VagentExecutor, ParentSpawnsChildrenWhoseOuterEmissionIsRelayed) {
  build();
  const auto parent = exec->launch("Parent", "p");
  ASSERT_TRUE(parent);
  exec->pumpUntilIdle();
  const auto c1 = peer(*parent, "p/Child#1");
  const auto c2 = peer(*parent, "p/Child#2");
  ASSERT_TRUE(c1 && c2);
  EXPECT_EQ(runtime->actorCount(), 3U);
  EXPECT_EQ(issuer.tickets.size(), 3U);
  // Each child emitted once to outer. The parent handled both itself and
  // relayed each to the sibling only, never back to the emitter.
  EXPECT_EQ(intField(*parent, "msgs"), 2);
  EXPECT_EQ(intField(*c1, "msgs"), 1);
  EXPECT_EQ(intField(*c2, "msgs"), 1);
  EXPECT_EQ(exec->stat(*parent, kStatUnroutable), 0U);
  // Every send was answered, so nothing is left incomplete.
  exec->tick(now);
  EXPECT_EQ(exec->outstanding(), 0U);
  EXPECT_EQ(runtime->deliveryCount(), 0U);
  // The children know their parent and their own names.
  auto *ctx              = exec->context(*c1);
  const auto vd          = valueDims(ctx->core);
  const auto agent       = *exec->agentCell(*c1);
  const auto parentField = fieldValue(ctx->arena, vd, agent, kFieldParent);
  ASSERT_TRUE(parentField);
  EXPECT_EQ(ctx->arena.textOf(*parentField), "p");
}

TEST_F(VagentExecutor, FloorsDropStaleIdsBeforeTheyRun) {
  build();
  const auto pid = exec->launch("Counter", "c");
  ASSERT_TRUE(pid);
  const auto prefix = senderPrefix("remote/Agent#1");
  const auto id     = [&](std::uint64_t turn) {
    return requestId(prefix, turn, 0);
  };
  const auto make = [&](std::uint64_t turn, std::uint64_t floorTurn) {
    Envelope e;
    e.kind    = "event";
    e.type    = "Ping";
    e.from    = "remote/Agent#1";
    e.ordinal = turn;
    e.floor   = id(floorTurn);
    auto m    = external(*pid, id(turn));
    m.value   = *encodeEnvelope(e);
    return m;
  };
  ASSERT_TRUE(deliver(make(1, 1)));
  ASSERT_EQ(runtime->pumpOne(now), true);
  // The second advances the floor past the first id.
  ASSERT_TRUE(deliver(make(5, 3)));
  ASSERT_EQ(runtime->pumpOne(now), true);
  EXPECT_EQ(intField(*pid, "msgs"), 2);
  EXPECT_EQ(deliver(make(2, 3)).error(), ActorError::StaleRequest);
  // At the floor is not below it.
  ASSERT_TRUE(deliver(make(3, 3)));
  ASSERT_EQ(runtime->pumpOne(now), true);
  EXPECT_EQ(intField(*pid, "msgs"), 3);
  // Ids queued before a floor advance still run (the mailbox is in order);
  // one sent below the advanced floor is refused when it arrives.
  ASSERT_TRUE(deliver(make(6, 3)));
  ASSERT_TRUE(deliver(make(9, 8)));
  ASSERT_EQ(runtime->pumpOne(now), true);
  ASSERT_EQ(runtime->pumpOne(now), true);
  EXPECT_EQ(deliver(make(7, 8)).error(), ActorError::StaleRequest);
  EXPECT_EQ(intField(*pid, "msgs"), 5);
}

TEST_F(VagentExecutor, FloorMarkReadsOnlyAnOwnFloor) {
  const auto prefix = senderPrefix("a");
  Envelope e;
  e.kind  = "event";
  e.type  = "T";
  e.floor = requestId(prefix, 4, 0);
  ActorMessage m;
  m.type          = MessageType::ActorSend;
  m.requestId     = requestId(prefix, 6, 2);
  m.value         = *encodeEnvelope(e);
  const auto mark = floorMarkOf(m);
  ASSERT_TRUE(mark);
  EXPECT_EQ(mark->prefix, prefix);
  EXPECT_EQ(mark->floorId, e.floor);
  e.floor = requestId(senderPrefix("other"), 4, 0);
  m.value = *encodeEnvelope(e);
  EXPECT_FALSE(floorMarkOf(m));
  m.requestId = "short";
  EXPECT_FALSE(floorMarkOf(m));
}

TEST_F(VagentExecutor, RedeliveryIsPacedAndAbandonedAfterTheRetries) {
  build();
  std::size_t sends = 0;
  runtime->setSender([&, loop = exec->loopbackSender()](
                         std::string_view node, const ActorMessage &message) {
    if (message.type == MessageType::ActorSend) ++sends;
    return loop(node, message);
  });
  const auto talker = exec->launch("Talker", "t");
  ASSERT_TRUE(talker);
  // Only the parent runs; its child never gets a turn, so never answers.
  ASSERT_TRUE(runtime->wake(*talker, now));
  EXPECT_EQ(sends, 1U);
  EXPECT_EQ(exec->outstanding(), 1U);
  exec->tick(now + 500ms);
  EXPECT_EQ(sends, 1U);
  for (int i = 1; i <= 5; ++i) {
    exec->tick(now + std::chrono::seconds(i));
    EXPECT_EQ(sends, static_cast<std::size_t>(1 + i)) << i;
  }
  EXPECT_EQ(exec->outstanding(), 1U);
  exec->tick(now + 6s);
  EXPECT_EQ(sends, 6U);
  EXPECT_EQ(exec->outstanding(), 0U);
  EXPECT_EQ(runtime->deliveryCount(), 0U);
  EXPECT_EQ(exec->stat(*talker, kStatDeliveryFailed), 1U);
}

TEST_F(VagentExecutor, ACompletedDeliveryIsForgottenNotRetried) {
  build();
  std::size_t sends = 0;
  runtime->setSender([&, loop = exec->loopbackSender()](
                         std::string_view node, const ActorMessage &message) {
    if (message.type == MessageType::ActorSend) ++sends;
    return loop(node, message);
  });
  const auto talker = exec->launch("Talker", "t");
  ASSERT_TRUE(talker);
  exec->pumpUntilIdle();
  const auto before = sends;
  exec->tick(now + 10s);
  EXPECT_EQ(sends, before);
  EXPECT_EQ(exec->outstanding(), 0U);
}

TEST_F(VagentExecutor, ChildParametersSurviveAFreshAdapterOverTheSameRuntime) {
  build();
  WireRecord args;
  args.set("k", WireValue::integer(7));
  const auto pid = exec->launch("Counter", "p/Counter#9", args);
  ASSERT_TRUE(pid);
  // A per-pid table in the first adapter would be lost here, as it is for a
  // child a remote parent spawned or one rebuilt by resume.
  AgentClock clock{.wall   = [this] { return now; },
                   .steady = [] { return ActorTime{}; }};
  exec = std::make_unique<AgentExecutor>(*runtime, issuer, program, owner,
                                         "node", 1, AgentSettings{}, clock);
  exec->install();
  ASSERT_TRUE(runtime->wake(*pid, now));
  EXPECT_EQ(intField(*pid, "k"), 7);
  EXPECT_EQ(intField(*pid, "inits"), 1);
  auto *ctx        = exec->context(*pid);
  const auto agent = exec->agentCell(*pid);
  ASSERT_TRUE(ctx && agent);
  const auto name =
      fieldValue(ctx->arena, valueDims(ctx->core), *agent, kFieldName);
  ASSERT_TRUE(name);
  EXPECT_EQ(ctx->arena.textOf(*name), "p/Counter#9");
}

// -- Lifecycle and delivery events
// ----------------------------------------------------

/// What an agent was told, read from the event cell while the turn runs.
struct Heard {
  ActorPid pid;
  std::string type;
  std::optional<std::string> source;
  std::map<std::string, std::string> text;
};

class VagentLifecycle : public VagentExecutor {
protected:
  std::vector<Heard> heard;

  void listen() {
    exec->setTurnObserver([this](const ActorPid &pid, const WorkerContext &c,
                                 CellRef event) {
      const auto vd = valueDims(c.core);
      Heard h{pid, c.arena.textOf(event), std::nullopt, {}};
      if (const auto s = fieldValue(c.arena, vd, event, kEventImplicitSource)) {
        if (const auto v = c.arena.linked(*s, vd.grab)) {
          h.source = c.arena.textOf(*v);
        }
      }
      for (const auto name : {"child", "cause", "to", "kind"}) {
        if (const auto f = fieldValue(c.arena, vd, event, name)) {
          h.text[name] = c.arena.textOf(*f);
        }
      }
      heard.push_back(std::move(h));
    });
  }
  [[nodiscard]] std::vector<Heard> heardBy(const ActorPid &pid,
                                           std::string_view type) const {
    std::vector<Heard> out;
    for (const auto &h : heard) {
      if (h.pid == pid && h.type == type) out.push_back(h);
    }
    return out;
  }
  /// A parent that spawns one child of `childType` and optionally stops it.
  void addMother(const std::string &type, const std::string &childType,
                 const std::string &childName, bool stopIt) {
    program.types[type] = [=](WorkerContext &c) {
      Asm a(c);
      const auto agent = c.arena.makeCell(std::string_view(type));
      const auto s     = a.spawnEntry(childType, childName);
      std::vector<CellRef> init{a.linkTo(agent, a.dims.outbox, s)};
      if (stopIt) {
        const auto vd   = valueDims(c.core);
        const auto stop = c.arena.makeCell(std::string_view(kKindStop));
        setField(c.arena, vd, stop, kOutboxStopChild,
                 c.arena.makeCell(std::string_view(childName)));
        init.push_back(a.linkTo(s, a.dims.outbox, stop));
      }
      return CompiledAgent{agent, a.chain(init),
                           a.chain({a.add(a.counterField(agent, "msgs"))}),
                           a.halt, a.fail};
    };
  }
  [[nodiscard]] bool inHolon(const ActorPid &pid, std::string_view name) {
    auto *ctx        = exec->context(pid);
    const auto agent = *exec->agentCell(pid);
    const auto dims  = registerDimensions(ctx->arena);
    for (const auto c : zigzag::rankAfter(ctx->arena, agent, dims.holon)) {
      if (ctx->arena.textOf(c) == name) return true;
    }
    return false;
  }
};

TEST_F(VagentLifecycle, ParentStopRunsDestroyAnnouncesAndRemovesTheChild) {
  addMother("Mother", "Counter", "m/Counter#1", true);
  build();
  listen();
  const auto mother = exec->launch("Mother", "m");
  ASSERT_TRUE(mother);
  exec->pumpUntilIdle();
  EXPECT_EQ(runtime->actorCount(), 1U);
  EXPECT_FALSE(peer(*mother, "m/Counter#1"));
  EXPECT_FALSE(inHolon(*mother, "m/Counter#1"));
  const auto stopping = heardBy(*mother, "ChildStopping");
  ASSERT_EQ(stopping.size(), 1U);
  EXPECT_EQ(stopping[0].text.at("child"), "m/Counter#1");
  EXPECT_EQ(stopping[0].text.at("cause"), "stop");
  // The ActorDown that follows is not a second report.
  EXPECT_TRUE(heardBy(*mother, "ChildDown").empty());
  std::size_t destroys = 0;
  for (const auto &h : heard) {
    if (h.type == "Destroy") {
      ++destroys;
      EXPECT_FALSE(h.source);
      EXPECT_NE(h.pid, *mother);
    }
  }
  EXPECT_EQ(destroys, 1U);
}

TEST_F(VagentLifecycle, AStopFromAnyoneButTheParentIsRefused) {
  build();
  listen();
  const auto pid = exec->launch("Counter", "c");
  ASSERT_TRUE(pid);
  auto m = external(*pid, "s1", "Stop");
  Envelope e;
  e.kind  = "stop";
  e.type  = "Stop";
  e.floor = "none";
  m.value = *encodeEnvelope(e);
  ASSERT_TRUE(deliver(m));
  ASSERT_EQ(runtime->pumpOne(now), true);
  EXPECT_EQ(runtime->actorCount(), 1U);
  EXPECT_EQ(exec->stat(*pid, kStatStopRefused), 1U);
  EXPECT_TRUE(heardBy(*pid, "Destroy").empty());
}

TEST_F(VagentLifecycle, TerminateRunsDestroyThenStops) {
  program.types["Quitter"] = [](WorkerContext &c) {
    Asm a(c);
    const auto agent = c.arena.makeCell(std::string_view("Quitter"));
    const auto flag  = c.arena.makeScalarCell(false);
    setField(c.arena, valueDims(c.core), agent, kFieldTerminating, flag);
    const auto yes = c.vm.mintOpcode(OpcodeKind::Not);
    c.core.bindInput(yes, flag);
    c.core.bindOutput(yes, flag);
    return CompiledAgent{agent, a.chain({a.add(a.counterField(agent, "i"))}),
                         a.chain({yes}), a.halt, a.fail};
  };
  addMother("Mother", "Quitter", "m/Quitter#1", false);
  build();
  listen();
  const auto mother = exec->launch("Mother", "m");
  ASSERT_TRUE(mother);
  exec->pumpUntilIdle();
  EXPECT_EQ(runtime->actorCount(), 2U);
  const auto child = peer(*mother, "m/Quitter#1");
  ASSERT_TRUE(child);
  // Dispatch sets $terminating; the next turn's settle ends the agent.
  ASSERT_TRUE(runtime->wake(*child, now));
  EXPECT_EQ(heardBy(*child, "Destroy").size(), 1U);
  // Erasure waits for the parent to answer the announcement.
  exec->pumpUntilIdle();
  EXPECT_EQ(runtime->actorCount(), 1U);
  const auto stopping = heardBy(*mother, "ChildStopping");
  ASSERT_EQ(stopping.size(), 1U);
  EXPECT_EQ(stopping[0].text.at("cause"), "terminate");
  EXPECT_FALSE(peer(*mother, "m/Quitter#1"));
}

TEST_F(VagentLifecycle, AChildGoneWithoutAnnouncementRaisesChildDown) {
  addMother("Mother", "Counter", "m/Counter#1", false);
  build();
  listen();
  const auto mother = exec->launch("Mother", "m");
  ASSERT_TRUE(mother);
  exec->pumpUntilIdle();
  const auto child = peer(*mother, "m/Counter#1");
  ASSERT_TRUE(child);
  ASSERT_TRUE(runtime->stop(*child));
  exec->pumpUntilIdle();
  const auto down = heardBy(*mother, "ChildDown");
  ASSERT_EQ(down.size(), 1U);
  EXPECT_EQ(down[0].text.at("child"), "m/Counter#1");
  EXPECT_FALSE(down[0].source);
  EXPECT_FALSE(peer(*mother, "m/Counter#1"));
  EXPECT_FALSE(inHolon(*mother, "m/Counter#1"));
  EXPECT_EQ(exec->stat(*mother, kStatChildDown), 1U);
  EXPECT_TRUE(heardBy(*mother, "ChildStopping").empty());
}

TEST_F(VagentLifecycle, ARefusedSpawnRaisesSpawnFailed) {
  addMother("Mother", "Counter", "m/Counter#1", false);
  issuer.refuse = "Counter";
  build();
  listen();
  const auto mother = exec->launch("Mother", "m");
  ASSERT_TRUE(mother);
  exec->pumpUntilIdle();
  const auto failed = heardBy(*mother, "SpawnFailed");
  ASSERT_EQ(failed.size(), 1U);
  EXPECT_EQ(failed[0].text.at("child"), "m/Counter#1");
  EXPECT_FALSE(failed[0].source);
  EXPECT_EQ(exec->stat(*mother, kStatSpawnFailed), 1U);
  EXPECT_EQ(runtime->actorCount(), 1U);
}

TEST_F(VagentLifecycle, AnAbandonedDeliveryRaisesDeliveryFailedAndWritesStats) {
  build();
  listen();
  const auto talker = exec->launch("Talker", "t");
  ASSERT_TRUE(talker);
  ASSERT_TRUE(runtime->wake(*talker, now));
  for (int i = 1; i <= 6; ++i) exec->tick(now + std::chrono::seconds(i));
  const auto failed = heardBy(*talker, "DeliveryFailed");
  ASSERT_EQ(failed.size(), 1U);
  EXPECT_EQ(failed[0].text.at("to"), "t/Counter#1");
  EXPECT_EQ(failed[0].text.at("kind"), "event");
  EXPECT_EQ(failed[0].text.at("cause"), "retries");
  EXPECT_FALSE(failed[0].source);
  // The agent was idle, yet its $stats cell already holds the count.
  auto *ctx     = exec->context(*talker);
  const auto vd = valueDims(ctx->core);
  const auto stats =
      fieldValue(ctx->arena, vd, *exec->agentCell(*talker), kFieldStats);
  ASSERT_TRUE(stats);
  const auto cell = fieldValue(ctx->arena, vd, *stats, kStatDeliveryFailed);
  ASSERT_TRUE(cell);
  EXPECT_EQ(ctx->arena.asInt64(*cell), 1);
}

TEST_F(VagentLifecycle, ADeadlineAbandonsBeforeTheRetriesAreSpent) {
  build();
  listen();
  const auto talker = exec->launch("Talker", "t");
  ASSERT_TRUE(talker);
  ASSERT_TRUE(runtime->wake(*talker, now));
  const auto id = requestId(senderPrefix("t"), 0, 1);
  ASSERT_TRUE(exec->setDeliveryDeadline(id, now + 2s));
  EXPECT_FALSE(exec->setDeliveryDeadline("nope", now));
  exec->tick(now + 3s);
  const auto failed = heardBy(*talker, "DeliveryFailed");
  ASSERT_EQ(failed.size(), 1U);
  EXPECT_EQ(failed[0].text.at("cause"), "deadline");
  EXPECT_EQ(exec->outstanding(), 0U);
}

TEST_F(VagentLifecycle, ALeaseIsRenewedAtTheFractionAndEndsWithoutRenewal) {
  leaseOf = [](const std::string &capability) {
    return capability.contains("Counter") ? 100s : 1000s;
  };
  addMother("Mother", "Counter", "m/Counter#1", false);
  build();
  listen();
  const auto mother = exec->launch("Mother", "m");
  ASSERT_TRUE(mother);
  exec->pumpUntilIdle();
  const auto child = peer(*mother, "m/Counter#1");
  ASSERT_TRUE(child);
  const auto issued = issuer.tickets.size();
  exec->tick(now + 79s);
  EXPECT_EQ(issuer.tickets.size(), issued);
  // At 90 s the lease is renewed (the authorizer reads the fixture's clock).
  now += 90s;
  exec->tick(now);
  EXPECT_EQ(issuer.tickets.size(), issued + 1);
  // The old expiry (100 s) passes without a stop.
  now += 20s;
  exec->tick(now);
  EXPECT_EQ(runtime->actorCount(), 2U);
  EXPECT_TRUE(heardBy(*child, "Destroy").empty());
  // Renewal now refused: the agent ends at its lease.
  issuer.refuse = "Counter";
  now += 200s;
  exec->tick(now);
  EXPECT_EQ(heardBy(*child, "Destroy").size(), 1U);
  exec->pumpUntilIdle();
  EXPECT_EQ(runtime->actorCount(), 1U);
  const auto stopping = heardBy(*mother, "ChildStopping");
  ASSERT_EQ(stopping.size(), 1U);
  EXPECT_EQ(stopping[0].text.at("cause"), "lease");
}

} // namespace
} // namespace xanadu::vagent
