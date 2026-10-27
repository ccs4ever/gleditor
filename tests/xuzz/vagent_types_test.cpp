/**
 * @file vagent_types_test.cpp
 * @brief `Opt<T>`, the built-in events of the implicit prelude and the unit of
 *        `Time` and `Duration` (design §5.9, §8.2, §8.7, §10), driven end to
 *        end through `CompiledProgram` and `LocalHost`.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "common/xanadu/vagent/runtime/lexer.hpp"
#include "common/xanadu/vagent/runtime/prelude.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using namespace std::chrono_literals;
using xanadu::identity::ActorPid;

class VagentTypes : public testing::Test {
protected:
  std::unique_ptr<LocalHost> host;
  std::chrono::system_clock::time_point base;

  void load(const char *source) {
    auto program = CompiledProgram::fromSource(source);
    ASSERT_TRUE(program) << program.error();
    LocalHost::Options options;
    options.manualClock = true;
    // The runtime refuses messages stamped far from real time.
    base = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
    options.clock.wall = [this] { return base; };
    host = std::make_unique<LocalHost>(std::move(*program), options);
  }

  static Envelope event(std::string type, WireRecord fields = {}) {
    Envelope e;
    e.kind   = "event";
    e.type   = std::move(type);
    e.fields = std::move(fields);
    return e;
  }

  void send(const ActorPid &pid, std::string type, WireRecord fields = {}) {
    ASSERT_TRUE(host->send(pid, event(std::move(type), std::move(fields))));
    host->pump();
  }

  std::optional<WireValue> field(const ActorPid &pid, std::string_view name) {
    auto *ctx        = host->executor().context(pid);
    const auto agent = host->executor().agentCell(pid);
    if (!ctx || !agent) return std::nullopt;
    const auto vd   = valueDims(ctx->core);
    const auto cell = fieldValue(ctx->arena, vd, *agent, name);
    if (!cell) return std::nullopt;
    auto value = readValue(ctx->arena, vd, *cell);
    return value ? std::optional<WireValue>(std::move(*value)) : std::nullopt;
  }

  std::int64_t intField(const ActorPid &pid, std::string_view name) {
    const auto value = field(pid, name);
    const auto *i = value ? std::get_if<std::int64_t>(&value->data) : nullptr;
    EXPECT_NE(i, nullptr) << name;
    return i == nullptr ? -1 : *i;
  }

  std::string textField(const ActorPid &pid, std::string_view name) {
    const auto value = field(pid, name);
    const auto *s    = value ? std::get_if<std::string>(&value->data) : nullptr;
    EXPECT_NE(s, nullptr) << name;
    return s == nullptr ? std::string{} : *s;
  }

  /// The name an `Opt<Agent>` field holds, or empty for `none`.
  std::string optText(const ActorPid &pid, std::string_view name) {
    const auto value = field(pid, name);
    const auto *list = value ? std::get_if<WireList>(&value->data) : nullptr;
    EXPECT_NE(list, nullptr) << name;
    if (list == nullptr || list->empty()) return {};
    const auto *s = std::get_if<std::string>(&list->front().data);
    return s == nullptr ? std::string{} : *s;
  }

  /// The number of elements an `Opt` field holds: 0 for `none`, 1 otherwise.
  std::size_t held(const ActorPid &pid, std::string_view name) {
    const auto value = field(pid, name);
    const auto *list = value ? std::get_if<WireList>(&value->data) : nullptr;
    EXPECT_NE(list, nullptr) << name;
    return list == nullptr ? 99U : list->size();
  }

  std::optional<ActorPid> peer(const ActorPid &parent, std::string_view child) {
    const auto peers = field(parent, kFieldPeers);
    const auto *rec  = peers ? std::get_if<WireRecord>(&peers->data) : nullptr;
    if (!rec) return std::nullopt;
    const auto *v = rec->find(child);
    const auto *s = v ? std::get_if<std::string>(&v->data) : nullptr;
    return s ? pidFromText(*s) : std::nullopt;
  }
};

// -- Opt<T>
// ---------------------------------------------------------------------

constexpr const char *kOpt = R"vagent(
event Arm { }
event Disarm { }
event Probe { }
event Fire { }
event Drop { }

record Box { v: Opt<Int>, w: Int }

agent A {
  var beat: Opt<Timer> := none
  var count: Opt<Int> := none
  var box: Box := Box{w: 1}
  var fired: Int := 0
  var present: Int := 0
  var absent: Int := 0
  var read: Int := 0
  var boxed: Int := 0

  on Arm { beat := Schedules.after(2s, Fire{}) }
  on Fire { fired := fired + 1; beat := none }
  on Disarm { Schedules.cancel(beat); beat := none }
  on Probe [beat] { present := present + 1 }
  on Probe [not beat] { absent := absent + 1 }
  on Drop { count := 7; box.v := 3 }
  on Probe [count == 7] { read := read + 1 }
  on Probe { if box.v == 3 then boxed := boxed + 1 }
}
)vagent";

TEST_F(VagentTypes, AnOptTimerFieldIsSetFromAfterAndClearedWhenItFires) {
  load(kOpt);
  const auto a = host->launch("A", "a");
  ASSERT_TRUE(a);
  host->pump();
  EXPECT_EQ(held(*a, "beat"), 0U);

  send(*a, "Arm");
  EXPECT_EQ(held(*a, "beat"), 1U);
  send(*a, "Probe");
  EXPECT_EQ(intField(*a, "present"), 1);
  EXPECT_EQ(intField(*a, "absent"), 0);

  base += 1500ms;
  host->advance(1500ms);
  EXPECT_EQ(intField(*a, "fired"), 0);
  base += 1ms;
  host->advance(501ms);
  EXPECT_EQ(intField(*a, "fired"), 1);
  EXPECT_EQ(held(*a, "beat"), 0U);
  send(*a, "Probe");
  EXPECT_EQ(intField(*a, "present"), 1);
  EXPECT_EQ(intField(*a, "absent"), 1);
}

TEST_F(VagentTypes, ReadingNoneFailsAndNotNoneSucceeds) {
  load(kOpt);
  const auto a = host->launch("A", "a");
  ASSERT_TRUE(a);
  host->pump();
  // `Schedules.cancel(beat)` reads a `none`: the handler fails, so its
  // writes are undone, and nothing breaks.
  send(*a, "Disarm");
  EXPECT_EQ(held(*a, "beat"), 0U);
  send(*a, "Probe");
  EXPECT_EQ(intField(*a, "absent"), 1);
  EXPECT_EQ(intField(*a, "read"), 0);
}

TEST_F(VagentTypes, AssigningATToAnOptStoresItAndCancelClearsIt) {
  load(kOpt);
  const auto a = host->launch("A", "a");
  ASSERT_TRUE(a);
  host->pump();
  send(*a, "Drop");
  EXPECT_EQ(held(*a, "count"), 1U);
  send(*a, "Probe");
  EXPECT_EQ(intField(*a, "read"), 1);
  EXPECT_EQ(intField(*a, "boxed"), 1);

  send(*a, "Arm");
  send(*a, "Disarm");
  EXPECT_EQ(held(*a, "beat"), 0U);
  // The cancelled timer never fires.
  host->advance(10s);
  EXPECT_EQ(intField(*a, "fired"), 0);
}

TEST_F(VagentTypes, ARecordFieldLeftOutIsNone) {
  load(R"vagent(
record Box { v: Opt<Int>, w: Int }
event Make { }
event Q { }
agent B {
  var seen: Int := 0
  var unseen: Int := 0
  on Make {
    let b := Box{w: 2}
    if b.v then seen := 1 else unseen := 1
    let c := Box{v: 5, w: 2}
    if c.v == 5 then seen := seen + 10
  }
}
)vagent");
  const auto b = host->launch("B", "b");
  ASSERT_TRUE(b);
  host->pump();
  send(*b, "Make");
  EXPECT_EQ(intField(*b, "seen"), 10);
  EXPECT_EQ(intField(*b, "unseen"), 1);
}

TEST_F(VagentTypes, SourceAndSpaceAreOptOnAnEvent) {
  load(R"vagent(
event Hello { }
event Kid { }
event Said { who: Str }
agent Child {
  on Initialize { emit Kid{} in outer }
}
agent Parent {
  var host_src: Int := 0
  var from_kid: Opt<Agent> := none
  var outer_hits: Int := 0
  var parent_set: Int := 0
  on Initialize { spawn Child() in inner }
  on Hello as h { if h.source then host_src := 1 }
  on Hello as h [h.space == outer] { outer_hits := outer_hits + 1 }
  on Kid as k [k.source] { from_kid := k.source }
  on Hello { if parent then parent_set := 1 }
}
)vagent");
  const auto p = host->launch("Parent", "p");
  ASSERT_TRUE(p);
  host->pump();
  send(*p, "Hello");
  // A host delivery names no emitter; it arrives through the outer space.
  EXPECT_EQ(intField(*p, "host_src"), 0);
  EXPECT_EQ(intField(*p, "outer_hits"), 1);
  EXPECT_EQ(optText(*p, "from_kid"), "p/Child#0");
  // A top-level agent has no parent.
  EXPECT_EQ(intField(*p, "parent_set"), 0);
}

TEST_F(VagentTypes, ParentIsOptAgentAndNoneForATopLevelAgent) {
  load(R"vagent(
event Ask { }
event Report { name: Str }
agent Child {
  var has_parent: Int := 0
  on Ask { if parent then has_parent := 1 }
}
agent Top {
  var kid: [Agent] := []
  var has_parent: Int := 0
  on Initialize { kid := kid ++ [spawn Child() in inner] }
  on Ask { if parent then has_parent := 1 }
}
)vagent");
  const auto top = host->launch("Top", "top");
  ASSERT_TRUE(top);
  host->pump();
  const auto kid = peer(*top, "top/Child#0");
  ASSERT_TRUE(kid);
  send(*top, "Ask");
  send(*kid, "Ask");
  EXPECT_EQ(intField(*top, "has_parent"), 0);
  EXPECT_EQ(intField(*kid, "has_parent"), 1);
  EXPECT_EQ(held(*top, kFieldParent), 0U);
  EXPECT_EQ(held(*kid, kFieldParent), 1U);
}

// -- Built-in events
// ---------------------------------------------------------------

TEST(VagentPrelude, EveryBuiltInEventOfTheDesignIsDeclared) {
  for (const auto *name :
       {"Initialize", "Destroy", "GoalAchieved", "GoalFailed", "GoalDropped",
        "PlanFailed", "EffectFailed", "ConditionFault", "ChildDown",
        "ChildStopping", "Orphaned", "SpawnFailed", "DeliveryFailed",
        "Moved"}) {
    EXPECT_NE(findPreludeEvent(name), nullptr) << name;
  }
  EXPECT_EQ(findPreludeEvent("Unheard"), nullptr);
  EXPECT_EQ(preludeImplicitFields().size(), 4U);
}

TEST(VagentPrelude, AHandlerForABuiltInEventChecksAndCompiles) {
  const auto ok = CompiledProgram::fromSource(R"vagent(
agent A {
  var c: Opt<Agent> := none
  var t: Str := ""
  on ChildDown as d { c := d.child }
  on ChildStopping as s { c := s.child; t := s.cause }
  on Orphaned as o { c := o.former_parent }
  on SpawnFailed as f { c := f.child }
  on DeliveryFailed as f { c := f.to; t := f.kind ++ f.cause }
  on Moved as m { c := m.name }
  on GoalFailed as g { t := g.goal ++ g.cause }
  on Destroy { t := "bye" }
}
)vagent");
  EXPECT_TRUE(ok) << (ok ? "" : ok.error());
  const auto bad = CompiledProgram::fromSource(R"vagent(
agent A {
  var c: Str := ""
  on ChildDown as d { c := d.nope }
}
)vagent");
  EXPECT_FALSE(bad);
}

constexpr const char *kLifecycle = R"vagent(
event Quit { }
event Stop { }
event Kill { }

agent Leaf {
  var former: Opt<Agent> := none
  var orphaned: Int := 0
  var has_parent: Int := 0
  on Orphaned as o {
    former := o.former_parent
    orphaned := orphaned + 1
    if parent then has_parent := 1
  }
}

agent Top {
  var kid: [Agent] := []
  var down: Opt<Agent> := none
  var stopping: Opt<Agent> := none
  var why: Str := ""
  on Initialize { kid := kid ++ [spawn Leaf() in inner] }
  on Stop { stop kid[0] }
  on Quit { terminate keeping children }
  on ChildStopping as s { stopping := s.child; why := s.cause }
  on ChildDown as d { down := d.child }
}
)vagent";

TEST_F(VagentTypes, OnChildStoppingFiresWithTheChildAndTheCause) {
  load(kLifecycle);
  const auto top = host->launch("Top", "top");
  ASSERT_TRUE(top);
  host->pump();
  send(*top, "Stop");
  EXPECT_EQ(optText(*top, "stopping"), "top/Leaf#0");
  EXPECT_EQ(textField(*top, "why"), "stop");
  EXPECT_EQ(optText(*top, "down"), "");
}

TEST_F(VagentTypes, OnOrphanedFiresInAKeptChildWithTheFormerParent) {
  load(kLifecycle);
  const auto top = host->launch("Top", "top");
  ASSERT_TRUE(top);
  host->pump();
  const auto leaf = peer(*top, "top/Leaf#0");
  ASSERT_TRUE(leaf);
  send(*top, "Quit");
  EXPECT_EQ(intField(*leaf, "orphaned"), 1);
  EXPECT_EQ(optText(*leaf, "former"), "top");
  // The launcher adopted it before the handler ran: no parent is named.
  EXPECT_EQ(intField(*leaf, "has_parent"), 0);
}

TEST_F(VagentTypes, OnChildDownFiresForAChildThatVanishedWithoutAWord) {
  load(kLifecycle);
  const auto top = host->launch("Top", "top");
  ASSERT_TRUE(top);
  host->pump();
  const auto leaf = peer(*top, "top/Leaf#0");
  ASSERT_TRUE(leaf);
  ASSERT_TRUE(host->runtime().stop(*leaf));
  host->pump();
  EXPECT_EQ(optText(*top, "down"), "top/Leaf#0");
  EXPECT_EQ(optText(*top, "stopping"), "");
}

// The adapter's own raise of SpawnFailed is covered by VagentLifecycle with a
// refusing issuer; here the compiled handler is reached by delivering the event
// as the adapter writes it, since LocalHost's issuer never refuses.
TEST_F(VagentTypes, OnSpawnFailedDispatchesWithItsChildField) {
  load(R"vagent(
agent Top {
  var failed: Opt<Agent> := none
  on SpawnFailed as f { failed := f.child }
}
)vagent");
  const auto top = host->launch("Top", "top");
  ASSERT_TRUE(top);
  host->pump();
  WireRecord fields;
  fields.set("child", WireValue::text("top/Leaf#0"));
  send(*top, "SpawnFailed", std::move(fields));
  EXPECT_EQ(optText(*top, "failed"), "top/Leaf#0");
}

TEST_F(VagentTypes, OnDeliveryFailedFiresWhenAnEmissionIsNeverAnswered) {
  load(R"vagent(
event Hi { }
agent Leaf {
  var n: Int := 0
  on Hi { n := n + 1 }
}
agent Top {
  var kid: [Agent] := []
  var dest: Opt<Agent> := none
  var kind: Str := ""
  var cause: Str := ""
  on Initialize {
    kid := kid ++ [spawn Leaf() in inner]
    emit Hi{} in inner for kid
  }
  on DeliveryFailed as f { dest := f.to; kind := f.kind; cause := f.cause }
}
)vagent");
  const auto top = host->launch("Top", "top");
  ASSERT_TRUE(top);
  // Only the parent runs; its child never gets a turn, so never answers.
  EXPECT_EQ(host->pump(1), 1U);
  for (int i = 1; i <= 7; ++i) {
    host->executor().tick(base + std::chrono::seconds(i));
  }
  EXPECT_EQ(optText(*top, "dest"), "top/Leaf#0");
  EXPECT_EQ(textField(*top, "kind"), "event");
  EXPECT_EQ(textField(*top, "cause"), "retries");
}

// -- Time and Duration are milliseconds (§8.7)
// ------------------------------------------

TEST(VagentTime, DurationLiteralsAreMilliseconds) {
  Lexer lexer("2s 250ms 1min");
  const auto tokens = lexer.tokenize();
  ASSERT_TRUE(tokens);
  ASSERT_GE(tokens->size(), 3U);
  EXPECT_EQ((*tokens)[0].durationMs, 2000);
  EXPECT_EQ((*tokens)[1].durationMs, 250);
  EXPECT_EQ((*tokens)[2].durationMs, 60000);
}

TEST_F(VagentTypes, ClockNowPlusADurationIsATimeThatMuchLater) {
  load(R"vagent(
event Stamp { }
event Later { }
agent A {
  var at: Time := Clock.now()
  var due: Time := Clock.now()
  var gap: Duration := 0ms
  var wait: Duration := 5s
  on Stamp { at := Clock.now(); due := Clock.now() + 2s }
  on Later { gap := due - at }
}
)vagent");
  const auto a = host->launch("A", "a");
  ASSERT_TRUE(a);
  host->pump();
  send(*a, "Stamp");
  send(*a, "Later");
  EXPECT_EQ(intField(*a, "gap"), 2000);
  EXPECT_EQ(intField(*a, "wait"), 5000);
}

TEST_F(VagentTypes, AFiveSecondTimerFiresAfterFiveThousandMilliseconds) {
  load(R"vagent(
event Arm { }
event Ping { }
agent A {
  var pings: Int := 0
  on Arm { Schedules.after(5s, Ping{}) }
  on Ping { pings := pings + 1 }
}
)vagent");
  const auto a = host->launch("A", "a");
  ASSERT_TRUE(a);
  host->pump();
  send(*a, "Arm");
  host->advance(4999ms);
  EXPECT_EQ(intField(*a, "pings"), 0);
  host->advance(1ms);
  EXPECT_EQ(intField(*a, "pings"), 1);
}

} // namespace
