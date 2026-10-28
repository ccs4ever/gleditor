#include <gtest/gtest.h>

#include <algorithm>
#include <ranges>
#include <string_view>

#include "common/xanadu/vagent/runtime/parser.hpp"
#include "common/xanadu/vagent/runtime/upgrade_schema.hpp"

namespace {
using namespace xanadu::vagent::runtime;

UpgradeSchema schema(std::string_view source) {
  auto unit = parseUnit(source);
  EXPECT_TRUE(unit) << (unit ? "" : formatParseError(unit.error()));
  if (!unit) return {};
  auto checked = check(*unit);
  EXPECT_TRUE(checked);
  if (!checked) return {};
  auto result = makeUpgradeSchema(*checked);
  EXPECT_TRUE(result);
  return result ? std::move(*result) : UpgradeSchema{};
}

TEST(VagentUpgradeSchemaTest, PredicateKeyAndOrderedTypeChangesAreBreaking) {
  const auto oldSchema = schema(R"(
predicate position(key who: Agent, x: Int, y: Float)
agent Worker {}
)");
  const auto changed   = schema(R"(
predicate position(x: Int, key who: Agent, y: Float)
agent Worker {}
)");
  const auto changes   = compareUpgradeSchemas(oldSchema, changed);
  EXPECT_TRUE(std::ranges::any_of(changes, [](const auto &c) {
    return c.kind == UpgradeChangeKind::PredicateKeyPositions;
  }));
}

TEST(VagentUpgradeSchemaTest, RemovedFieldsRemainAvailableAsTypedChanges) {
  const auto oldSchema = schema("agent Worker { var old_value: Int := 1 }");
  const auto newSchema = schema("agent Worker { var added: Str := \"new\" }");
  const auto changes   = compareUpgradeSchemas(oldSchema, newSchema);
  EXPECT_TRUE(std::ranges::any_of(changes, [](const auto &c) {
    return c.kind == UpgradeChangeKind::RemovedField &&
           c.member == "old_value" &&
           c.oldType == Type{Type::Kind::Int, {}, {}};
  }));
  EXPECT_TRUE(std::ranges::any_of(changes, [](const auto &c) {
    return c.kind == UpgradeChangeKind::AddedField && c.member == "added";
  }));
}

TEST(VagentUpgradeSchemaTest, PlanHashIgnoresFormattingAndChangesWithBody) {
  const auto compact   = schema(R"(
 goal Work() perform
agent Worker { plan work_plan for Work { let x: Int := 1; x := x + 1 } }
)");
  const auto formatted = schema(R"(
goal Work() perform
agent Worker {
  plan work_plan for Work {
    let x : Int := 1
    x := x + 1
  }
}
)");
  const auto changed   = schema(R"(
goal Work() perform
agent Worker { plan work_plan for Work { let x: Int := 1; x := x + 2 } }
)");
  const auto a         = compact.agents.at("Worker").plans.at("work_plan");
  const auto b         = formatted.agents.at("Worker").plans.at("work_plan");
  const auto c         = changed.agents.at("Worker").plans.at("work_plan");
  EXPECT_EQ(a.astEncodingVersion, "vagent-plan-ast-1");
  EXPECT_EQ(a.astHash, b.astHash);
  EXPECT_NE(a.astHash, c.astHash);
}

} // namespace
