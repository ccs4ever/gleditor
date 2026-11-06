/**
 * @file vql_projection_targets_profile_test.cpp
 * @brief Opt-in profile of graph-native shape projection target preparation.
 *
 * Run explicitly with --gtest_also_run_disabled_tests
 * --gtest_filter='*DISABLED_TargetScaling'.
 *
 * The measurements are raw per-case costs. They make no speedup claim.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using Clock = std::chrono::steady_clock;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

constexpr std::uint64_t maxCycles = 20'000'000;

enum class TargetMode { Alias, OwnerPath };

std::string_view targetModeName(const TargetMode mode) {
  return mode == TargetMode::Alias ? "alias_shared" : "owner_path";
}

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  void link(const CellRef owner, const DimRef dimension,
            const DimVector direction, const CellRef target) {
    zigzag::expectWritten(arena.link(owner, dimension, direction, target));
  }
};

struct TargetFixture {
  CellRef master;
  CellRef priorClone;
  CellRef tagAnchor;
  CellRef protectedNeighbor;
};

TargetFixture makeTarget(Harness &h, const DimRef cloneDimension,
                         const DimRef tagDimension,
                         const DimRef protectedDimension,
                         const std::string &text) {
  const CellRef master            = h.arena.makeCell(text);
  const CellRef priorClone        = h.arena.makeCell("prior clone");
  const CellRef tagAnchor         = h.arena.makeCell("prior tag anchor");
  const CellRef protectedNeighbor = h.arena.makeCell("protected neighbor");
  h.link(master, cloneDimension, DimVector::POS, priorClone);
  h.link(tagAnchor, tagDimension, DimVector::POS, master);
  h.link(master, protectedDimension, DimVector::POS, protectedNeighbor);
  return {.master            = master,
          .priorClone        = priorClone,
          .tagAnchor         = tagAnchor,
          .protectedNeighbor = protectedNeighbor};
}

void runTargetProfile(const std::size_t ownerCount, const TargetMode mode) {
  Harness h;
  const DimRef tagDimension    = h.coordinator.resolveDimension("d.tag");
  const DimRef oldTagDimension = h.coordinator.resolveDimension("d.old_tag");
  const DimRef keepDimension   = h.coordinator.resolveDimension("d.keep");
  const DimRef dropDimension   = h.coordinator.resolveDimension("d.drop");
  const DimRef cloneDimension  = h.coordinator.resolveDimension("d.clone");
  const DimRef protectedDimension =
      h.coordinator.resolveDimension("d.protected");

  TargetFixture sharedTarget{};
  if (mode == TargetMode::Alias) {
    sharedTarget = makeTarget(h, cloneDimension, tagDimension,
                              protectedDimension, "shared target");
  }

  std::vector<CellRef> owners;
  std::vector<CellRef> sourceTargets;
  std::vector<CellRef> displacedTargets;
  std::vector<CellRef> keepTargets;
  std::vector<CellRef> dropTargets;
  std::vector<TargetFixture> ownerTargets;
  owners.reserve(ownerCount);
  sourceTargets.reserve(ownerCount);
  displacedTargets.reserve(ownerCount);
  keepTargets.reserve(ownerCount);
  dropTargets.reserve(ownerCount);
  ownerTargets.reserve(ownerCount);

  for (std::size_t index = 0; index < ownerCount; ++index) {
    const std::string suffix = std::to_string(index);
    const CellRef owner      = h.arena.makeCell("owner " + suffix);
    const TargetFixture sourceTarget =
        makeTarget(h, cloneDimension, tagDimension, protectedDimension,
                   "owner target " + suffix);
    const CellRef displaced = h.arena.makeCell("displaced tag " + suffix);
    const CellRef keep      = h.arena.makeCell("masked keep " + suffix);
    const CellRef drop      = h.arena.makeCell("masked drop " + suffix);

    h.link(owner, tagDimension, DimVector::POS, displaced);
    h.link(owner, oldTagDimension, DimVector::POS, sourceTarget.master);
    h.link(owner, keepDimension, DimVector::POS, keep);
    h.link(owner, dropDimension, DimVector::POS, drop);

    owners.push_back(owner);
    sourceTargets.push_back(sourceTarget.master);
    displacedTargets.push_back(displaced);
    keepTargets.push_back(keep);
    dropTargets.push_back(drop);
    ownerTargets.push_back(sourceTarget);
  }

  CompilationOptions options;
  options.initialBindings.emplace("owners", owners);
  if (mode == TargetMode::Alias)
    options.initialBindings.emplace("target",
                                    std::vector<CellRef>{sharedTarget.master});

  const std::string query = mode == TargetMode::Alias
                                ? "$owners{+d.tag: $target}"
                                : "$owners{+d.tag: ./+d.old_tag}";
  VQLCompiler compiler(h.coordinator, h.vm);
  const std::size_t cellsBeforeCompile = h.arena.cellCount();
  const auto compileStart              = Clock::now();
  const CompilationResult program      = compiler.compile(query, options);
  const auto compileElapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                            compileStart);
  ASSERT_TRUE(program.success) << program.errorMessage;
  ASSERT_TRUE(program.cursor);
  const std::size_t cellsAfterCompile = h.arena.cellCount();

  const std::uint64_t cyclesBefore = h.vm.totalCycles();
  const auto runStart              = Clock::now();
  const auto execution             = h.vm.run(*program.cursor, maxCycles);
  const auto runElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - runStart);
  const std::size_t cellsAfterRun = h.arena.cellCount();
  const std::uint64_t cyclesUsed  = h.vm.totalCycles() - cyclesBefore;
  h.vm.setCursorOpcode(*program.cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_LT(cyclesUsed, maxCycles);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  std::vector<CellRef> results;
  for (const CellRef slot : program.resultSlots) {
    const auto values = h.vm.resultCells(slot);
    results.insert(results.end(), values.begin(), values.end());
  }
  ASSERT_EQ(results, owners);

  std::unordered_set<CellRef> clones;
  clones.reserve(ownerCount);
  CellRef previousClone = mode == TargetMode::Alias
                              ? sharedTarget.priorClone
                              : ownerTargets.front().priorClone;
  CellRef lastClone     = previousClone;

  for (std::size_t index = 0; index < ownerCount; ++index) {
    const CellRef owner  = owners[index];
    const auto projected = h.arena.linked(owner, tagDimension, DimVector::POS);
    ASSERT_TRUE(projected) << "owner " << index << " has no projected tag";
    const CellRef clone = *projected;
    EXPECT_TRUE(clones.insert(clone).second)
        << "owner " << index << " reused an existing target clone";
    EXPECT_EQ(h.arena.linked(clone, tagDimension, DimVector::NEG), owner);

    const CellRef expectedMaster =
        mode == TargetMode::Alias ? sharedTarget.master : sourceTargets[index];
    const CellRef expectedPredecessor = mode == TargetMode::Alias
                                            ? previousClone
                                            : ownerTargets[index].priorClone;
    EXPECT_EQ(h.arena.cloneMaster(clone, cloneDimension), expectedMaster);
    EXPECT_EQ(h.arena.linked(clone, cloneDimension, DimVector::NEG),
              expectedPredecessor);
    EXPECT_EQ(h.arena.linked(expectedMaster, tagDimension, DimVector::NEG),
              mode == TargetMode::Alias ? sharedTarget.tagAnchor
                                        : ownerTargets[index].tagAnchor);
    EXPECT_EQ(
        h.arena.linked(expectedMaster, protectedDimension, DimVector::POS),
        mode == TargetMode::Alias ? sharedTarget.protectedNeighbor
                                  : ownerTargets[index].protectedNeighbor);
    EXPECT_EQ(h.arena.textOf(expectedMaster),
              mode == TargetMode::Alias
                  ? "shared target"
                  : "owner target " + std::to_string(index));

    EXPECT_FALSE(h.arena.linked(owner, oldTagDimension, DimVector::POS));
    EXPECT_FALSE(
        h.arena.linked(sourceTargets[index], oldTagDimension, DimVector::NEG));
    EXPECT_FALSE(h.arena.linked(owner, keepDimension, DimVector::POS));
    EXPECT_FALSE(
        h.arena.linked(keepTargets[index], keepDimension, DimVector::NEG));
    EXPECT_FALSE(h.arena.linked(owner, dropDimension, DimVector::POS));
    EXPECT_FALSE(
        h.arena.linked(dropTargets[index], dropDimension, DimVector::NEG));
    EXPECT_FALSE(
        h.arena.linked(displacedTargets[index], tagDimension, DimVector::NEG));

    if (mode == TargetMode::Alias) previousClone = clone;
    lastClone = clone;
  }
  EXPECT_EQ(clones.size(), ownerCount);
  EXPECT_FALSE(h.arena.linked(lastClone, cloneDimension, DimVector::POS));

  std::cout << "VQL_PROJECTION_TARGET_PROFILE mode=" << targetModeName(mode)
            << " owners=" << ownerCount
            << " compile_us=" << compileElapsed.count()
            << " run_us=" << runElapsed.count() << " vm_cycles=" << cyclesUsed
            << " compile_cell_delta=" << cellsAfterCompile - cellsBeforeCompile
            << " run_cell_delta=" << cellsAfterRun - cellsAfterCompile
            << " emitted_opcodes=" << program.generatedOpcodes.size() << '\n';
}

TEST(VQLProjectionTargetsProfileTest, DISABLED_TargetScaling) {
  for (const std::size_t ownerCount : {32U, 128U}) {
    runTargetProfile(ownerCount, TargetMode::Alias);
    runTargetProfile(ownerCount, TargetMode::OwnerPath);
  }
}

} // namespace
} // namespace xanadu::vql
