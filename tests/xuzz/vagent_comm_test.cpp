/**
 * @file vagent_comm_test.cpp
 * @brief `tell`/`untell` and `stop`/`terminate [keeping children]` driven end
 *        to end through `CompiledProgram` and `LocalHost` (design §4.5, §6.1,
 *        §7.1, §8.6).
 */
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/program.hpp"
#include "common/xanadu/vagent/wire.hpp"

namespace {

using namespace xanadu::vagent;

class VagentComm : public testing::Test {
protected:
  std::unique_ptr<LocalHost> host;

  void load(const char *source) {
    auto program = CompiledProgram::fromSource(source);
    ASSERT_TRUE(program) << program.error();
    host = std::make_unique<LocalHost>(std::move(*program));
  }

  static Envelope event(std::string type) {
    Envelope e;
    e.kind = "event";
    e.type = std::move(type);
    return e;
  }

  std::optional<WireValue> field(const xanadu::identity::ActorPid &pid,
                                 std::string_view name) {
    auto *ctx        = host->executor().context(pid);
    const auto agent = host->executor().agentCell(pid);
    if (!ctx || !agent) return std::nullopt;
    const auto vd   = valueDims(ctx->core);
    const auto cell = fieldValue(ctx->arena, vd, *agent, name);
    if (!cell) return std::nullopt;
    auto value = readValue(ctx->arena, vd, *cell);
    return value ? std::optional<WireValue>(std::move(*value)) : std::nullopt;
  }

  std::int64_t intField(const xanadu::identity::ActorPid &pid,
                        std::string_view name) {
    const auto value = field(pid, name);
    const auto *i = value ? std::get_if<std::int64_t>(&value->data) : nullptr;
    EXPECT_NE(i, nullptr) << name;
    return i == nullptr ? -1 : *i;
  }

  std::string textField(const xanadu::identity::ActorPid &pid,
                        std::string_view name) {
    const auto value = field(pid, name);
    const auto *s    = value ? std::get_if<std::string>(&value->data) : nullptr;
    EXPECT_NE(s, nullptr) << name;
    return s == nullptr ? std::string{} : *s;
  }

  /// A child's pid, read from its parent's `$peers` while the parent lives.
  std::optional<xanadu::identity::ActorPid>
  peer(const xanadu::identity::ActorPid &parent, std::string_view child) {
    const auto peers = field(parent, kFieldPeers);
    const auto *rec  = peers ? std::get_if<WireRecord>(&peers->data) : nullptr;
    if (!rec) return std::nullopt;
    const auto *v = rec->find(child);
    const auto *s = v ? std::get_if<std::string>(&v->data) : nullptr;
    return s ? pidFromText(*s) : std::nullopt;
  }

  std::size_t emitted(std::string_view type) {
    std::size_t n = 0;
    for (const auto &e : host->emissions()) n += e.type == type ? 1 : 0;
    return n;
  }
};

// -- tell / untell
// -------------------------------------------------------------

constexpr const char *kTellProgram = R"vagent(
predicate ready(who: Str)
predicate mood(m: Str)
event Retract { }
event Soothe { }
event Moody { m: Str }
event Calm { }

agent Worker {
  on Initialize { tell parent ready("x") }
  on Retract { untell parent ready("x") }
  on +mood(?m) { emit Moody{m: m} in outer }
}

agent Hub {
  var workers: [Agent] := []
  var plus: Int := 0
  var minus: Int := 0
  var moods: Int := 0
  on Initialize {
    workers := workers ++ [spawn Worker() in inner]
    workers := workers ++ [spawn Worker() in inner]
  }
  on +ready(?w) { plus := plus + 1 }
  on -ready(?w) { minus := minus + 1 }
  on Retract as r [r.space == outer] { emit Retract{} in inner for workers[0] }
  on Soothe as s [s.space == outer] { tell workers[0] mood("calm") }
  on Calm as s [s.space == outer] { untell workers[0] mood("calm") }
  on Moody { moods := moods + 1 }
}
)vagent";

TEST_F(VagentComm, TellAddsTheSenderAsASourceAndFiresPlusOnce) {
  load(kTellProgram);
  const auto hub = host->launch("Hub", "hub");
  ASSERT_TRUE(hub);
  host->pump();
  // Two workers told the same fact: one fact, so `+ready` ran once.
  EXPECT_EQ(intField(*hub, "plus"), 1);
  EXPECT_EQ(intField(*hub, "minus"), 0);
  EXPECT_EQ(host->executor().stat(*hub, kStatBadTell), 0U);
}

TEST_F(VagentComm, AFactWithTwoSourcesSurvivesOneUntell) {
  load(kTellProgram);
  const auto hub = host->launch("Hub", "hub");
  ASSERT_TRUE(hub);
  host->pump();
  ASSERT_EQ(intField(*hub, "plus"), 1);

  // worker 0 retracts: worker 1 still asserts the fact, so no `-ready`.
  ASSERT_TRUE(host->send(*hub, event("Retract")));
  host->pump();
  EXPECT_EQ(intField(*hub, "minus"), 0);
  EXPECT_EQ(intField(*hub, "plus"), 1);

  // worker 0 retracting again changes nothing either.
  ASSERT_TRUE(host->send(*hub, event("Retract")));
  host->pump();
  EXPECT_EQ(intField(*hub, "minus"), 0);
}

TEST_F(VagentComm, TheLastUntellRaisesMinus) {
  constexpr const char *kSolo = R"vagent(
predicate ready(who: Str)
event Retract { }
agent Worker {
  on Initialize { tell parent ready("x") }
  on Retract { untell parent ready("x") }
}
agent Hub {
  var w: [Agent] := []
  var plus: Int := 0
  var minus: Int := 0
  on Initialize { w := w ++ [spawn Worker() in inner] }
  on +ready(?x) { plus := plus + 1 }
  on -ready(?x) { minus := minus + 1 }
  on Retract as r [r.space == outer] { emit Retract{} in inner for w }
}
)vagent";
  load(kSolo);
  const auto hub = host->launch("Hub", "hub");
  ASSERT_TRUE(hub);
  host->pump();
  EXPECT_EQ(intField(*hub, "plus"), 1);
  ASSERT_TRUE(host->send(*hub, event("Retract")));
  host->pump();
  EXPECT_EQ(intField(*hub, "minus"), 1);
}

TEST_F(VagentComm, ParentTellsAChildAndAListOfChildren) {
  load(kTellProgram);
  const auto hub = host->launch("Hub", "hub");
  ASSERT_TRUE(hub);
  host->pump();
  ASSERT_TRUE(host->send(*hub, event("Soothe")));
  host->pump();
  // The worker's `+mood` handler emitted to its parent, the hub.
  EXPECT_EQ(intField(*hub, "moods"), 1);
}

TEST_F(VagentComm, UntellOfAFactNeverToldChangesNothing) {
  load(kTellProgram);
  const auto hub = host->launch("Hub", "hub");
  ASSERT_TRUE(hub);
  host->pump();
  ASSERT_TRUE(host->send(*hub, event("Calm")));
  host->pump();
  EXPECT_EQ(intField(*hub, "moods"), 0);
  EXPECT_EQ(host->runtime().actorCount(), 3U);
}

TEST_F(VagentComm, TellToAListReachesEveryRecipient) {
  constexpr const char *kList = R"vagent(
predicate go(n: Int)
event Fire { }
event Heard { n: Int }
agent Worker {
  on +go(?n) { emit Heard{n: n} in outer }
}
agent Hub {
  var kids: [Agent] := []
  var heard: Int := 0
  on Initialize {
    kids := kids ++ [spawn Worker() in inner]
    kids := kids ++ [spawn Worker() in inner]
    kids := kids ++ [spawn Worker() in inner]
  }
  on Fire as f [f.space == outer] { tell kids go(7) }
  on Heard as h { heard := heard + h.n }
}
)vagent";
  load(kList);
  const auto hub = host->launch("Hub", "hub");
  ASSERT_TRUE(hub);
  host->pump();
  ASSERT_TRUE(host->send(*hub, event("Fire")));
  host->pump();
  EXPECT_EQ(intField(*hub, "heard"), 21);
}

TEST(VagentCommChecker, TellToANonAgentIsRefused) {
  const auto bad = CompiledProgram::fromSource(R"vagent(
predicate p(x: Int)
agent A { on Initialize { tell 3 p(1) } }
)vagent");
  EXPECT_FALSE(bad);
}

// -- stop / terminate
// ----------------------------------------------------------

TEST(VagentCommParser, KeepingChildrenIsContextual) {
  const auto ok = CompiledProgram::fromSource(R"vagent(
event Quit { }
agent A {
  var keeping: Int := 1
  var children: Int := 2
  on Quit { terminate keeping children }
}
)vagent");
  EXPECT_TRUE(ok) << (ok ? "" : ok.error());
  const auto bad = CompiledProgram::fromSource(R"vagent(
event Quit { }
agent A { on Quit { terminate keeping } }
)vagent");
  EXPECT_FALSE(bad);
}

constexpr const char *kTree = R"vagent(
event Quit { }
event QuitKeeping { }
event KillMid { }
event KillMidKeeping { }
event Ping { }
event Pong { who: Str }

agent Leaf {
  on Ping { emit Pong{who: "leaf"} in outer }
}

agent Mid {
  var leaf: [Agent] := []
  on Initialize { leaf := leaf ++ [spawn Leaf() in inner] }
  on Ping { emit Pong{who: "mid"} in outer }
}

agent Top {
  var mid: [Agent] := []
  on Initialize { mid := mid ++ [spawn Mid() in inner] }
  on Quit { terminate }
  on QuitKeeping { terminate keeping children }
  on KillMid { stop mid[0] }
  on KillMidKeeping { stop mid[0] keeping children }
  on Ping { emit Pong{who: "top"} in outer }
}
)vagent";

struct Tree {
  xanadu::identity::ActorPid top, mid, leaf;
};

TEST_F(VagentComm, TerminateCascadesThroughThreeLevels) {
  load(kTree);
  const auto top = host->launch("Top", "top");
  ASSERT_TRUE(top);
  host->pump();
  ASSERT_EQ(host->runtime().actorCount(), 3U);
  ASSERT_TRUE(host->send(*top, event("Quit")));
  host->pump();
  EXPECT_EQ(host->runtime().actorCount(), 0U);
}

TEST_F(VagentComm, StopOfAChildReapsItsSubtreeAndLeavesTheParent) {
  load(kTree);
  const auto top = host->launch("Top", "top");
  ASSERT_TRUE(top);
  host->pump();
  ASSERT_TRUE(host->send(*top, event("KillMid")));
  host->pump();
  EXPECT_EQ(host->runtime().actorCount(), 1U);
  // The parent still serves.
  ASSERT_TRUE(host->send(*top, event("Ping")));
  host->pump();
  EXPECT_EQ(emitted("Pong"), 1U);
}

TEST_F(VagentComm, StopKeepingChildrenLeavesTheGrandchildRunning) {
  load(kTree);
  const auto top = host->launch("Top", "top");
  ASSERT_TRUE(top);
  host->pump();
  const auto mid = peer(*top, "top/Mid#0");
  ASSERT_TRUE(mid);
  const auto leaf = peer(*mid, "top/Mid#0/Leaf#0");
  ASSERT_TRUE(leaf);
  ASSERT_TRUE(host->send(*top, event("KillMidKeeping")));
  host->pump();
  // Mid is gone, Leaf lives and now belongs to the launcher.
  EXPECT_EQ(host->runtime().actorCount(), 2U);
  EXPECT_EQ(textField(*leaf, kFieldParent), "");
  ASSERT_TRUE(host->send(*leaf, event("Ping")));
  host->pump();
  ASSERT_EQ(emitted("Pong"), 1U);
  EXPECT_EQ(host->emissions().back().from, "top/Mid#0/Leaf#0");
}

TEST_F(VagentComm, TerminateKeepingChildrenReparentsToTheLauncher) {
  load(kTree);
  const auto top = host->launch("Top", "top");
  ASSERT_TRUE(top);
  host->pump();
  const auto mid = peer(*top, "top/Mid#0");
  ASSERT_TRUE(mid);
  const auto leaf = peer(*mid, "top/Mid#0/Leaf#0");
  ASSERT_TRUE(leaf);
  ASSERT_TRUE(host->send(*top, event("QuitKeeping")));
  host->pump();
  // Top is gone; Mid kept Leaf in turn only because Top kept Mid, and Mid was
  // not stopped, so both run on.
  EXPECT_EQ(host->runtime().actorCount(), 2U);
  EXPECT_EQ(textField(*mid, kFieldParent), "");
  EXPECT_EQ(textField(*leaf, kFieldParent), "top/Mid#0");
  ASSERT_TRUE(host->send(*mid, event("Ping")));
  host->pump();
  ASSERT_EQ(emitted("Pong"), 1U);
  EXPECT_EQ(host->emissions().back().from, "top/Mid#0");
}

} // namespace
