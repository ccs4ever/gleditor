#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/contract_codec.hpp"
#include "common/xanadu/vql/contracts.hpp"

namespace {
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct Program {
  CellRef opcode;
  CellRef cursor;
  std::optional<CellRef> matched;
  std::vector<CellRef> captures;
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  xanadu::MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  Harness() { vm.setContractCoordinator(coordinator); }
  Harness(const zigzag::Manifold &base, const xanadu::Store &store)
      : arena(&base, store) {
    vm.setContractCoordinator(coordinator);
  }

  Program compile(const std::string_view syntax, const bool required = false) {
    const auto descriptor =
        std::string("VQL_CONTRACT/3\n") + std::string(syntax);
    const auto type   = xanadu::vql::decodeTypeDescriptor(descriptor);
    const auto opcode = vm.mintOpcode(required ? OpcodeKind::RequireContract
                                               : OpcodeKind::MatchContract);
    const auto halt   = vm.mintOpcode(OpcodeKind::Halt);
    zigzag::expectWritten(
        arena.link(opcode, core.dims().spin, DimVector::POS, halt));
    core.bindInput(opcode, arena.makeCell(descriptor));
    if (required) core.bindInput(opcode, arena.makeCell("checked"));
    core.bindInput(opcode, arena.makeScalarCell(std::int64_t{1}));
    core.bindInput(opcode, core.home());
    Program program{.opcode = opcode,
                    .cursor = vm.spawnCursor(opcode, "atomic-contract")};
    if (!required) {
      program.matched = arena.makeCell();
      core.bindOutput(opcode, *program.matched);
      for (const auto &name :
           xanadu::vql::ContractMatcher::contractCaptureNames(type)) {
        (void)name;
        const auto capture = arena.makeCell();
        core.bindOutput(opcode, capture);
        program.captures.push_back(capture);
      }
    }
    return program;
  }

  zigzag::Mark snapshot() {
    const auto state = arena.mark();
    arena.discard(state);
    return state;
  }

  void expectStorageRestored(const zigzag::Mark &before) {
    const auto after = snapshot();
    EXPECT_EQ(after.cellCount, before.cellCount);
    EXPECT_EQ(after.scratchSize, before.scratchSize);
    EXPECT_EQ(arena.outstandingMarks(), 0u);
  }
};

TEST(VQLContractAtomicityTest, FailedMatchDiscardsDefaultsAndMintedDimensions) {
  Harness h;
  const auto program =
      h.compile("Cell { +d.default: $value: Int := 7, +d.required: Int }");
  const auto before    = h.snapshot();
  const auto execution = h.vm.step(program.cursor);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.arena.asBool(*program.matched), false);
  ASSERT_EQ(program.captures.size(), 1u);
  EXPECT_TRUE(h.vm.resultCells(program.captures.front()).empty());
  h.expectStorageRestored(before);
  EXPECT_FALSE(h.core.findDimension("d.default"));
  EXPECT_FALSE(h.core.findDimension("d.required"));

  const auto dimension = h.coordinator.resolveDimension("d.required");
  ASSERT_TRUE(h.arena.contains(dimension));
  const auto value = h.arena.makeScalarCell(std::int64_t{9});
  zigzag::expectWritten(
      h.arena.link(h.core.home(), dimension, DimVector::POS, value));
  h.vm.setCursorOpcode(program.cursor, program.opcode);
  const auto retried = h.vm.step(program.cursor);
  ASSERT_TRUE(retried.success) << retried.errorMessage;
  EXPECT_EQ(h.arena.asBool(*program.matched), true);
  const auto captured = h.vm.resultCell(program.captures.front());
  ASSERT_TRUE(captured);
  EXPECT_EQ(h.arena.asInt64(*captured), 7);
  EXPECT_TRUE(h.core.findDimension("d.default"));
}

TEST(VQLContractAtomicityTest, SuccessfulUnionDropsFailedArmDefaults) {
  Harness h;
  const auto program =
      h.compile("Cell { +d.failed: $value: Int := 7, +d.required: Int } | "
                "Cell { +d.chosen: $value: Int := 8 }");
  const auto execution = h.vm.step(program.cursor);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.arena.asBool(*program.matched), true);
  EXPECT_FALSE(h.core.findDimension("d.failed"));
  EXPECT_FALSE(h.core.findDimension("d.required"));
  const auto chosen = h.core.findDimension("d.chosen");
  ASSERT_TRUE(chosen);
  EXPECT_TRUE(h.arena.contains(*chosen));
  ASSERT_EQ(program.captures.size(), 1u);
  const auto captured = h.vm.resultCell(program.captures.front());
  ASSERT_TRUE(captured);
  EXPECT_EQ(h.arena.asInt64(*captured), 8);
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VQLContractAtomicityTest,
     FallbackWithoutCapturesLeavesNoSpeculativeCells) {
  Harness h;
  const auto program = h.compile(
      "Cell { +d.failed: $value: Int := 7, +d.required: Int } | Cell");
  const auto before    = h.snapshot();
  const auto execution = h.vm.step(program.cursor);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.arena.asBool(*program.matched), true);
  EXPECT_TRUE(h.vm.resultCells(program.captures.front()).empty());
  h.expectStorageRestored(before);
  EXPECT_FALSE(h.core.findDimension("d.failed"));
}

TEST(VQLContractAtomicityTest, ReturnChecksNeverRetainValidationDefaults) {
  for (const bool expectedMatch : {false, true}) {
    Harness h;
    const auto syntax =
        expectedMatch
            ? "Cell { +d.default: $value: Int := 7 }"
            : "Cell { +d.default: $value: Int := 7, +d.required: Int }";
    const auto program   = h.compile(syntax, true);
    const auto before    = h.snapshot();
    const auto execution = h.vm.step(program.cursor);
    EXPECT_EQ(execution.success, expectedMatch) << execution.errorMessage;
    if (!expectedMatch) {
      EXPECT_EQ(execution.contractViolation,
                zigzag::vortex::ContractViolationKind::Postcondition);
    }
    h.expectStorageRestored(before);
    EXPECT_FALSE(h.core.findDimension("d.default"));
    EXPECT_FALSE(h.core.findDimension("d.required"));
  }
}

TEST(VQLContractAtomicityTest, AllocationFailureAfterFirstDefaultCanRetry) {
  Harness h;
  (void)h.coordinator.resolveDimension("d.first");
  (void)h.coordinator.resolveDimension("d.second");
  const auto program = h.compile(
      "Cell { +d.first: $first: Int := 1, +d.second: $second: Int := 2 }");
  const auto before = h.snapshot();
  h.arena.setAllocationLimitForTesting(
      static_cast<std::uint32_t>(before.cellCount + 1));
  const auto failed = h.vm.step(program.cursor);
  EXPECT_FALSE(failed.success);
  EXPECT_NE(failed.errorMessage.find("cannot allocate"), std::string::npos);
  h.expectStorageRestored(before);
  for (const auto capture : program.captures)
    EXPECT_TRUE(h.vm.resultCells(capture).empty());

  h.arena.setAllocationLimitForTesting(before.cellCount + 100);
  const auto retried = h.vm.step(program.cursor);
  ASSERT_TRUE(retried.success) << retried.errorMessage;
  EXPECT_EQ(h.arena.asBool(*program.matched), true);
  ASSERT_EQ(program.captures.size(), 2u);
  for (std::size_t i = 0; i < program.captures.size(); ++i) {
    const auto captured = h.vm.resultCell(program.captures[i]);
    ASSERT_TRUE(captured);
    EXPECT_EQ(h.arena.asInt64(*captured), static_cast<std::int64_t>(i + 1));
  }
}

TEST(VQLContractAtomicityTest, FailedCheckDoesNotRestoreSpentFuel) {
  Harness h;
  (void)h.coordinator.resolveDimension("d.first");
  (void)h.coordinator.resolveDimension("d.second");
  constexpr std::string_view syntax =
      "Cell { +d.first: $first: Int := 1, +d.second: $second: Int := 2 }";
  const auto program = h.compile(syntax);
  const auto before  = h.snapshot();
  const auto descriptorWork =
      std::string_view("VQL_CONTRACT/3\n").size() + syntax.size();
  // Two subject charges, the shape and first field visits, its default cell,
  // and its Int check use this fuel; the next field must exhaust it.
  constexpr std::size_t firstDefaultWork = 6;
  h.arena.setExecutionLimits(before.cellCount + 100,
                             descriptorWork + firstDefaultWork, 1000000);
  const auto execution = h.vm.step(program.cursor);
  EXPECT_FALSE(execution.success);
  EXPECT_NE(execution.errorMessage.find("work budget exhausted"),
            std::string::npos);
  h.expectStorageRestored(before);
  for (const auto capture : program.captures)
    EXPECT_TRUE(h.vm.resultCells(capture).empty());
  EXPECT_THROW(h.arena.consumeWork(), std::runtime_error);
}

TEST(VQLContractAtomicityTest, FailedMatchLeavesOverlaidStoreUnchanged) {
  struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("vql_contract_atomic_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TemporaryDirectory() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  xanadu::Store store(scroll);
  auto version         = store.sliceGenesis(store.latest());
  const auto dimension = store.makeDimension(version, "d.original");
  version              = store.makeCell(dimension.version, "original content");
  const auto value     = store.cellRefOf(version);
  ASSERT_TRUE(value);
  const auto home = store.homeCell();
  ASSERT_TRUE(home);
  version =
      store.setLink(version, *home, dimension.dim, DimVector::POS, *value);
  const auto latest        = store.latest();
  const auto structureHead = store.structureHead();
  const auto base          = store.rebuildManifold(version);
  Harness h(base, store);
  const auto program =
      h.compile("Cell { +d.default: $value: Int := 7, +d.required: Int }");
  const auto before    = h.snapshot();
  const auto execution = h.vm.step(program.cursor);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.arena.asBool(*program.matched), false);
  h.expectStorageRestored(before);
  EXPECT_EQ(store.latest(), latest);
  EXPECT_EQ(store.structureHead(), structureHead);
  const auto rebuilt = store.rebuildManifold(version);
  EXPECT_EQ(rebuilt.linked(*home, dimension.dim, DimVector::POS), value);
  EXPECT_EQ(rebuilt.textOf(*value, store), "original content");
  EXPECT_EQ(h.arena.textOf(*value), "original content");
}

TEST(VQLContractAtomicityTest,
     UnsupportedCompositionsRefuseWithoutChangingOutputs) {
  for (const bool attachOutputPipeline : {false, true}) {
    Harness h;
    const auto program = h.compile("Cell { +d.default: $value: Int := 7 }");
    const auto seeded  = h.vm.step(program.cursor);
    ASSERT_TRUE(seeded.success) << seeded.errorMessage;
    ASSERT_EQ(h.arena.asBool(*program.matched), true);
    const auto capture = h.vm.resultCell(program.captures.front());
    ASSERT_TRUE(capture);
    const auto captureBits = h.arena.slot(program.captures.front())->valueBits;
    const auto clause      = h.vm.mintOpcode(OpcodeKind::Nop);
    if (attachOutputPipeline)
      h.core.attachPipeline(*program.matched, clause);
    else
      h.core.attachPostcondition(program.opcode, clause);
    h.vm.setCursorOpcode(program.cursor, program.opcode);
    const auto before    = h.snapshot();
    const auto execution = h.vm.step(program.cursor);
    EXPECT_FALSE(execution.success);
    EXPECT_NE(execution.errorMessage.find("do not support"), std::string::npos);
    EXPECT_EQ(h.arena.asBool(*program.matched), true);
    EXPECT_EQ(h.vm.resultCell(program.captures.front()), capture);
    EXPECT_EQ(h.arena.slot(program.captures.front())->valueBits, captureBits);
    h.expectStorageRestored(before);
  }
}

TEST(VQLContractAtomicityTest, DuplicateOutputRefusalPreservesPriorRegisters) {
  Harness h;
  const auto program = h.compile("Cell { +d.value: $value: Int := 7 }");
  const auto seeded  = h.vm.step(program.cursor);
  ASSERT_TRUE(seeded.success) << seeded.errorMessage;
  ASSERT_EQ(h.arena.asBool(*program.matched), true);
  const auto capture = h.vm.resultCell(program.captures.front());
  ASSERT_TRUE(capture);
  const auto captureBits = h.arena.slot(program.captures.front())->valueBits;

  h.core.bindOutput(program.opcode, program.captures.front());
  h.vm.setCursorOpcode(program.cursor, program.opcode);
  const auto before    = h.snapshot();
  const auto malformed = h.vm.step(program.cursor);
  EXPECT_FALSE(malformed.success);
  EXPECT_NE(malformed.errorMessage.find("distinct scalar cells"),
            std::string::npos);
  EXPECT_EQ(h.arena.asBool(*program.matched), true);
  EXPECT_EQ(h.vm.resultCell(program.captures.front()), capture);
  EXPECT_EQ(h.arena.slot(program.captures.front())->valueBits, captureBits);
  h.expectStorageRestored(before);
}

TEST(VQLContractAtomicityTest, EmptyOutputRefusalIsSafeAndNonmutating) {
  Harness h;
  const auto descriptor = h.arena.makeCell("VQL_CONTRACT/3\nCell");
  const auto count      = h.arena.makeScalarCell(std::int64_t{1});
  const auto opcode     = h.vm.mintOpcode(OpcodeKind::MatchContract);
  const auto halt       = h.vm.mintOpcode(OpcodeKind::Halt);
  zigzag::expectWritten(
      h.arena.link(opcode, h.core.dims().spin, DimVector::POS, halt));
  h.core.bindInput(opcode, descriptor);
  h.core.bindInput(opcode, count);
  h.core.bindInput(opcode, h.core.home());
  const auto cursor = h.vm.spawnCursor(opcode, "empty-contract-outputs");
  const auto before = h.snapshot();

  const auto malformed = h.vm.step(cursor);
  EXPECT_FALSE(malformed.success);
  EXPECT_NE(malformed.errorMessage.find("output slots are missing"),
            std::string::npos);
  h.expectStorageRestored(before);
  EXPECT_EQ(h.vm.choiceDepth(), 0u);
}

TEST(VQLContractAtomicityTest, CaptureArityRefusalPreservesPriorRegisters) {
  Harness h;
  const auto program = h.compile("Cell { +d.value: $value: Int := 7 }");
  const auto seeded  = h.vm.step(program.cursor);
  ASSERT_TRUE(seeded.success) << seeded.errorMessage;
  ASSERT_EQ(h.arena.asBool(*program.matched), true);
  const auto capture = h.vm.resultCell(program.captures.front());
  ASSERT_TRUE(capture);
  const auto captureBits = h.arena.slot(program.captures.front())->valueBits;

  const auto descriptor = h.core.inputsOf(program.opcode).front();
  h.core.writeValue(descriptor, 0, std::nullopt,
                    zigzag::vortex::CellValue{"VQL_CONTRACT/3\nCell"});
  h.vm.setCursorOpcode(program.cursor, program.opcode);
  const auto before    = h.snapshot();
  const auto malformed = h.vm.step(program.cursor);
  EXPECT_FALSE(malformed.success);
  EXPECT_NE(malformed.errorMessage.find("capture slots disagree"),
            std::string::npos);
  EXPECT_EQ(h.arena.asBool(*program.matched), true);
  EXPECT_EQ(h.vm.resultCell(program.captures.front()), capture);
  EXPECT_EQ(h.arena.slot(program.captures.front())->valueBits, captureBits);
  h.expectStorageRestored(before);
}
} // namespace
