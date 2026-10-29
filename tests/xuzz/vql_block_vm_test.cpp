/**
 * @file vql_block_vm_test.cpp
 * @brief VM behavior for VQL block iteration, filtering, and actions.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
struct Fixture {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  QueryExecutor executor{coordinator, vm};
  CellRef alice{arena.makeCell("Alice")};
  CellRef bob{arena.makeCell("Bob")};
  CellRef carol{arena.makeCell("Carol")};
  CellRef red{arena.makeCell("red")};
  CellRef blue{arena.makeCell("blue")};
  CellRef personAxis{coordinator.resolveDimension("d.people")};
  CellRef tagAxis{coordinator.resolveDimension("d.tags")};
  CellRef nameAxis{coordinator.resolveDimension("d.name")};
  CellRef ageAxis{coordinator.resolveDimension("d.age")};
  CellRef tagField{coordinator.resolveDimension("d.tag")};
  CellRef activeAxis{coordinator.resolveDimension("d.active")};
  CellRef alternateAxis{coordinator.resolveDimension("d.alternate")};

  Fixture() {
    const auto aliceName = arena.makeCell("Alice");
    const auto bobName   = arena.makeCell("Bob");
    const auto carolName = arena.makeCell("Carol");
    zigzag::expectWritten(
        arena.link(core.home(), personAxis, DimVector::POS, alice));
    zigzag::expectWritten(arena.link(alice, personAxis, DimVector::POS, bob));
    zigzag::expectWritten(arena.link(bob, personAxis, DimVector::POS, carol));
    zigzag::expectWritten(
        arena.link(core.home(), tagAxis, DimVector::POS, red));
    zigzag::expectWritten(arena.link(red, tagAxis, DimVector::POS, blue));
    for (const auto [person, name, age] :
         {std::tuple{alice, aliceName, 31}, std::tuple{bob, bobName, 16},
          std::tuple{carol, carolName, 42}}) {
      zigzag::expectWritten(arena.link(person, nameAxis, DimVector::POS, name));
      zigzag::expectWritten(
          arena.link(person, ageAxis, DimVector::POS,
                     arena.makeScalarCell(static_cast<std::int64_t>(age))));
    }
    zigzag::expectWritten(arena.link(alice, tagField, DimVector::POS, red));
    zigzag::expectWritten(arena.link(carol, tagField, DimVector::POS, blue));
    zigzag::expectWritten(arena.link(alice, activeAxis, DimVector::POS,
                                     arena.makeScalarCell(true)));
    zigzag::expectWritten(arena.link(bob, activeAxis, DimVector::POS,
                                     arena.makeScalarCell(false)));
    zigzag::expectWritten(arena.link(alice, alternateAxis, DimVector::POS,
                                     arena.makeCell("fallback-A")));
    zigzag::expectWritten(arena.link(bob, alternateAxis, DimVector::POS,
                                     arena.makeCell("fallback-B")));
  }

  std::vector<std::string> texts(const std::vector<CellRef> &cells) const {
    std::vector<std::string> result;
    for (const auto cell : cells) result.push_back(arena.textOf(cell));
    return result;
  }

  std::vector<CellRef> run(std::string_view query) {
    auto result = executor.execute(query);
    EXPECT_TRUE(result) << result.error().message;
    return result ? std::move(*result) : std::vector<CellRef>{};
  }
};

TEST(VQLBlockVMTest, WhereFiltersEachInputAndPreservesSourceOrder) {
  Fixture h;
  const auto result =
      h.run("for $person in ##/d.people where $person/d.age >= 18 return "
            "$person/d.name");
  EXPECT_EQ(h.texts(result), (std::vector<std::string>{"Alice", "Carol"}));
}

TEST(VQLBlockVMTest,
     NestedForProducesOrderedCartesianStreamIncludingDuplicates) {
  Fixture h;
  const auto result =
      h.run("for $person in ##/d.people for $tag in ##/d.tags return "
            "$person/d.name");
  EXPECT_EQ(h.texts(result),
            (std::vector<std::string>{"Alice", "Alice", "Bob", "Bob", "Carol",
                                      "Carol"}));
}

TEST(VQLBlockVMTest, NestedBindingsShadowWithoutChangingEarlierValues) {
  Fixture h;
  const auto result =
      h.run("for $person in ##/d.people let $name := $person/d.name "
            "for $person in ##/d.tags return $name");
  EXPECT_EQ(h.texts(result),
            (std::vector<std::string>{"Alice", "Alice", "Bob", "Bob", "Carol",
                                      "Carol"}));
}

TEST(VQLBlockVMTest, DestructureDefaultsApplyForEveryMissingField) {
  Fixture h;
  const auto result =
      h.run("for { +d.tag: $tag := \"untagged\" } in ##/d.people return $tag");
  EXPECT_EQ(h.texts(result),
            (std::vector<std::string>{"red", "untagged", "blue"}));
}

TEST(VQLBlockVMTest, UntypedMissingFieldDoesNotDropContextOrIteration) {
  Fixture h;
  const auto result =
      h.run("for { .: $person, +d.tag: $tag } in ##/d.people return "
            "$person, $tag");
  EXPECT_EQ(h.texts(result),
            (std::vector<std::string>{"Alice", "red", "Bob", "Carol", "blue"}));
}

TEST(VQLBlockVMTest, DefaultBindingMintsDistinctCellsForEachInput) {
  Fixture h;
  const auto result = h.run("for { +d.unused: $value: String := \"fallback\" } "
                            "in ##/d.people return $value");
  ASSERT_EQ(h.texts(result),
            (std::vector<std::string>{"fallback", "fallback", "fallback"}));
  EXPECT_EQ(std::unordered_set<CellRef>(result.begin(), result.end()).size(),
            result.size());
}

TEST(VQLBlockVMTest, ConditionalActionsSelectOnlyTheChosenBranch) {
  Fixture h;
  const auto result =
      h.run("def broken(): String := 7; if true return %\"selected\" else "
            "return broken()");
  EXPECT_EQ(h.texts(result), (std::vector<std::string>{"selected"}));
}

TEST(VQLBlockVMTest, ConditionalActionChoosesIndependentlyForEachInput) {
  Fixture h;
  const auto result =
      h.run("for $person in ##/d.people if $person/d.active = true return "
            "$person/d.name else return $person/d.alternate");
  EXPECT_EQ(h.texts(result), (std::vector<std::string>{"Alice", "fallback-B"}));
}

TEST(VQLBlockVMTest, ContextCaptureUsesTheArgumentCell) {
  Fixture h;
  const auto result =
      h.run("def identity($person: Cell { .: $self }): Cell := $self; "
            "##/d.people/identity()");
  EXPECT_EQ(h.texts(result),
            (std::vector<std::string>{"Alice", "Bob", "Carol"}));
}

TEST(VQLBlockVMTest, MultipleReturnItemsAppendInDeclaredOrder) {
  Fixture h;
  const auto result = h.run("return ##/d.people, ##/d.tags");
  EXPECT_EQ(h.texts(result),
            (std::vector<std::string>{"Alice", "Bob", "Carol", "red", "blue"}));
}

TEST(VQLBlockVMTest, LoweredProgramContainsRuntimeContractAndBranchOpcodes) {
  Fixture h;
  const auto result = h.executor.execute(
      "def adult($person: Cell { +d.age: Int }): Bool := true; "
      "for $person in ##/d.people if adult($person) return $person");
  ASSERT_TRUE(result) << result.error().message;
  const auto &disassembly = h.executor.disassembly();
  EXPECT_NE(disassembly.find("MATCH_CONTRACT"), std::string_view::npos);
  EXPECT_NE(disassembly.find("BRANCH_FALSE_CELL"), std::string_view::npos);
}

} // namespace
} // namespace xanadu::vql
