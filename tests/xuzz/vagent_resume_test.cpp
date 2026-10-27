/**
 * @file vagent_resume_test.cpp
 * @brief Checkpoint, resume and migration of compiled vagent agents (design
 *        §4.2 quiescence, §4.4, §4.5, §5.5): state survives the move, the
 *        program is not initialised again, timers still fire, peers learn the
 *        new pid, and a checkpoint is refused unless the agent is quiescent.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using namespace std::chrono_literals;
using zigzag::vortex::mesh::ActorError;

constexpr const char *kProgram = R"vagent(
predicate color(c: Str)
event Bump { amount: Int }
event Paint { c: Str }
event Arm { }
event Ping { n: Int }
event Quit { }
event Poke { }
event Echo { who: Str }

agent Counter(start: Int) {
  var total: Int := 0
  var inits: Int := 0
  var seen: Int := 0
  var pings: Int := 0
  var tags: [Str] := []
  var label: Str := "fresh"
  on Initialize { total := start; inits := inits + 1; label := "set" }
  on Bump as b { total := total + b.amount; tags := tags ++ ["b"] }
  on Paint as p { believe color(p.c) }
  on +color(?c) { seen := seen + 1 }
  on Arm { Schedules.after(5s, Ping{n: 1}) }
  on Ping as p { pings := pings + p.n }
  on Quit { terminate }
}

agent Child {
  var got: Int := 0
  on Poke { got := got + 1; emit Echo{who: "child"} in outer }
}

agent Parent {
  var kids: [Agent] := []
  var echoes: Int := 0
  on Initialize { kids := kids ++ [spawn Child() in inner] }
  on Poke as p [p.space == outer] { emit Poke{} in inner for kids }
  on Echo { echoes := echoes + 1 }
}
)vagent";

class VagentResume : public testing::Test {
protected:
  std::chrono::system_clock::time_point base =
      std::chrono::floor<std::chrono::seconds>(
          std::chrono::system_clock::now());
  std::unique_ptr<LocalHost> a;
  std::unique_ptr<LocalHost> b;

  std::unique_ptr<LocalHost>
  makeHost(std::uint64_t epoch, std::chrono::seconds offset = 0s,
           std::uint32_t valueOperations           = 65536,
           std::optional<xanadu::MutableKeys> keys = std::nullopt) {
    auto program = CompiledProgram::fromSource(kProgram);
    EXPECT_TRUE(program) << (program ? "" : program.error());
    LocalHost::Options options;
    options.manualClock              = true;
    options.keys                     = std::move(keys);
    options.epoch                    = epoch;
    options.settings.valueOperations = valueOperations;
    const auto start                 = base + offset;
    options.clock.wall               = [start] { return start; };
    return std::make_unique<LocalHost>(std::move(*program), options);
  }

  void SetUp() override {
    a = makeHost(1);
    b = makeHost(1);
    a->connect(*b);
  }

  void pumpBoth() {
    for (std::size_t guard = 0; guard < 100; ++guard) {
      if (a->pump() + b->pump() == 0) return;
    }
  }

  static Envelope event(std::string type, WireRecord fields = {}) {
    Envelope e;
    e.kind   = "event";
    e.type   = std::move(type);
    e.fields = std::move(fields);
    return e;
  }

  static std::optional<WireValue> field(LocalHost &host,
                                        const xanadu::identity::ActorPid &pid,
                                        std::string_view name) {
    auto *ctx        = host.executor().context(pid);
    const auto agent = host.executor().agentCell(pid);
    if (!ctx || !agent) return std::nullopt;
    const auto vd   = valueDims(ctx->core);
    const auto cell = fieldValue(ctx->arena, vd, *agent, name);
    if (!cell) return std::nullopt;
    auto value = readValue(ctx->arena, vd, *cell);
    return value ? std::optional<WireValue>(std::move(*value)) : std::nullopt;
  }

  static std::int64_t intField(LocalHost &host,
                               const xanadu::identity::ActorPid &pid,
                               std::string_view name) {
    const auto value = field(host, pid, name);
    const auto *i = value ? std::get_if<std::int64_t>(&value->data) : nullptr;
    EXPECT_NE(i, nullptr) << name;
    return i == nullptr ? -1 : *i;
  }

  /// Facts in the `color` predicate: how many clauses, and the first one's
  /// first argument.
  static std::pair<std::size_t, std::string>
  colors(LocalHost &host, const xanadu::identity::ActorPid &pid) {
    auto *ctx        = host.executor().context(pid);
    const auto agent = host.executor().agentCell(pid);
    if (!ctx || !agent) return {0, {}};
    auto &arena      = ctx->arena;
    const auto dims  = registerDimensions(arena);
    const auto &core = ctx->core.dims();
    for (const auto predicate : zigzag::rankAfter(arena, *agent, dims.belief)) {
      if (arena.textOf(predicate) != "color") continue;
      std::size_t count = 0;
      std::string first;
      for (const auto clause :
           zigzag::rankAfter(arena, predicate, core.clause)) {
        if (count++ == 0) {
          const auto term = arena.linked(clause, core.grab);
          const auto arg = term ? arena.linked(*term, core.grab) : std::nullopt;
          if (arg) first = arena.textOf(*arg);
        }
      }
      return {count, first};
    }
    return {0, {}};
  }

  xanadu::identity::ActorPid launchCounter(LocalHost &host,
                                           std::int64_t start) {
    WireRecord args;
    args.set("start", WireValue::integer(start));
    const auto pid = host.launch("Counter", "c", std::move(args));
    EXPECT_TRUE(pid);
    host.pump();
    return *pid;
  }
};

TEST_F(VagentResume, CounterKeepsFieldsAndBeliefsAndDoesNotRerunInitialize) {
  const auto pid = launchCounter(*a, 10);
  WireRecord bump;
  bump.set("amount", WireValue::integer(5));
  ASSERT_TRUE(a->send(pid, event("Bump", bump)));
  WireRecord paint;
  paint.set("c", WireValue::text("red"));
  ASSERT_TRUE(a->send(pid, event("Paint", paint)));
  a->pump();
  ASSERT_EQ(intField(*a, pid, "total"), 15);
  ASSERT_EQ(intField(*a, pid, "seen"), 1);

  const auto saved = a->executor().checkpoint(pid);
  ASSERT_TRUE(saved) << toString(saved.error());
  const auto resumed = b->executor().resume(*saved);
  ASSERT_TRUE(resumed);
  EXPECT_NE(*resumed, pid);
  b->pump();

  EXPECT_EQ(intField(*b, *resumed, "total"), 15);
  EXPECT_EQ(intField(*b, *resumed, "inits"), 1);
  EXPECT_EQ(intField(*b, *resumed, "seen"), 1);
  const auto label = field(*b, *resumed, "label");
  ASSERT_TRUE(label);
  EXPECT_EQ(*label, WireValue::text("set"));
  const auto tags = field(*b, *resumed, "tags");
  ASSERT_TRUE(tags);
  EXPECT_EQ(*tags, WireValue::list({WireValue::text("b")}));
  EXPECT_EQ(field(*b, *resumed, kFieldInitialized), WireValue::boolean(true));
  EXPECT_EQ(colors(*b, *resumed),
            (std::pair<std::size_t, std::string>{1, "red"}));

  // The restored belief is the live one: asserting it again adds no fact and
  // so raises no `+color`; a new colour does.
  ASSERT_TRUE(b->send(*resumed, event("Paint", paint)));
  b->pump();
  EXPECT_EQ(intField(*b, *resumed, "seen"), 1);
  paint.set("c", WireValue::text("blue"));
  ASSERT_TRUE(b->send(*resumed, event("Paint", paint)));
  b->pump();
  EXPECT_EQ(intField(*b, *resumed, "seen"), 2);
  EXPECT_EQ(colors(*b, *resumed).first, 2U);

  // And the compiled routines run against the restored fields.
  ASSERT_TRUE(b->send(*resumed, event("Bump", bump)));
  b->pump();
  EXPECT_EQ(intField(*b, *resumed, "total"), 20);
}

TEST_F(VagentResume, TurnCountIsRaisedToTheTurnTimeInMilliseconds) {
  const auto pid    = launchCounter(*a, 1);
  const auto before = intField(*a, pid, kFieldTurnCount);
  ASSERT_LT(before, 100);
  const auto saved = a->executor().checkpoint(pid);
  ASSERT_TRUE(saved);
  const auto resumed = b->executor().resume(*saved);
  ASSERT_TRUE(resumed);
  b->pump();
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          base.time_since_epoch())
                          .count();
  EXPECT_GE(intField(*b, *resumed, kFieldTurnCount), millis);
}

TEST_F(VagentResume, ARuntimeCannotResumeWhatItNeverStoppedOnTheSameEpoch) {
  const auto pid   = launchCounter(*a, 1);
  const auto saved = a->executor().checkpoint(pid);
  ASSERT_TRUE(saved);
  const auto again = a->executor().resume(*saved);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), ActorError::WrongEpoch);
}

TEST_F(VagentResume, ANewerEpochOfTheSameDeviceResumes) {
  const auto pid   = launchCounter(*a, 3);
  const auto saved = a->executor().checkpoint(pid);
  ASSERT_TRUE(saved);
  auto restarted     = makeHost(2, 0s, 65536, a->deviceKeys());
  const auto resumed = restarted->executor().resume(*saved);
  ASSERT_TRUE(resumed);
  EXPECT_EQ(resumed->epoch, 2U);
  restarted->pump();
  EXPECT_EQ(intField(*restarted, *resumed, "total"), 3);
  EXPECT_EQ(intField(*restarted, *resumed, "inits"), 1);
}

TEST_F(VagentResume, ATimerSetBeforeTheCheckpointFiresAfterResume) {
  const auto pid = launchCounter(*a, 0);
  ASSERT_TRUE(a->send(pid, event("Arm")));
  a->pump();
  a->advance(1s);
  ASSERT_EQ(intField(*a, pid, "pings"), 0);
  ASSERT_EQ(std::get<WireList>(field(*a, pid, kFieldTimers)->data).size(), 1U);
  const auto saved = a->executor().checkpoint(pid);
  ASSERT_TRUE(saved);

  auto later         = makeHost(1, 1s);
  const auto resumed = later->executor().resume(*saved);
  ASSERT_TRUE(resumed);
  later->pump();
  {
    const auto t = field(*later, *resumed, kFieldTimers);
    ASSERT_TRUE(t);
    ASSERT_EQ(std::get<WireList>(t->data).size(), 1U);
    const auto before = field(*a, pid, kFieldTimers);
    EXPECT_EQ(*t, *before);
  }
  later->advance(3s);
  EXPECT_EQ(intField(*later, *resumed, "pings"), 0);
  later->advance(1s);
  EXPECT_EQ(intField(*later, *resumed, "pings"), 1);
  later->advance(10s);
  EXPECT_EQ(intField(*later, *resumed, "pings"), 1);
}

TEST_F(VagentResume, ParentAndChildKeepTalkingAfterTheChildMigrates) {
  const auto parent = a->launch("Parent", "p");
  ASSERT_TRUE(parent);
  pumpBoth();
  ASSERT_TRUE(a->send(*parent, event("Poke")));
  pumpBoth();
  ASSERT_EQ(intField(*a, *parent, "echoes"), 1);

  const auto peers = field(*a, *parent, kFieldPeers);
  ASSERT_TRUE(peers);
  const auto &record = std::get<WireRecord>(peers->data);
  ASSERT_EQ(record.names.size(), 1U);
  const auto childName = record.names.front();
  const auto oldChild =
      pidFromText(std::get<std::string>(record.values.front().data));
  ASSERT_TRUE(oldChild);

  const auto saved = a->executor().checkpoint(*oldChild);
  ASSERT_TRUE(saved) << toString(saved.error());
  const auto moved = b->executor().resume(*saved);
  ASSERT_TRUE(moved);
  EXPECT_EQ(moved->node, b->node());
  pumpBoth();

  // The parent now addresses the child's new pid.
  const auto after = field(*a, *parent, kFieldPeers);
  ASSERT_TRUE(after);
  const auto *entry = std::get<WireRecord>(after->data).find(childName);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(pidFromText(std::get<std::string>(entry->data)), *moved);

  ASSERT_TRUE(a->executor().retire(*oldChild));
  pumpBoth();
  // The old actor's DOWN did not make the parent forget a child that moved.
  EXPECT_EQ(a->executor().stat(*parent, kStatChildDown), 0U);

  ASSERT_TRUE(a->send(*parent, event("Poke")));
  pumpBoth();
  EXPECT_EQ(intField(*b, *moved, "got"), 2);
  EXPECT_EQ(intField(*a, *parent, "echoes"), 2);
}

// -- Refusals ----------------------------------------------------------------

TEST_F(VagentResume, CheckpointOfAnUnknownAgentIsRefused) {
  const xanadu::identity::ActorPid nobody{a->node(), "x", 1, 99};
  const auto saved = a->executor().checkpoint(nobody);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error(), CheckpointRefusal::UnknownAgent);
}

TEST_F(VagentResume, CheckpointIsRefusedForAStoppedAgent) {
  const auto pid = launchCounter(*a, 1);
  ASSERT_TRUE(a->send(pid, event("Quit")));
  a->pump();
  const auto saved = a->executor().checkpoint(pid);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error(), CheckpointRefusal::Stopped);
}

TEST_F(VagentResume, CheckpointIsRefusedWithAMessageInTheMailbox) {
  const auto pid = launchCounter(*a, 1);
  WireRecord bump;
  bump.set("amount", WireValue::integer(1));
  ASSERT_TRUE(a->send(pid, event("Bump", bump)));
  const auto saved = a->executor().checkpoint(pid);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error(), CheckpointRefusal::MailboxNotEmpty);
  a->pump();
  EXPECT_TRUE(a->executor().checkpoint(pid));
}

TEST_F(VagentResume, CheckpointIsRefusedWhileASendIsUnanswered) {
  const auto parent = a->launch("Parent", "p");
  ASSERT_TRUE(parent);
  a->pump();
  ASSERT_TRUE(a->send(*parent, event("Poke")));
  // One turn: the parent forwards the poke to its child and waits for the
  // reply the runtime owes it.
  a->executor().pumpUntilIdle(1);
  const auto saved = a->executor().checkpoint(*parent);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error(), CheckpointRefusal::SendsIncomplete);
  a->pump();
  EXPECT_TRUE(a->executor().checkpoint(*parent));
}

TEST_F(VagentResume, CheckpointIsRefusedInsideAMarkOrChoicePoint) {
  const auto pid = launchCounter(*a, 1);
  auto *ctx      = a->executor().context(pid);
  ASSERT_NE(ctx, nullptr);
  const auto mark  = ctx->arena.mark();
  const auto saved = a->executor().checkpoint(pid);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error(), CheckpointRefusal::NotQuiescent);
  ctx->arena.release(mark);
  EXPECT_TRUE(a->executor().checkpoint(pid));
}

// -- P5: the checkpoint bound -------------------------------------------------

TEST_F(VagentResume, ValueOperationsBoundsBothCheckpointAndResume) {
  const auto pid      = launchCounter(*a, 1);
  auto tight          = makeHost(1, 0s, 8);
  const auto tightPid = launchCounter(*tight, 1);
  const auto refused  = tight->executor().checkpoint(tightPid);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), CheckpointRefusal::PublishFailed);

  const auto saved = a->executor().checkpoint(pid);
  ASSERT_TRUE(saved);
  auto bounded = makeHost(1, 0s, 8);
  EXPECT_FALSE(bounded->executor().resume(*saved));
}

} // namespace
