#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <ostream>
#include <string>
#include <type_traits>
#include <variant>

#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"

namespace xanadu::vagent::runtime {
// Readable gtest output for the scalar wire values these tests compare.
void PrintTo(const WireValue &value, std::ostream *os) {
  std::visit(
      [os](const auto &v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>)
          *os << '"' << v << '"';
        else if constexpr (std::is_arithmetic_v<T>)
          *os << v;
        else if constexpr (std::is_same_v<T, std::monostate>)
          *os << "()";
        else
          *os << "<structured>";
      },
      value.data);
}
} // namespace xanadu::vagent::runtime

namespace {
using namespace xanadu::vagent::runtime;
using xanadu::identity::ActorPid;
using zigzag::vortex::mesh::ActorError;

Envelope event(std::string name) {
  Envelope value;
  value.kind = std::string(kKindEvent);
  value.type = std::move(name);
  return value;
}

std::optional<WireValue> field(LocalHost &host, const ActorPid &pid,
                               std::string_view name) {
  const auto *context = host.executor().context(pid);
  const auto agent    = host.executor().agentCell(pid);
  if (!context || !agent) return std::nullopt;
  const auto dims = valueDims(context->core);
  const auto cell = fieldValue(context->arena, dims, *agent, name);
  if (!cell) return std::nullopt;
  const auto value = readValue(context->arena, dims, *cell);
  return value ? std::optional<WireValue>(*value) : std::nullopt;
}

constexpr auto initial     = R"(
event Ping { n: Int }
agent A {
  var count: Int := 7
  var legacy: Str := "saved"
  on Ping as p { count := count + p.n }
}
)";
constexpr auto replacement = R"(
event Ping { n: Int }
event Migrated { old: Str, version: Str }
agent A {
  var count: Int := 999
  var added: Int := count + 3
  on Upgrade as u {
    count := count + 100
    emit Migrated{old: dropped_field("legacy"), version: u.from_version} in outer
  }
  on Ping as p { count := count + p.n * 2 }
}
)";

TEST(VagentLiveUpgrade, MetadataShapeUsesNewProgramFormatIdentity) {
  auto compiled = CompiledProgram::fromSource(initial);
  ASSERT_TRUE(compiled) << compiled.error();
  std::string oldMaterial("vagent-program-2");
  oldMaterial.push_back('\0');
  oldMaterial += initial;
  const auto oldRef = xanadu::toHex32(xanadu::sha256Digest(oldMaterial));
  EXPECT_EQ(kProgramFormat, "vagent-program-3");
  EXPECT_NE(compiled->programRef(), oldRef);
  LocalHost host(std::move(*compiled));
  const auto refused = host.executor().launch("A", "old-format", {}, oldRef);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), ActorError::NotAuthorized);
}

TEST(VagentLiveUpgrade,
     PreservesPidMailboxFieldsAndIndependentProgramVersions) {
  auto old  = CompiledProgram::fromSource(initial);
  auto next = CompiledProgram::fromSource(replacement);
  ASSERT_TRUE(old) << old.error();
  ASSERT_TRUE(next) << next.error();
  const std::string originalRef(old->programRef());
  LocalHost host(std::move(*old));
  const auto changed   = host.launch("A", "changed");
  const auto unchanged = host.launch("A", "unchanged");
  ASSERT_TRUE(changed);
  ASSERT_TRUE(unchanged);
  host.pump();
  auto held = host.retainProgram(std::move(*next));
  ASSERT_NE(held, nullptr);
  auto *oldContext   = host.executor().context(*unchanged);
  const auto oldOps  = host.program().programStore()->opCount();
  const auto nextOps = held->programStore()->opCount();
  Envelope ping      = event("Ping");
  ping.fields.set("n", WireValue::integer(5));
  ASSERT_TRUE(host.send(*changed, ping));
  ASSERT_EQ(host.runtime().pending(*changed), 1U);
  ASSERT_TRUE(host.upgrade(*changed, held));
  EXPECT_EQ(host.runtime().pending(*changed), 1U);
  EXPECT_EQ(host.runtime().actorCount(), 2U);
  EXPECT_EQ(field(host, *changed, "count"), WireValue::integer(107));
  EXPECT_EQ(field(host, *changed, "added"), WireValue::integer(10));
  EXPECT_FALSE(field(host, *changed, "legacy"));
  ASSERT_EQ(host.emissions().size(), 1U);
  EXPECT_EQ(*host.emissions().back().fields.find("old"),
            WireValue::text("saved"));
  EXPECT_EQ(*host.emissions().back().fields.find("version"),
            WireValue::text(originalRef));
  host.pump();
  EXPECT_EQ(field(host, *changed, "count"), WireValue::integer(117));
  ASSERT_TRUE(host.send(*unchanged, ping));
  host.pump();
  EXPECT_EQ(field(host, *unchanged, "count"), WireValue::integer(12));
  EXPECT_EQ(host.executor().context(*unchanged), oldContext);
  const auto fresh = host.launch(held, "A", "new-version");
  ASSERT_TRUE(fresh);
  host.pump();
  EXPECT_EQ(field(host, *fresh, "count"), WireValue::integer(999));
  EXPECT_EQ(host.program().programStore()->opCount(), oldOps);
  EXPECT_EQ(held->programStore()->opCount(), nextOps);
  const auto checkpoint = host.executor().checkpoint(*changed);
  ASSERT_TRUE(checkpoint) << toString(checkpoint.error());
  EXPECT_EQ(checkpoint->programRef, held->programRef());
  // A runtime never resumes what it has not stopped on the same epoch, so the
  // checkpoint moves to another host holding only the upgraded program.
  auto moved = CompiledProgram::fromSource(replacement);
  ASSERT_TRUE(moved) << moved.error();
  LocalHost later(std::move(*moved));
  const auto resumed = later.executor().resume(*checkpoint);
  ASSERT_TRUE(resumed);
  later.pump();
  ASSERT_TRUE(later.send(*resumed, ping));
  later.pump();
  EXPECT_EQ(field(later, *resumed, "count"), WireValue::integer(127));
}

TEST(VagentLiveUpgrade,
     HookFailureAndFuelFailureLeaveActorAdapterAndOutboxUntouched) {
  auto old = CompiledProgram::fromSource(initial);
  ASSERT_TRUE(old) << old.error();
  LocalHost host(std::move(*old));
  auto pid = host.launch("A", "same");
  ASSERT_TRUE(pid);
  host.pump();
  auto *context         = host.executor().context(*pid);
  const auto agent      = host.executor().agentCell(*pid);
  const auto checkpoint = host.executor().checkpoint(*pid);
  ASSERT_TRUE(checkpoint);
  // `fail` is ordinary statement failure and the hook still commits; only a
  // hard turn failure such as fuel exhaustion may refuse the upgrade.
  for (const auto &body :
       {std::string("while count >= 0 do count := count + 1")}) {
    const std::string source = R"(
event Ping { n: Int }
event Leaked {}
agent A {
  var count: Int := 0
  var legacy: Str := "different"
  on Upgrade {
    count := 900
    emit Leaked{} in outer
)" + body + R"(
  }
  on Ping as p { count := count + p.n }
}
)";
    auto compiled            = CompiledProgram::fromSource(source);
    ASSERT_TRUE(compiled) << compiled.error();
    auto held = host.retainProgram(std::move(*compiled));
    ASSERT_NE(held, nullptr);
    EXPECT_FALSE(host.upgrade(*pid, held, 100000));
    EXPECT_EQ(host.executor().context(*pid), context);
    EXPECT_EQ(host.executor().agentCell(*pid), agent);
    EXPECT_EQ(field(host, *pid, "count"), WireValue::integer(7));
    EXPECT_TRUE(host.emissions().empty());
    const auto after = host.executor().checkpoint(*pid);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->programRef, checkpoint->programRef);
  }
  auto ping = event("Ping");
  ping.fields.set("n", WireValue::integer(2));
  ASSERT_TRUE(host.send(*pid, ping));
  host.pump();
  EXPECT_EQ(field(host, *pid, "count"), WireValue::integer(9));
}

TEST(VagentLiveUpgrade, RefusesOrderedKeyAndFieldTypeChangesWithoutMutation) {
  constexpr auto source = R"(
predicate p(id: Int, value: Str)
agent A { var count: Int := 7 }
)";
  auto old              = CompiledProgram::fromSource(source);
  ASSERT_TRUE(old) << old.error();
  LocalHost host(std::move(*old));
  const auto pid = host.launch("A", "a");
  ASSERT_TRUE(pid);
  host.pump();
  auto *context = host.executor().context(*pid);
  for (const auto next : {R"(predicate p(key id: Int, value: Str)
agent A { var count: Int := 0 })",
                          R"(predicate p(id: Int, value: Str)
agent A { var count: Float := 0.0 })"}) {
    auto compiled = CompiledProgram::fromSource(next);
    ASSERT_TRUE(compiled) << compiled.error();
    auto held = host.retainProgram(std::move(*compiled));
    ASSERT_NE(held, nullptr);
    EXPECT_FALSE(host.upgrade(*pid, held));
    EXPECT_EQ(host.executor().context(*pid), context);
    EXPECT_EQ(field(host, *pid, "count"), WireValue::integer(7));
  }
}

TEST(VagentLiveUpgrade,
     RunsRealMountedSkillUpgradeHookAndPreservesPrivateFields) {
  const auto source = [](bool changed) {
    return std::string(R"(
capacity Storage { def get_tag(): Int }
skill S implements Storage {
  var value: Int := 20
  on upgrade(previous) { if previous != "" then value := value + 3 else value := 0 }
  def get_tag(): Int { value }
}
event Read {}
agent A {
  uses Storage
  install S()
  var seen: Int := 0
  on Read { seen := Storage.get_tag() }
)") + (changed ? "var added: Int := 1\n" : "") +
           "}\n";
  };
  auto old  = CompiledProgram::fromSource(source(false));
  auto next = CompiledProgram::fromSource(source(true));
  ASSERT_TRUE(old) << old.error();
  ASSERT_TRUE(next) << next.error();
  LocalHost host(std::move(*old));
  const auto pid = host.launch("A", "a");
  ASSERT_TRUE(pid);
  host.pump();
  auto held = host.retainProgram(std::move(*next));
  ASSERT_TRUE(host.upgrade(*pid, held));
  ASSERT_TRUE(host.send(*pid, event("Read")));
  host.pump();
  EXPECT_EQ(field(host, *pid, "seen"), WireValue::integer(23));
}

TEST(VagentLiveUpgrade, RefusesActiveArenaMarkAndQueuedSchemaChange) {
  auto old  = CompiledProgram::fromSource(initial);
  auto next = CompiledProgram::fromSource(replacement);
  ASSERT_TRUE(old);
  ASSERT_TRUE(next);
  LocalHost host(std::move(*old));
  const auto pid = host.launch("A", "a");
  ASSERT_TRUE(pid);
  host.pump();
  auto held         = host.retainProgram(std::move(*next));
  auto *context     = host.executor().context(*pid);
  const auto mark   = context->arena.mark();
  const auto status = host.upgrade(*pid, held);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), ActorError::NotQuiescent);
  context->arena.discard(mark);
  auto changed = CompiledProgram::fromSource(R"(
event Ping { n: Int, extra: Int }
agent A { var count: Int := 7 }
)");
  ASSERT_TRUE(changed);
  auto incompatible = host.retainProgram(std::move(*changed));
  auto ping         = event("Ping");
  ping.fields.set("n", WireValue::integer(1));
  ASSERT_TRUE(host.send(*pid, ping));
  EXPECT_FALSE(host.upgrade(*pid, incompatible));
  EXPECT_EQ(host.runtime().pending(*pid), 1U);
  EXPECT_EQ(host.executor().context(*pid), context);
}
TEST(VagentLiveUpgrade, ChildrenSpawnAndCheckpointUseTheCommittedProgramRef) {
  const auto source = [](int version) {
    return std::string(R"(
event Make {}
event ChildVersion { value: Int }
agent Child {
  on Initialize { emit ChildVersion{value: )") +
           std::to_string(version) + R"(} in outer }
}
agent A {
  var seen: Int := 0
  on Make { spawn Child() in inner }
  on ChildVersion as v { seen := v.value }
}
)";
  };
  auto old  = CompiledProgram::fromSource(source(1));
  auto next = CompiledProgram::fromSource(source(2));
  ASSERT_TRUE(old) << old.error();
  ASSERT_TRUE(next) << next.error();
  LocalHost host(std::move(*old));
  const auto pid = host.launch("A", "parent");
  ASSERT_TRUE(pid);
  host.pump();
  ASSERT_TRUE(host.send(*pid, event("Make")));
  host.pump();
  EXPECT_EQ(field(host, *pid, "seen"), WireValue::integer(1));
  auto held = host.retainProgram(std::move(*next));
  ASSERT_TRUE(host.upgrade(*pid, held));
  ASSERT_TRUE(host.send(*pid, event("Make")));
  host.pump();
  EXPECT_EQ(field(host, *pid, "seen"), WireValue::integer(2));
  EXPECT_EQ(host.runtime().actorCount(), 3U);
  const auto checkpoint = host.executor().checkpoint(*pid);
  ASSERT_TRUE(checkpoint);
  EXPECT_EQ(checkpoint->programRef, held->programRef());
}

TEST(VagentLiveUpgrade, PreservesBeliefSupportsAndScheduledTimerDeadlines) {
  const auto source = [](int version) {
    return std::string(R"(
predicate color(value: Str)
event Paint {}
event Query {}
event Arm {}
event Beat {}
agent A {
  var seen: Int := 0
  var beats: Int := 0
  on Paint { believe color("red") }
  on Query { if believes color("red") then seen := seen + 1 }
  on Arm { Schedules.after(5s, Beat{}) }
  on Beat { beats := beats + )") +
           std::to_string(version) + R"( }
}
)";
  };
  auto old  = CompiledProgram::fromSource(source(1));
  auto next = CompiledProgram::fromSource(source(2));
  ASSERT_TRUE(old) << old.error();
  ASSERT_TRUE(next) << next.error();
  LocalHost::Options options;
  options.manualClock = true;
  LocalHost host(std::move(*old), options);
  const auto pid = host.launch("A", "a");
  ASSERT_TRUE(pid);
  host.pump();
  ASSERT_TRUE(host.send(*pid, event("Paint")));
  ASSERT_TRUE(host.send(*pid, event("Arm")));
  host.pump();
  host.advance(std::chrono::seconds(2));
  auto held = host.retainProgram(std::move(*next));
  ASSERT_TRUE(host.upgrade(*pid, held));
  ASSERT_TRUE(host.send(*pid, event("Query")));
  host.pump();
  EXPECT_EQ(field(host, *pid, "seen"), WireValue::integer(1));
  host.advance(std::chrono::seconds(3));
  EXPECT_EQ(field(host, *pid, "beats"), WireValue::integer(2));
}

} // namespace
