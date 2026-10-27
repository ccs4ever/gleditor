/**
 * @file vagent_skill_test.cpp
 * @brief Capacities, skills and behaviors end to end (design §5.3, §5.7,
 *        §8.5, §12.2): source text through `CompiledProgram` and `LocalHost`.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"

namespace {

using namespace xanadu::vagent::runtime;

Envelope event(std::string type,
               std::vector<std::pair<std::string, WireValue>> fields = {}) {
  Envelope e;
  e.kind = "event";
  e.type = std::move(type);
  for (auto &[name, value] : fields) e.fields.set(name, std::move(value));
  return e;
}

class VagentSkill : public testing::Test {
protected:
  std::unique_ptr<LocalHost> host;

  void load(const char *source) {
    auto program = CompiledProgram::fromSource(source);
    ASSERT_TRUE(program) << program.error();
    host = std::make_unique<LocalHost>(std::move(*program));
  }

  xanadu::identity::ActorPid start(const char *agent, WireRecord args = {}) {
    const auto pid = host->launch(agent, "a", std::move(args));
    EXPECT_TRUE(pid);
    host->pump();
    return *pid;
  }

  void tell(const xanadu::identity::ActorPid &pid, Envelope e) {
    ASSERT_TRUE(host->send(pid, std::move(e)));
    host->pump();
  }

  /// The event types the agent emitted to the launcher, in order.
  std::vector<std::string> types() const {
    std::vector<std::string> out;
    for (const Envelope &e : host->emissions()) out.push_back(e.type);
    return out;
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

  /// The capacity names on the agent's `d.skill` rank, in order. A skill that
  /// implements no capacity contributes nothing here by name.
  std::vector<std::string>
  installedCapacities(const xanadu::identity::ActorPid &pid) {
    std::vector<std::string> out;
    auto *ctx        = host->executor().context(pid);
    const auto agent = host->executor().agentCell(pid);
    if (!ctx || !agent) return out;
    const auto dims = registerDimensions(ctx->arena);
    auto inst = ctx->arena.linked(*agent, dims.skill, zigzag::DimVector::POS);
    while (inst && *inst != zigzag::noCell) {
      const auto cap =
          ctx->arena.linked(*inst, dims.capacity, zigzag::DimVector::POS);
      if (cap && *cap != zigzag::noCell) {
        out.emplace_back(ctx->arena.textOf(*cap));
      }
      inst = ctx->arena.linked(*inst, dims.skill, zigzag::DimVector::POS);
    }
    return out;
  }
};

// Section 12.2, with the two emissions going to the launcher instead of a
// `dest` agent: delivery to a named agent is not what is under test.
constexpr const char *kSwap = R"vagent(
event Payload { data: Str }
event SealedPayload { key_id: Str, data: Str }
event SecurityAlert { level: Int }
event OutgoingReport { recipient: Agent, content: Str }

capacity Transport {
  def send_payload(dest: Agent, data: Str)
}
skill Loopback implements Transport {
  def send_payload(dest: Agent, data: Str) { emit Payload{data: data} in outer }
}
skill Sealed(key_id: Str) implements Transport {
  def send_payload(dest: Agent, data: Str) {
    emit SealedPayload{key_id: key_id, data: data} in outer
  }
}

behavior Reporter {
  uses Transport
  on OutgoingReport as o { send_payload(o.recipient, o.content) }
}

agent Adaptive(alert_level: Int, key_id: Str) {
  install Loopback()
  behavior Reporter
  var sealed: Bool := false

  on SecurityAlert as a [a.level > alert_level & sealed == false] {
    install Sealed(key_id)
    sealed := true
  }
}
)vagent";

Envelope report(const std::string &text) {
  return event("OutgoingReport", {{"recipient", WireValue::text("peer")},
                                  {"content", WireValue::text(text)}});
}

TEST_F(VagentSkill, SwappingASkillRoutesTheNextCallToTheNewOne) {
  load(kSwap);
  WireRecord args;
  args.set("alert_level", WireValue::integer(3));
  args.set("key_id", WireValue::text("k1"));
  const auto pid = start("Adaptive", std::move(args));
  EXPECT_EQ(installedCapacities(pid), std::vector<std::string>{"Transport"});

  tell(pid, report("one"));
  ASSERT_EQ(host->emissions().size(), 1U);
  EXPECT_EQ(host->emissions()[0].type, "Payload");
  EXPECT_EQ(*host->emissions()[0].fields.find("data"), WireValue::text("one"));

  // Below the threshold: nothing is swapped.
  tell(pid, event("SecurityAlert", {{"level", WireValue::integer(2)}}));
  tell(pid, report("two"));
  EXPECT_EQ(host->emissions().back().type, "Payload");

  tell(pid, event("SecurityAlert", {{"level", WireValue::integer(9)}}));
  EXPECT_EQ(installedCapacities(pid), std::vector<std::string>{"Transport"});
  tell(pid, report("three"));
  const Envelope &last = host->emissions().back();
  EXPECT_EQ(last.type, "SealedPayload");
  EXPECT_EQ(*last.fields.find("key_id"), WireValue::text("k1"));
  EXPECT_EQ(*last.fields.find("data"), WireValue::text("three"));
  EXPECT_EQ(host->emissions().size(), 3U);
}

constexpr const char *kExtends = R"vagent(
event Ask {}
event Answer { text: Str }

capacity Base { def name(): Str }
capacity Sub extends Base { def shout(x: Str): Str }

skill Impl implements Sub {
  def name(): Str { "impl" }
  def shout(x: Str): Str { x ++ "!" }
}

agent User {
  uses Sub
  install Impl()
  on Ask {
    emit Answer{text: Base.name() ++ ":" ++ shout("hey")} in outer
  }
}
)vagent";

TEST_F(VagentSkill, ACapacityThatExtendsAnotherGetsAnInstanceForEach) {
  load(kExtends);
  const auto pid = start("User");
  EXPECT_EQ(installedCapacities(pid),
            (std::vector<std::string>{"Sub", "Base"}));
  tell(pid, event("Ask"));
  ASSERT_EQ(host->emissions().size(), 1U);
  EXPECT_EQ(*host->emissions()[0].fields.find("text"),
            WireValue::text("impl:hey!"));
}

constexpr const char *kShared = R"vagent(
event Ask {}
event Answer { text: Str }
event Gone { who: Str }
event Drop {}

capacity Left { def left(): Str }
capacity Mid { def mid(): Str }
capacity Right { def right(): Str }

skill A implements Left, Mid {
  def left(): Str { "a.left" }
  def mid(): Str { "a.mid" }
  on uninstall { emit Gone{who: "A"} in outer }
}
skill B implements Mid, Right {
  def mid(): Str { "b.mid" }
  def right(): Str { "b.right" }
  on uninstall { emit Gone{who: "B"} in outer }
}

agent Swapper {
  uses Left, Mid, Right
  install A()
  on Ask {
    emit Answer{text: Mid.mid()} in outer
  }
  on Drop {
    install B()
    emit Answer{text: Mid.mid() ++ "/" ++ Right.right()} in outer
  }
}
)vagent";

TEST_F(VagentSkill, InstallReplacesEverySkillSharingACapacityWhole) {
  load(kShared);
  const auto pid = start("Swapper");
  EXPECT_EQ(installedCapacities(pid),
            (std::vector<std::string>{"Left", "Mid"}));
  tell(pid, event("Ask"));
  EXPECT_EQ(*host->emissions().back().fields.find("text"),
            WireValue::text("a.mid"));

  tell(pid, event("Drop"));
  // A shared Mid, so A goes whole: its Left instance leaves with it.
  EXPECT_EQ(installedCapacities(pid),
            (std::vector<std::string>{"Mid", "Right"}));
  EXPECT_EQ(types(), (std::vector<std::string>{"Answer", "Gone", "Answer"}));
  EXPECT_EQ(*host->emissions()[1].fields.find("who"), WireValue::text("A"));
  EXPECT_EQ(*host->emissions()[2].fields.find("text"),
            WireValue::text("b.mid/b.right"));
}

constexpr const char *kLifecycle = R"vagent(
event Tick {}
event Count { n: Int }
event Retire {}
event Note { text: Str }

capacity Counting { def next(): Int }

skill Ticker(start: Int) implements Counting {
  var n: Int := start
  def next(): Int {
    n := n + 1
    n
  }
  on install { emit Note{text: "up"} in outer }
  on uninstall { emit Note{text: "down"} in outer }
}

agent Counter {
  uses Counting
  install Ticker(10)
  on Tick { emit Count{n: next()} in outer }
  on Retire { uninstall Ticker }
}
)vagent";

TEST_F(VagentSkill, SkillFieldsLiveInTheFrameAndLifecycleBlocksRun) {
  load(kLifecycle);
  const auto pid = start("Counter");
  EXPECT_EQ(types(), std::vector<std::string>{"Note"});
  tell(pid, event("Tick"));
  tell(pid, event("Tick"));
  ASSERT_EQ(host->emissions().size(), 3U);
  EXPECT_EQ(*host->emissions()[1].fields.find("n"), WireValue::integer(11));
  EXPECT_EQ(*host->emissions()[2].fields.find("n"), WireValue::integer(12));

  tell(pid, event("Retire"));
  EXPECT_EQ(*host->emissions().back().fields.find("text"),
            WireValue::text("down"));
  EXPECT_TRUE(installedCapacities(pid).empty());

  // With nothing installed the capacity call fails, and the handler with it.
  tell(pid, event("Tick"));
  EXPECT_EQ(host->emissions().size(), 4U);

  // Uninstalling again is a no-op: the block does not run twice.
  tell(pid, event("Retire"));
  EXPECT_EQ(host->emissions().size(), 4U);
}

constexpr const char *kBehaviors = R"vagent(
event Ping {}
event Off {}
event On {}
event Said { who: Str }

behavior First {
  on Ping priority 1 { emit Said{who: "first"} in outer }
}
behavior Second(tag: Str) {
  var seen: Int := 0
  on Ping priority 3 {
    seen := seen + 1
    emit Said{who: tag ++ text(seen)} in outer
  }
}

agent Host {
  behavior First
  behavior Second("second")
  on Ping priority 2 { emit Said{who: "agent"} in outer }
  on Off { unmount Second }
  on On { mount Second("again") }
}
)vagent";

std::vector<std::string> said(const std::vector<Envelope> &all,
                              std::size_t from) {
  std::vector<std::string> out;
  for (std::size_t i = from; i < all.size(); ++i) {
    out.push_back(
        std::string(std::get<std::string>(all[i].fields.find("who")->data)));
  }
  return out;
}

TEST_F(VagentSkill, BehaviorHandlersInterleaveByPriorityThenUnmountStopsThem) {
  load(kBehaviors);
  const auto pid = start("Host");
  tell(pid, event("Ping"));
  EXPECT_EQ(said(host->emissions(), 0),
            (std::vector<std::string>{"second1", "agent", "first"}));

  std::size_t mark = host->emissions().size();
  tell(pid, event("Off"));
  tell(pid, event("Ping"));
  EXPECT_EQ(said(host->emissions(), mark),
            (std::vector<std::string>{"agent", "first"}));

  // Mounting again re-initialises the behavior's fields from the arguments.
  mark = host->emissions().size();
  tell(pid, event("On"));
  tell(pid, event("Ping"));
  EXPECT_EQ(said(host->emissions(), mark),
            (std::vector<std::string>{"again1", "agent", "first"}));
}

TEST_F(VagentSkill, EqualPriorityRunsTheAgentBeforeItsBehaviorsInMountOrder) {
  load(R"vagent(
event Ping {}
event Said { who: Str }
behavior B1 { on Ping { emit Said{who: "b1"} in outer } }
behavior B2 { on Ping { emit Said{who: "b2"} in outer } }
agent Host {
  behavior B1
  behavior B2
  on Ping { emit Said{who: "agent"} in outer }
}
)vagent");
  const auto pid = start("Host");
  tell(pid, event("Ping"));
  EXPECT_EQ(said(host->emissions(), 0),
            (std::vector<std::string>{"agent", "b1", "b2"}));
}

TEST_F(VagentSkill, ANativeSkillIsRefusedByName) {
  auto program = CompiledProgram::fromSource(R"vagent(
capacity Navigation {
  effect def stop_engines() within 2s
}
native skill Thruster implements Navigation
agent Drone {
  uses Navigation
  install Thruster()
}
)vagent");
  ASSERT_FALSE(program);
  EXPECT_NE(program.error().find("native skill Thruster"), std::string::npos)
      << program.error();
}

} // namespace
