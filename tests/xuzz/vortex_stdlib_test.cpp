/**
 * @file vortex_stdlib_test.cpp
 * @brief Unit tests for the Vortex Standard Library in Vortex (Stage 2).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/stdlib_image_format.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vortex/vortex_worker_pool.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::noCell;
using namespace zigzag::vortex;

struct TestHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm};
};

TEST(VortexStdLibTest, BootstrapAndModuleDiscovery) {
  TestHarness h;
  std::vector<std::string> mods = h.stdlib.modules();
  EXPECT_GE(mods.size(), 7u);

  EXPECT_TRUE(h.stdlib.has("std:memoize"));
  EXPECT_TRUE(h.stdlib.has("std:contract"));
  EXPECT_TRUE(h.stdlib.has("std:pipeline"));
  EXPECT_TRUE(h.stdlib.has("std:functional"));
  EXPECT_TRUE(h.stdlib.has("std:collections"));
  EXPECT_TRUE(h.stdlib.has("std:math"));
  EXPECT_TRUE(h.stdlib.has("std:string"));
  EXPECT_TRUE(h.stdlib.has("std:env"));
  EXPECT_TRUE(h.stdlib.has("std:io"));
  EXPECT_TRUE(h.stdlib.has("std:map"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime"));
  EXPECT_TRUE(h.stdlib.has("std:zigzag"));

  EXPECT_FALSE(h.stdlib.has("std:nonexistent"));
  EXPECT_FALSE(h.stdlib.has("std:io/write_file"));
  EXPECT_FALSE(h.stdlib.has("std:io/write"));
}

TEST(VortexStdLibTest, ManualBuilderWritesPrivateImageManifest) {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm, VortexStdLib::Bootstrap::Manual};
  EXPECT_TRUE(stdlib.modules().empty());

  const auto beforeBegin = arena.cellCount();
  EXPECT_FALSE(stdlib.endImageManifest());
  EXPECT_EQ(arena.cellCount(), beforeBegin);
  const auto blockedBegin = arena.mark();
  EXPECT_FALSE(stdlib.beginImageOps());
  EXPECT_EQ(arena.cellCount(), beforeBegin);
  arena.release(blockedBegin);
  ASSERT_TRUE(stdlib.beginImageOps());
  EXPECT_FALSE(stdlib.beginImageOps());
  const auto beforeRefusal = arena.cellCount();
  EXPECT_FALSE(stdlib.buildModule("std:goal"));
  EXPECT_EQ(arena.cellCount(), beforeRefusal);
  EXPECT_FALSE(stdlib.buildModule("std:missing"));
  EXPECT_EQ(arena.cellCount(), beforeRefusal);

  const CellRef data = arena.makeScalarCell(std::int64_t{1});
  const auto math    = stdlib.buildModule("std:math");
  ASSERT_TRUE(math);
  EXPECT_FALSE(stdlib.buildModule("std:math"));
  const auto blockedEnd       = arena.mark();
  const auto beforeBlockedEnd = arena.cellCount();
  EXPECT_FALSE(stdlib.buildModule("std:string"));
  EXPECT_FALSE(stdlib.endImageManifest());
  EXPECT_EQ(arena.cellCount(), beforeBlockedEnd);
  arena.release(blockedEnd);
  ASSERT_TRUE(stdlib.endImageManifest());
  EXPECT_FALSE(stdlib.endImageManifest());
  EXPECT_FALSE(stdlib.buildModule("std:string"));

  const auto manifestKey =
      zigzag::firstOf(zigzag::rankAfter(arena, core.home(), core.dims().vars) |
                      std::views::filter([&](CellRef cell) {
                        return arena.textOf(cell) == "stdlib.image";
                      }));
  ASSERT_TRUE(manifestKey);
  const auto holder =
      arena.linked(*manifestKey, core.dims().values, zigzag::DimVector::POS);
  ASSERT_TRUE(holder);
  EXPECT_EQ(std::ranges::distance(
                zigzag::rankAfter(arena, *holder, core.dims().vars)),
            8);
  EXPECT_EQ(std::ranges::distance(
                zigzag::rankAfter(arena, core.home(), core.dims().vars)),
            1);
  for (const CellRef record :
       zigzag::rankAfter(arena, *holder, core.dims().vars)) {
    const auto value =
        arena.linked(record, core.dims().values, zigzag::DimVector::POS);
    ASSERT_TRUE(value);
    if (arena.textOf(record) == "imageOpcodeCount") {
      EXPECT_EQ(arena.asInt64(*value),
                static_cast<std::int64_t>(stdlibImageOpcodeCount()));
    } else if (arena.textOf(record) == "imageEpoch") {
      EXPECT_EQ(arena.textOf(*value), stdlibImageEpoch());
    }
  }

  std::optional<CellRef> opsRecord;
  for (const CellRef record :
       zigzag::rankAfter(arena, *holder, core.dims().vars)) {
    if (arena.textOf(record) == "ops") opsRecord = record;
  }
  ASSERT_TRUE(opsRecord);
  const auto opsHead =
      arena.linked(*opsRecord, core.dims().values, zigzag::DimVector::POS);
  ASSERT_TRUE(opsHead);
  bool sawRegisteredOpcode = false;
  for (const CellRef member :
       zigzag::rankAfter(arena, *opsHead, core.dims().step)) {
    const auto position = arena.asInt64(member);
    ASSERT_TRUE(position);
    const CellRef opcode = static_cast<CellRef>(*position);
    EXPECT_TRUE(vm.hasOpcodeKind(opcode));
    sawRegisteredOpcode = true;
  }
  EXPECT_TRUE(sawRegisteredOpcode);
  EXPECT_FALSE(vm.hasOpcodeKind(data));
}

TEST(VortexStdLibTest, RebindRestoresClampSignatureWithoutMinting) {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM originalVm{core};
  VortexStdLib builder{core, originalVm, VortexStdLib::Bootstrap::Manual};
  ASSERT_TRUE(builder.beginImageOps());
  ASSERT_TRUE(builder.buildModule("std:math"));
  ASSERT_TRUE(builder.endImageManifest());
  const CellRef clamp = *builder.resolve("std:math/clamp");
  const std::vector<CellRef> inputs(builder.routineInputs(clamp).begin(),
                                    builder.routineInputs(clamp).end());
  const std::vector<CellRef> outputs(builder.routineOutputs(clamp).begin(),
                                     builder.routineOutputs(clamp).end());
  ASSERT_EQ(inputs.size(), 3U);
  ASSERT_EQ(outputs.size(), 1U);

  VortexVM freshVm{core};
  VortexStdLib rebound{core, freshVm, VortexStdLib::Bootstrap::Manual};
  const auto cellsBeforeRebind = arena.cellCount();
  ASSERT_TRUE(rebound.rebindInstance());
  EXPECT_EQ(arena.cellCount(), cellsBeforeRebind);
  EXPECT_TRUE(freshVm.hasOpcodeKind(clamp));
  EXPECT_EQ(std::vector<CellRef>(rebound.routineInputs(clamp).begin(),
                                 rebound.routineInputs(clamp).end()),
            inputs);
  EXPECT_EQ(std::vector<CellRef>(rebound.routineOutputs(clamp).begin(),
                                 rebound.routineOutputs(clamp).end()),
            outputs);
  EXPECT_EQ(rebound.call("std:math/clamp", {std::int64_t{150}, std::int64_t{0},
                                            std::int64_t{100}}),
            (std::vector<CellValue>{std::int64_t{100}}));
}

TEST(VortexStdLibTest, RebindAllowsHydrationFloorButRefusesRealMarks) {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM originalVm{core};
  VortexStdLib builder{core, originalVm, VortexStdLib::Bootstrap::Manual};
  ASSERT_TRUE(builder.beginImageOps());
  ASSERT_TRUE(builder.buildModule("std:math"));
  ASSERT_TRUE(builder.endImageManifest());
  const auto prefixMark = arena.mark();
  const auto prefix     = arena.exportPrefix(prefixMark);
  arena.discard(prefixMark);
  ASSERT_TRUE(arena.hydratePrefix(prefix));
  EXPECT_EQ(arena.outstandingMarks(), 1U);
  EXPECT_EQ(arena.liveMarks(), 0U);

  VortexVM vm{core};
  VortexStdLib rebound{core, vm, VortexStdLib::Bootstrap::Manual};
  const auto before = arena.cellCount();
  ASSERT_TRUE(rebound.rebindInstance());
  EXPECT_EQ(arena.cellCount(), before);
  const auto mark = arena.mark();
  EXPECT_FALSE(rebound.rebindInstance());
  EXPECT_EQ(arena.cellCount(), before);
  arena.release(mark);
  arena.evictOldestMark();
}

TEST(VortexStdLibTest, RebindRestoresGoalCachesAndRefusesUnsafeMetadata) {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM builderVm{core};
  VortexStdLib builder{core, builderVm, VortexStdLib::Bootstrap::Manual};
  ASSERT_TRUE(builder.beginImageOps());
  ASSERT_TRUE(builder.buildModule("std:logic"));
  ASSERT_TRUE(builder.buildModule("std:goal"));
  ASSERT_TRUE(builder.endImageManifest());

  VortexVM vm{core};
  VortexStdLib rebound{core, vm, VortexStdLib::Bootstrap::Manual};
  ASSERT_TRUE(rebound.rebindInstance());
  ASSERT_TRUE(rebound.resolve("std:goal/halt"));
  ASSERT_TRUE(rebound.resolve("std:goal/fail"));
  ASSERT_TRUE(rebound.resolve("std:goal/sentinel"));
  EXPECT_EQ(rebound.goalHaltOp(), *rebound.resolve("std:goal/halt"));
  EXPECT_EQ(rebound.goalFailOp(), *rebound.resolve("std:goal/fail"));
  EXPECT_EQ(rebound.goalSentinelOp(), *rebound.resolve("std:goal/sentinel"));
  for (const auto getter :
       {&VortexStdLib::predicateEqual, &VortexStdLib::predicateUnify,
        &VortexStdLib::predicateMember, &VortexStdLib::predicateAppend,
        &VortexStdLib::predicateLength, &VortexStdLib::predicateSetting,
        &VortexStdLib::predicateSettingShape,
        &VortexStdLib::predicateSettingDefault,
        &VortexStdLib::predicateCellValue, &VortexStdLib::predicateCellLink,
        &VortexStdLib::predicateTransclude, &VortexStdLib::predicateXanalink,
        &VortexStdLib::predicateCellSpan, &VortexStdLib::predicateBridgeEdge}) {
    EXPECT_EQ((rebound.*getter)(), (builder.*getter)());
  }

  const CellRef cachedHalt = rebound.goalHaltOp();
  const CellRef fromEntry  = *rebound.resolve("std:goal/from");
  const std::vector<CellRef> fromInputs(
      rebound.routineInputs(fromEntry).begin(),
      rebound.routineInputs(fromEntry).end());
  const CellRef manifestKey =
      *zigzag::firstOf(zigzag::rankAfter(arena, core.home(), core.dims().vars) |
                       std::views::filter([&](CellRef cell) {
                         return arena.textOf(cell) == "stdlib.image";
                       }));
  const CellRef holder =
      *arena.linked(manifestKey, core.dims().values, zigzag::DimVector::POS);
  std::optional<CellRef> opsRecord;
  for (const CellRef record :
       zigzag::rankAfter(arena, holder, core.dims().vars)) {
    if (arena.textOf(record) == "ops") opsRecord = record;
  }
  ASSERT_TRUE(opsRecord);
  const CellRef opsHead =
      *arena.linked(*opsRecord, core.dims().values, zigzag::DimVector::POS);
  const CellRef firstMember =
      *arena.linked(opsHead, core.dims().step, zigzag::DimVector::POS);
  ASSERT_TRUE(arena.setValueBits(firstMember, xanadu::ValueKind::Int64,
                                 std::int64_t{999999}));
  const auto cellsBeforeRefusal = arena.cellCount();
  EXPECT_FALSE(rebound.rebindInstance());
  EXPECT_EQ(rebound.goalHaltOp(), cachedHalt);
  EXPECT_EQ(std::vector<CellRef>(rebound.routineInputs(fromEntry).begin(),
                                 rebound.routineInputs(fromEntry).end()),
            fromInputs);
  EXPECT_EQ(arena.cellCount(), cellsBeforeRefusal);

  const auto mark = arena.mark();
  EXPECT_FALSE(rebound.rebindInstance());
  EXPECT_EQ(rebound.goalHaltOp(), cachedHalt);
  EXPECT_EQ(std::vector<CellRef>(rebound.routineInputs(fromEntry).begin(),
                                 rebound.routineInputs(fromEntry).end()),
            fromInputs);
  EXPECT_EQ(arena.cellCount(), cellsBeforeRefusal);
  arena.release(mark);
}

TEST(VortexStdLibTest, FromAndSeqBothKeepCallableEntriesAfterRebind) {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM builderVm{core};
  VortexStdLib builder{core, builderVm};
  VortexVM vm{core};
  VortexStdLib rebound{core, vm, VortexStdLib::Bootstrap::Manual};
  ASSERT_TRUE(rebound.rebindInstance());
  for (const auto path : {"std:goal/from", "std:goal/seq"}) {
    const auto entry = rebound.resolve(path);
    ASSERT_TRUE(entry) << path;
    const auto inputs  = rebound.routineInputs(*entry);
    const auto outputs = rebound.routineOutputs(*entry);
    ASSERT_EQ(inputs.size(), 4U);
    ASSERT_EQ(outputs.size(), 1U);
    ASSERT_TRUE(arena.setValueBits(inputs[0], xanadu::ValueKind::Int64, 10));
    ASSERT_TRUE(arena.setValueBits(inputs[1], xanadu::ValueKind::Int64, 2));
    EXPECT_EQ(
        rebound.goalBoundedFirst(*entry, outputs[0], rebound.goalHaltOp()),
        (std::optional<CellValue>{CellValue{std::int64_t{10}}}));
    EXPECT_EQ(arena.liveMarks(), 0U);
  }
}

TEST(VortexStdLibTest, GoalOutputAbsenceIsUnitAndValuesStayPresent) {
  TestHarness h;
  const auto makeProducer = [&h](const CellValue &value) {
    const auto makeValueCell = [&h](const CellValue &v) {
      return std::visit(
          [&h](const auto &scalar) -> CellRef {
            using T = std::decay_t<decltype(scalar)>;
            if constexpr (std::is_same_v<T, std::string>) {
              return h.arena.makeCell(scalar);
            } else {
              return h.arena.makeScalarCell(scalar);
            }
          },
          v);
    };
    OpcodeKind kind;
    std::vector<CellRef> inputs;
    if (const auto *boolean = std::get_if<bool>(&value)) {
      kind = OpcodeKind::Not;
      inputs.push_back(makeValueCell(CellValue{!*boolean}));
    } else if (const auto *integer = std::get_if<std::int64_t>(&value)) {
      kind = OpcodeKind::Sub;
      inputs.push_back(makeValueCell(CellValue{*integer}));
      inputs.push_back(makeValueCell(CellValue{std::int64_t{0}}));
    } else {
      kind = OpcodeKind::Add;
      inputs.push_back(makeValueCell(value));
      inputs.push_back(makeValueCell(CellValue{std::string{}}));
    }
    const CellRef output  = h.arena.makeCell();
    const CellRef produce = h.vm.mintOpcode(kind, "#TEST_PRODUCE");
    const CellRef suspend = h.vm.mintOpcode(OpcodeKind::Jump, "#TEST_SUSPEND");
    const CellRef target  = h.arena.makeScalarCell(
        static_cast<std::int64_t>(h.stdlib.goalHaltOp()));
    for (const auto input : inputs) h.core.bindInput(produce, input);
    h.core.bindOutput(produce, output);
    h.core.bindInput(suspend, target);
    EXPECT_TRUE(h.arena.link(produce, h.core.dims().spin,
                             zigzag::DimVector::POS, suspend));
    return std::pair{produce, output};
  };

  for (const CellValue &value : {CellValue{false}, CellValue{std::int64_t{0}},
                                 CellValue{std::string{}}}) {
    const auto [entry, out] = makeProducer(value);
    EXPECT_EQ(h.stdlib.goalBoundedFirst(entry, out, h.stdlib.goalHaltOp()),
              std::optional<CellValue>{value});
    EXPECT_EQ(h.stdlib.goalResults(entry, out, h.stdlib.goalHaltOp()),
              (std::vector<CellValue>{value}));
  }

  const CellValue unit{true};
  const auto [entry, ignored] = makeProducer(CellValue{std::int64_t{42}});
  (void)ignored;
  EXPECT_EQ(
      h.stdlib.goalBoundedFirst(entry, std::nullopt, h.stdlib.goalHaltOp()),
      std::optional<CellValue>{unit});
  EXPECT_EQ(h.stdlib.goalResults(entry, std::nullopt, h.stdlib.goalHaltOp()),
            (std::vector<CellValue>{unit}));
}

TEST(VortexStdLibTest, SpatialSymbolResolutionAndInvocation) {
  TestHarness h;
  EXPECT_TRUE(h.stdlib.has("std:math/abs"));
  EXPECT_TRUE(h.stdlib.has("std:math/min"));
  EXPECT_TRUE(h.stdlib.has("std:math/max"));
  EXPECT_TRUE(h.stdlib.has("std:math/clamp"));
  EXPECT_TRUE(h.stdlib.has("std:math/add"));
  EXPECT_TRUE(h.stdlib.has("std:math/mul"));

  // Call std:math/abs
  auto absRes = h.stdlib.call("std:math/abs", {static_cast<std::int64_t>(-99)});
  ASSERT_EQ(absRes.size(), 1u);
  EXPECT_EQ(absRes[0], CellValue(static_cast<std::int64_t>(99)));

  // Call std:math/min
  auto minRes = h.stdlib.call("std:math/min", {static_cast<std::int64_t>(15),
                                               static_cast<std::int64_t>(42)});
  ASSERT_EQ(minRes.size(), 1u);
  EXPECT_EQ(minRes[0], CellValue(static_cast<std::int64_t>(15)));

  // Call std:math/max
  auto maxRes = h.stdlib.call("std:math/max", {static_cast<std::int64_t>(15),
                                               static_cast<std::int64_t>(42)});
  ASSERT_EQ(maxRes.size(), 1u);
  EXPECT_EQ(maxRes[0], CellValue(static_cast<std::int64_t>(42)));

  // Call std:math/clamp
  auto clampRes =
      h.stdlib.call("std:math/clamp", {static_cast<std::int64_t>(150),
                                       static_cast<std::int64_t>(0),
                                       static_cast<std::int64_t>(100)});
  ASSERT_EQ(clampRes.size(), 1u);
  EXPECT_EQ(clampRes[0], CellValue(static_cast<std::int64_t>(100)));

  // Call std:math/add
  auto addRes = h.stdlib.call("std:math/add", {static_cast<std::int64_t>(25),
                                               static_cast<std::int64_t>(17)});
  ASSERT_EQ(addRes.size(), 1u);
  EXPECT_EQ(addRes[0], CellValue(static_cast<std::int64_t>(42)));

  // Call std:math/sub
  auto subRes = h.stdlib.call("std:math/sub", {static_cast<std::int64_t>(50),
                                               static_cast<std::int64_t>(8)});
  ASSERT_EQ(subRes.size(), 1u);
  EXPECT_EQ(subRes[0], CellValue(static_cast<std::int64_t>(42)));

  // Call std:math/div
  auto divRes = h.stdlib.call("std:math/div", {static_cast<std::int64_t>(84),
                                               static_cast<std::int64_t>(2)});
  ASSERT_EQ(divRes.size(), 1u);
  EXPECT_EQ(divRes[0], CellValue(static_cast<std::int64_t>(42)));

  // Call std:math/div by zero fails precondition contract -> empty result
  auto divZero = h.stdlib.call("std:math/div", {static_cast<std::int64_t>(42),
                                                static_cast<std::int64_t>(0)});
  EXPECT_TRUE(divZero.empty());

  // Call std:math/mod
  auto modRes = h.stdlib.call("std:math/mod", {static_cast<std::int64_t>(47),
                                               static_cast<std::int64_t>(5)});
  ASSERT_EQ(modRes.size(), 1u);
  EXPECT_EQ(modRes[0], CellValue(static_cast<std::int64_t>(2)));

  // Call std:math/neg
  auto negRes = h.stdlib.call("std:math/neg", {static_cast<std::int64_t>(42)});
  ASSERT_EQ(negRes.size(), 1u);
  EXPECT_EQ(negRes[0], CellValue(static_cast<std::int64_t>(-42)));
}

TEST(VortexStdLibTest, StringModuleOperations) {
  TestHarness h;
  EXPECT_TRUE(h.stdlib.has("std:string/to_lower"));
  EXPECT_TRUE(h.stdlib.has("std:string/to_upper"));
  EXPECT_TRUE(h.stdlib.has("std:string/trim"));

  auto upperRes =
      h.stdlib.call("std:string/to_upper", {std::string("hello vortex")});
  ASSERT_EQ(upperRes.size(), 1u);
  EXPECT_EQ(upperRes[0], CellValue(std::string("HELLO VORTEX")));

  auto lowerRes =
      h.stdlib.call("std:string/to_lower", {std::string("XANADU ZZSTRUCTURE")});
  ASSERT_EQ(lowerRes.size(), 1u);
  EXPECT_EQ(lowerRes[0], CellValue(std::string("xanadu zzstructure")));

  auto trimRes =
      h.stdlib.call("std:string/trim", {std::string("   padded text   ")});
  ASSERT_EQ(trimRes.size(), 1u);
  EXPECT_EQ(trimRes[0], CellValue(std::string("padded text")));

  auto cleanRes =
      h.stdlib.call("std:string/clean", {std::string("   HeLLo WoRLd   ")});
  ASSERT_EQ(cleanRes.size(), 1u);
  EXPECT_EQ(cleanRes[0], CellValue(std::string("hello world")));

  // Static string helpers
  EXPECT_TRUE(VortexStdLib::strStartsWith("hyperstructure", "hyper"));
  EXPECT_FALSE(VortexStdLib::strStartsWith("hyperstructure", "structure"));
  EXPECT_TRUE(VortexStdLib::strEndsWith("hyperstructure", "structure"));
  EXPECT_FALSE(VortexStdLib::strEndsWith("hyperstructure", "hyper"));

  auto parts = VortexStdLib::strSplit("alpha,beta,gamma", ",");
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], "alpha");
  EXPECT_EQ(parts[1], "beta");
  EXPECT_EQ(parts[2], "gamma");

  EXPECT_EQ(VortexStdLib::strJoin(parts, "::"), "alpha::beta::gamma");
}

TEST(VortexStdLibTest, MathModuleStaticHelpers) {
  EXPECT_EQ(VortexStdLib::mathGcd(48, 18), 6);
  EXPECT_EQ(VortexStdLib::mathLcm(4, 6), 12);
  EXPECT_DOUBLE_EQ(VortexStdLib::mathPow(2.0, 8.0), 256.0);
  EXPECT_DOUBLE_EQ(VortexStdLib::mathSqrt(144.0), 12.0);
}

TEST(VortexStdLibTest, ContractModulePreconditionsAndPostconditions) {
  TestHarness h;
  CellRef inParam   = h.arena.makeScalarCell(static_cast<std::int64_t>(-10));
  CellRef outTarget = h.arena.makeCell();

  // #ADD inParam 5 -> outTarget
  CellRef op   = h.vm.mintOpcode(OpcodeKind::Add, "#ADD");
  CellRef five = h.arena.makeScalarCell(static_cast<std::int64_t>(5));
  h.core.bindInput(op, inParam);
  h.core.bindInput(op, five);
  h.core.bindOutput(op, outTarget);

  // Attach require_positive contract
  CellRef req = h.stdlib.createRequirePositive(inParam);
  h.core.attachPrecondition(op, req);

  CellRef cursor = h.vm.spawnCursor(op, "contract_test");
  auto res       = h.vm.step(cursor);
  EXPECT_FALSE(res.success);
  EXPECT_EQ(res.contractViolation, ContractViolationKind::Precondition);

  // Now satisfy precondition with positive input
  h.core.writeValue(inParam, 0, std::nullopt, static_cast<std::int64_t>(20));
  CellRef cursor2 = h.vm.spawnCursor(op, "contract_pass");
  auto res2       = h.vm.step(cursor2);
  EXPECT_TRUE(res2.success);
  EXPECT_EQ(h.core.render(outTarget), CellValue(static_cast<std::int64_t>(25)));
}

TEST(VortexStdLibTest, PipelineModulePreAndPostprocessing) {
  TestHarness h;
  // inParam with surrounding whitespace
  CellRef inParam   = h.arena.makeCell("   test message   ");
  CellRef outTarget = h.arena.makeCell();

  // Attach #TRIM preprocessor to inParam
  CellRef trimPipe = h.stdlib.createTrimPipeline();
  h.core.attachPipeline(inParam, trimPipe);

  // Attach #TO_UPPER postprocessor to outTarget
  CellRef upperPipe = h.stdlib.createToUpperPipeline();
  h.core.attachPipeline(outTarget, upperPipe);

  // Main opcode: pass-through #ADD inParam ""
  CellRef op    = h.vm.mintOpcode(OpcodeKind::Add, "#MAIN");
  CellRef empty = h.arena.makeCell("");
  h.core.bindInput(op, inParam);
  h.core.bindInput(op, empty);
  h.core.bindOutput(op, outTarget);

  CellRef cursor = h.vm.spawnCursor(op, "pipe_test");
  auto res       = h.vm.step(cursor);
  EXPECT_TRUE(res.success);

  // inParam was trimmed to "test message"
  // outTarget was postprocessed to "TEST MESSAGE"
  EXPECT_EQ(h.core.render(outTarget), CellValue(std::string("TEST MESSAGE")));
}

TEST(VortexStdLibTest, MemoizeModuleTopologicalLifecycle) {
  TestHarness h;
  CellRef op = h.vm.mintOpcode(OpcodeKind::Mul, "#SQUARE");
  CellRef in = h.arena.makeScalarCell(static_cast<std::int64_t>(9));
  h.core.bindInput(op, in);
  h.core.bindInput(op, in);
  CellRef out = h.arena.makeCell();
  h.core.bindOutput(op, out);

  // Setup memoization via stdlib
  std::string_view cacheKey = "std:memo/square";
  CellRef pin               = h.stdlib.memoize(op, cacheKey);
  EXPECT_NE(pin, noCell);

  // Execute 1: Miss
  CellRef c1 = h.vm.spawnCursor(op, "c1");
  h.vm.step(c1);
  EXPECT_EQ(h.core.render(out), CellValue(static_cast<std::int64_t>(81)));
  EXPECT_EQ(h.stdlib.memoEntryCount(cacheKey), 1u);

  // Execute 2: Hit
  CellRef c2 = h.vm.spawnCursor(op, "c2");
  h.vm.step(c2);
  EXPECT_EQ(h.core.render(out), CellValue(static_cast<std::int64_t>(81)));
  EXPECT_EQ(h.stdlib.memoEntryCount(cacheKey), 1u);

  // Atomic flush
  EXPECT_TRUE(h.stdlib.flushMemo(cacheKey));
  EXPECT_EQ(h.stdlib.memoEntryCount(cacheKey), 0u);

  // Pin remains alive after flush
  EXPECT_TRUE(h.core.findPin(cacheKey).has_value());

  // Retirement
  EXPECT_TRUE(h.stdlib.retireMemo(cacheKey));
  EXPECT_FALSE(h.core.findPin(cacheKey).has_value());
}

TEST(VortexStdLibTest, CollectionsModuleListsAndSequences) {
  TestHarness h;
  std::vector<CellValue> items = {
      static_cast<std::int64_t>(10),
      static_cast<std::int64_t>(20),
      static_cast<std::int64_t>(30),
  };

  CellRef listHead = h.stdlib.createList(items).value();
  EXPECT_EQ(h.stdlib.listLength(listHead), 3u);

  // Push back 40
  h.stdlib.pushBack(listHead, static_cast<std::int64_t>(40));
  EXPECT_EQ(h.stdlib.listLength(listHead), 4u);

  // Push front 5
  CellRef newHead = h.stdlib.pushFront(listHead, static_cast<std::int64_t>(5));
  EXPECT_EQ(h.stdlib.listLength(newHead), 5u);

  // Pop back -> 40
  auto backVal = h.stdlib.popBack(newHead);
  ASSERT_TRUE(backVal.has_value());
  EXPECT_EQ(*backVal, CellValue(static_cast<std::int64_t>(40)));
  EXPECT_EQ(h.stdlib.listLength(newHead), 4u);

  // Pop front -> 5
  const auto restoredHead = h.stdlib.popFront(newHead);
  ASSERT_TRUE(restoredHead);
  EXPECT_EQ(h.stdlib.listLength(*restoredHead), 3u);

  // Convert to vector
  auto vec = h.stdlib.listToVector(*restoredHead);
  ASSERT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0], CellValue(static_cast<std::int64_t>(10)));
  EXPECT_EQ(vec[1], CellValue(static_cast<std::int64_t>(20)));
  EXPECT_EQ(vec[2], CellValue(static_cast<std::int64_t>(30)));
}

TEST(VortexStdLibTest, EmptyCollectionsAndInvalidReferencesStayDistinct) {
  TestHarness h;
  const DimRef dim = h.core.dims().step;
  const DimRef out = h.arena.makeCell("d.empty.out");

  EXPECT_EQ(h.stdlib.createList({}), std::nullopt);
  EXPECT_EQ(h.stdlib.arrayIota(0), std::nullopt);
  EXPECT_THROW(h.stdlib.arrayIota(0, dim, noCell), std::invalid_argument);
  const auto emptyGrid = h.stdlib.createGrid(0, 3, dim, out);
  EXPECT_FALSE(emptyGrid.head);
  EXPECT_EQ(emptyGrid.rows, 0U);
  EXPECT_EQ(emptyGrid.cols, 3U);

  const CellRef singleton =
      h.stdlib.createList({CellValue{std::int64_t{1}}}).value();
  EXPECT_EQ(h.stdlib.arrayTake(singleton, dim, 0), std::nullopt);
  EXPECT_EQ(h.stdlib.filter(singleton, dim, out,
                            [](const CellValue &) { return false; }),
            std::nullopt);
  EXPECT_THROW(h.stdlib.arrayTake(noCell, dim, 1), std::invalid_argument);
  EXPECT_THROW(h.stdlib.arrayDrop(noCell, dim, 0), std::invalid_argument);

  EXPECT_EQ(h.stdlib.scanPush(noCell), std::nullopt);
  EXPECT_EQ(h.stdlib.zzDuplicate(noCell), std::nullopt);
  EXPECT_EQ(h.stdlib.zzCloneToChain(singleton, noCell), std::nullopt);
  EXPECT_EQ(h.stdlib.hopHead(noCell, dim), std::nullopt);
  EXPECT_EQ(h.stdlib.importModuleFromStore(xanadu::Store{}), std::nullopt);

  EXPECT_EQ(h.vm.assembleSequence(std::span<const OpcodeKind>{}), std::nullopt);
}

TEST(VortexStdLibTest, CollectionsModuleAssociativeMaps) {
  TestHarness h;
  CellRef map = h.stdlib.createMap();
  EXPECT_NE(map, noCell);

  h.stdlib.mapSet(map, "name", std::string("Ted Nelson"));
  h.stdlib.mapSet(map, "year", static_cast<std::int64_t>(1960));
  h.stdlib.mapSet(map, "vision", std::string("Xanadu"));

  EXPECT_TRUE(h.stdlib.mapHas(map, "name"));
  EXPECT_TRUE(h.stdlib.mapHas(map, "year"));
  EXPECT_TRUE(h.stdlib.mapHas(map, "vision"));
  EXPECT_FALSE(h.stdlib.mapHas(map, "nonexistent"));

  auto nameVal = h.stdlib.mapGet(map, "name");
  ASSERT_TRUE(nameVal.has_value());
  EXPECT_EQ(*nameVal, CellValue(std::string("Ted Nelson")));

  auto yearVal = h.stdlib.mapGet(map, "year");
  ASSERT_TRUE(yearVal.has_value());
  EXPECT_EQ(*yearVal, CellValue(static_cast<std::int64_t>(1960)));

  auto keys = h.stdlib.mapKeys(map);
  EXPECT_EQ(keys.size(), 3u);

  // Update existing key
  h.stdlib.mapSet(map, "year", static_cast<std::int64_t>(1965));
  auto updatedYear = h.stdlib.mapGet(map, "year");
  ASSERT_TRUE(updatedYear.has_value());
  EXPECT_EQ(*updatedYear, CellValue(static_cast<std::int64_t>(1965)));
}

TEST(VortexStdLibTest, CollectionsModule2DGridsAndSpatialTransposition) {
  TestHarness h;
  DimRef dRow = h.arena.makeCell("d.row");
  DimRef dCol = h.arena.makeCell("d.col");

  // Create 3 rows x 4 cols grid
  const auto gridResult =
      h.stdlib.createGrid(3, 4, dRow, dCol, static_cast<std::int64_t>(0));
  ASSERT_TRUE(gridResult.head);
  EXPECT_EQ(gridResult.rows, 3U);
  EXPECT_EQ(gridResult.cols, 4U);
  CellRef grid = *gridResult.head;

  // Set cell at (1, 2) to 42
  h.stdlib.setGrid(grid, 1, 2, dRow, dCol, static_cast<std::int64_t>(42));
  EXPECT_EQ(h.stdlib.getGrid(grid, 1, 2, dRow, dCol),
            CellValue(static_cast<std::int64_t>(42)));

  // Spatial Transposition: simply swap dRow and dCol dimensions!
  // Cell at (1, 2) in original grid is at (2, 1) in transposed view!
  EXPECT_EQ(h.stdlib.getGrid(grid, 2, 1, dCol, dRow),
            CellValue(static_cast<std::int64_t>(42)));

  h.stdlib.setGrid(grid, 0, 0, dRow, dCol, false);
  EXPECT_EQ(h.stdlib.getGrid(grid, 0, 0, dRow, dCol), CellValue(false));
  EXPECT_EQ(h.stdlib.getGrid(grid, 3, 0, dRow, dCol), std::nullopt);
}

TEST(VortexStdLibTest, FunctionalModuleMapFilterFoldZip) {
  TestHarness h;
  DimRef inDim  = h.core.dims().step;
  DimRef outDim = h.arena.makeCell("d.out");

  std::vector<CellValue> nums = {
      static_cast<std::int64_t>(1), static_cast<std::int64_t>(2),
      static_cast<std::int64_t>(3), static_cast<std::int64_t>(4),
      static_cast<std::int64_t>(5),
  };
  CellRef listHead = h.stdlib.createList(nums, inDim).value();

  // Map: square (x * x)
  CellRef squaredHead = h.stdlib
                            .map(listHead, inDim, outDim,
                                 [](const CellValue &v) -> CellValue {
                                   std::int64_t x = std::get<std::int64_t>(v);
                                   return x * x;
                                 })
                            .value();
  auto squaredVec     = h.stdlib.listToVector(squaredHead, outDim);
  ASSERT_EQ(squaredVec.size(), 5u);
  EXPECT_EQ(squaredVec[0], CellValue(static_cast<std::int64_t>(1)));
  EXPECT_EQ(squaredVec[1], CellValue(static_cast<std::int64_t>(4)));
  EXPECT_EQ(squaredVec[2], CellValue(static_cast<std::int64_t>(9)));
  EXPECT_EQ(squaredVec[3], CellValue(static_cast<std::int64_t>(16)));
  EXPECT_EQ(squaredVec[4], CellValue(static_cast<std::int64_t>(25)));

  // Filter: evens only
  DimRef filterDim  = h.arena.makeCell("d.filter");
  CellRef evensHead = h.stdlib
                          .filter(squaredHead, outDim, filterDim,
                                  [](const CellValue &v) -> bool {
                                    return std::get<std::int64_t>(v) % 2 == 0;
                                  })
                          .value();
  auto evensVec     = h.stdlib.listToVector(evensHead, filterDim);
  ASSERT_EQ(evensVec.size(), 2u);
  EXPECT_EQ(evensVec[0], CellValue(static_cast<std::int64_t>(4)));
  EXPECT_EQ(evensVec[1], CellValue(static_cast<std::int64_t>(16)));

  // Fold: sum
  CellValue sumVal = h.stdlib.fold(
      evensHead, filterDim, static_cast<std::int64_t>(0),
      [](const CellValue &acc, const CellValue &cur) -> CellValue {
        return std::get<std::int64_t>(acc) + std::get<std::int64_t>(cur);
      });
  EXPECT_EQ(sumVal, CellValue(static_cast<std::int64_t>(20)));

  // Zip
  DimRef zipDim = h.arena.makeCell("d.zip");
  const auto zipped =
      h.stdlib.zip(squaredHead, evensHead, outDim, filterDim, zipDim);
  EXPECT_TRUE(zipped.has_value());
}

TEST(VortexStdLibTest, ArrayModuleOperations) {
  TestHarness h;
  EXPECT_TRUE(h.stdlib.has("sys:array"));
  EXPECT_TRUE(h.stdlib.has("sys:array/iota"));
  EXPECT_TRUE(h.stdlib.has("sys:array/shape"));
  EXPECT_TRUE(h.stdlib.has("sys:array/take"));
  EXPECT_TRUE(h.stdlib.has("sys:array/drop"));
  EXPECT_TRUE(h.stdlib.has("sys:array/reverse"));
  EXPECT_TRUE(h.stdlib.has("sys:array/tally"));

  DimRef d1 = h.arena.makeCell("d.1");

  // arrayIota: mint 5 cells on d1
  CellRef head5 = h.stdlib.arrayIota(5, d1).value();
  EXPECT_EQ(h.stdlib.arrayTally(head5, d1), 5u);

  auto shape5 = h.stdlib.arrayShape(head5, std::span<const DimRef>{&d1, 1});
  ASSERT_TRUE(shape5);
  ASSERT_EQ(shape5->size(), 1u);
  EXPECT_EQ((*shape5)[0], 5u);

  // arrayTake: first 3 cells
  CellRef take3 = h.stdlib.arrayTake(head5, d1, 3).value();
  EXPECT_EQ(h.stdlib.arrayTally(take3, d1), 3u);

  // arrayDrop: drop 2 cells -> 3 left
  const auto drop2 = h.stdlib.arrayDrop(head5, d1, 2);
  ASSERT_TRUE(drop2);
  EXPECT_EQ(h.stdlib.arrayTally(*drop2, d1), 3u);

  // arrayReverse: reverse 5 cells
  CellRef rev5 = h.stdlib.arrayReverse(head5, d1).value();
  EXPECT_EQ(h.stdlib.arrayTally(rev5, d1), 5u);

  // Check invocation via call()
  auto callIotaRes =
      h.stdlib.call("sys:array/iota", {static_cast<std::int64_t>(4),
                                       static_cast<std::int64_t>(d1)});
  ASSERT_EQ(callIotaRes.size(), 1u);
  CellRef callHead =
      static_cast<CellRef>(std::get<std::int64_t>(callIotaRes[0]));
  EXPECT_NE(callHead, noCell);
  EXPECT_EQ(h.stdlib.arrayTally(callHead, d1), 4u);

  auto callTallyRes =
      h.stdlib.call("sys:array/tally", {static_cast<std::int64_t>(callHead),
                                        static_cast<std::int64_t>(d1)});
  ASSERT_EQ(callTallyRes.size(), 1u);
  EXPECT_EQ(callTallyRes[0], CellValue(static_cast<std::int64_t>(4)));
}

TEST(VortexStdLibTest, ArrayShapeAndTallyDistinguishInvalidOrigins) {
  TestHarness h;
  const DimRef dim = h.arena.makeCell("d.test-array");

  EXPECT_EQ(h.stdlib.arrayShape(noCell, std::span<const DimRef>{&dim, 1}),
            std::nullopt);
  EXPECT_EQ(h.stdlib.arrayTally(noCell, dim), std::nullopt);
  const auto noCellTally =
      h.stdlib.call("sys:array/tally", {static_cast<std::int64_t>(noCell),
                                        static_cast<std::int64_t>(dim)});
  ASSERT_EQ(noCellTally.size(), 1U);
  EXPECT_EQ(noCellTally.front(), CellValue(std::int64_t{0}));
  EXPECT_THROW(
      h.stdlib.call("sys:array/tally", {static_cast<std::int64_t>(999999),
                                        static_cast<std::int64_t>(dim)}),
      std::invalid_argument);
  EXPECT_THROW(h.stdlib.call("sys:array/tally", {}), std::invalid_argument);

  const CellRef head = h.stdlib.arrayIota(1, dim).value();
  const auto shape =
      h.stdlib.arrayShape(head, std::span<const DimRef>{&dim, 1});
  ASSERT_TRUE(shape);
  ASSERT_EQ(shape->size(), 1u);
  EXPECT_EQ((*shape)[0], 1u);
  EXPECT_EQ(h.stdlib.arrayTally(head, dim), 1u);
}

TEST(VortexStdLibTest, UiModuleActions) {
  TestHarness h;
  EXPECT_TRUE(h.stdlib.has("std:ui"));
  EXPECT_TRUE(h.stdlib.has("std:ui/view"));
  EXPECT_TRUE(h.stdlib.has("std:ui/swap_axes"));
  EXPECT_TRUE(h.stdlib.has("std:ui/cycle_dims_forward"));
  EXPECT_TRUE(h.stdlib.has("std:ui/cycle_dims_backward"));
  EXPECT_TRUE(h.stdlib.has("std:ui/bundle_execution"));

  zigzag::ViewAxisBinding axes{
      .x_dimension = "d.1", .y_dimension = "d.2", .z_dimension = "d.3"};
  h.stdlib.swapAxes(axes);
  EXPECT_EQ(axes.x_dimension, "d.2");
  EXPECT_EQ(axes.y_dimension, "d.1");

  h.stdlib.cycleDims(axes, true);
  EXPECT_EQ(axes.x_dimension, "d.1");
  EXPECT_EQ(axes.y_dimension, "d.3");
  EXPECT_EQ(axes.z_dimension, "d.2");

  h.stdlib.cycleDims(axes, false);
  EXPECT_EQ(axes.x_dimension, "d.2");
  EXPECT_EQ(axes.y_dimension, "d.1");
  EXPECT_EQ(axes.z_dimension, "d.3");

  h.stdlib.setView(axes, "d.spin", "d.step", "d.branch");
  EXPECT_EQ(axes.x_dimension, "d.spin");
  EXPECT_EQ(axes.y_dimension, "d.step");
  EXPECT_EQ(axes.z_dimension, "d.branch");

  h.stdlib.applyBundle(axes, zigzag::DimensionBundle::Execution);
  EXPECT_EQ(axes.x_dimension, "d.spin");
  EXPECT_EQ(axes.y_dimension, "d.step");
  EXPECT_EQ(axes.z_dimension, "d.branch");

  auto viewRes = h.stdlib.call("std:ui/view", {"d.x", "d.y", "d.z"});
  EXPECT_FALSE(viewRes.empty());
}

TEST(VortexStdLibTest, NavModuleActions) {
  TestHarness h;
  EXPECT_TRUE(h.stdlib.has("std:nav"));
  EXPECT_TRUE(h.stdlib.has("std:nav/hop_head"));
  EXPECT_TRUE(h.stdlib.has("std:nav/hop_tail"));
  EXPECT_TRUE(h.stdlib.has("std:nav/jump_home"));

  DimRef d1  = h.arena.makeCell("d.1");
  CellRef c1 = h.arena.makeCell("Node 1");
  CellRef c2 = h.arena.makeCell("Node 2");
  CellRef c3 = h.arena.makeCell("Node 3");
  EXPECT_TRUE(h.arena.link(c1, d1, zigzag::DimVector::POS, c2));
  EXPECT_TRUE(h.arena.link(c2, d1, zigzag::DimVector::POS, c3));

  EXPECT_EQ(h.stdlib.hopHead(c2, d1), std::optional<CellRef>{c1});
  EXPECT_EQ(h.stdlib.hopHead(c3, d1), std::optional<CellRef>{c1});
  EXPECT_EQ(h.stdlib.hopTail(c1, d1), std::optional<CellRef>{c3});
  EXPECT_EQ(h.stdlib.hopTail(c2, d1), std::optional<CellRef>{c3});
  EXPECT_EQ(h.stdlib.jumpHome(), h.core.home());
}

TEST(VortexStdLibTest, ZzDuplicate) {
  TestHarness h;
  CellRef original = h.arena.makeCell("Original Content");
  EXPECT_TRUE(h.arena.setValueBits(original, xanadu::ValueKind::Int64, 42));

  CellRef dup = h.stdlib.zzDuplicate(original).value();
  EXPECT_NE(dup, original);
  EXPECT_EQ(h.arena.textOf(dup), "Original Content");
  EXPECT_EQ(h.arena.asInt64(dup), 42);
  EXPECT_EQ(
      h.arena.linked(original, h.core.dims().clone, zigzag::DimVector::POS),
      dup);
}

TEST(VortexStdLibTest, SovereignStoreLibraryRoundtrip) {
  TestHarness h;
  xanadu::Store store;
  bool expOk = h.stdlib.exportStandardLibraryToStore(store);
  EXPECT_TRUE(expOk);
  EXPECT_GT(store.opCount(), 0u);

  TestHarness h2;
  EXPECT_TRUE(h2.stdlib.importModuleFromStore(store).has_value());
}

TEST(VortexStdLibTest, XuzzModuleRegistrationAndActions) {
  TestHarness h;
  EXPECT_TRUE(h.stdlib.has("std:xuzz"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/quit"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/save"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/close"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/next_doc"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/prev_doc"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/open_doc"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/close_doc"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/transclude"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/xanalink"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/cancel_link"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/beams"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/sworph"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/publish"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/history"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/delete"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/page_break"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/hypertime_map"));
  EXPECT_TRUE(h.stdlib.has("std:xuzz/radial_menu"));
  EXPECT_TRUE(h.stdlib.has("std:zigzag/duplicate"));
  EXPECT_TRUE(h.stdlib.has("std:ui/bundle_execution"));

  // Invoking std:xuzz/quit executes opcode successfully
  auto quitRes = h.stdlib.call("std:xuzz/quit", {});
  ASSERT_EQ(quitRes.size(), 1u);
  EXPECT_FALSE(quitRes.empty());

  // Invoking std:xuzz/save executes opcode successfully
  auto saveRes = h.stdlib.call("std:xuzz/save", {});
  ASSERT_EQ(saveRes.size(), 1u);
  EXPECT_FALSE(saveRes.empty());
}

// Stage 2 of design/vortex-multithreading-study.md §6.2: data-parallel
// map/filter/fold must be bit-identical to the serial result across any worker
// count, and fold must fix a left-to-right chunk-index combine order so a
// non-commutative combiner matches the serial left fold. We attach pools of
// 1, 2, 4, 8 with a threshold of 1 (so parallelism actually engages on a small
// list) and assert every result equals the serial baseline.
TEST(VortexStdLibTest, DataParallelMapFilterFoldIsDeterministic) {
  // Build the serial baseline with no pool attached.
  auto buildList = [](TestHarness &h, DimRef dim) {
    std::vector<CellValue> nums;
    for (int i = 1; i <= 50; ++i) {
      nums.push_back(static_cast<std::int64_t>(i));
    }
    return h.stdlib.createList(nums, dim).value();
  };

  auto squareFn = [](const CellValue &v) -> CellValue {
    const std::int64_t x = std::get<std::int64_t>(v);
    return x * x;
  };
  auto evenPred = [](const CellValue &v) -> bool {
    return std::get<std::int64_t>(v) % 2 == 0;
  };
  // A deliberately NON-COMMUTATIVE combiner: ordered string concatenation.
  // "a"+"b" != "b"+"a", so a wrong combine order would be caught.
  auto concatFn = [](const CellValue &acc, const CellValue &cur) -> CellValue {
    return std::get<std::string>(acc) +
           std::to_string(std::get<std::int64_t>(cur)) + ",";
  };

  // Serial baseline.
  std::vector<CellValue> baselineMap;
  std::vector<CellValue> baselineFilter;
  CellValue baselineFold;
  {
    TestHarness h;
    const DimRef inDim  = h.arena.makeCell("d.in");
    const DimRef outDim = h.arena.makeCell("d.out");
    const DimRef fltDim = h.arena.makeCell("d.flt");
    CellRef list        = buildList(h, inDim);
    CellRef mapped      = h.stdlib.map(list, inDim, outDim, squareFn).value();
    baselineMap         = h.stdlib.listToVector(mapped, outDim);
    CellRef filtered =
        h.stdlib.filter(mapped, outDim, fltDim, evenPred).value();
    baselineFilter = h.stdlib.listToVector(filtered, fltDim);
    baselineFold =
        h.stdlib.fold(list, inDim, CellValue(std::string("start:")), concatFn);
  }
  ASSERT_EQ(baselineMap.size(), 50u);
  ASSERT_FALSE(baselineFilter.empty());

  for (std::uint32_t threads : {1u, 2u, 4u, 8u}) {
    TestHarness h;
    VortexWorkerPool pool{threads};
    h.stdlib.setWorkerPool(&pool, /*threshold=*/1);

    const DimRef inDim  = h.arena.makeCell("d.in");
    const DimRef outDim = h.arena.makeCell("d.out");
    const DimRef fltDim = h.arena.makeCell("d.flt");
    CellRef list        = buildList(h, inDim);

    CellRef mapped = h.stdlib.map(list, inDim, outDim, squareFn).value();
    auto gotMap    = h.stdlib.listToVector(mapped, outDim);
    EXPECT_EQ(gotMap, baselineMap) << "map differs at threads=" << threads;

    CellRef filtered =
        h.stdlib.filter(mapped, outDim, fltDim, evenPred).value();
    auto gotFilter = h.stdlib.listToVector(filtered, fltDim);
    EXPECT_EQ(gotFilter, baselineFilter)
        << "filter differs at threads=" << threads;

    CellValue gotFold =
        h.stdlib.fold(list, inDim, CellValue(std::string("start:")), concatFn);
    EXPECT_EQ(gotFold, baselineFold)
        << "non-commutative fold differs at threads=" << threads;
  }
}

TEST(VortexStdLibTest, EnvModuleOperations) {
  TestHarness h;
  EXPECT_TRUE(h.stdlib.has("std:env"));
  EXPECT_TRUE(h.stdlib.has("std:env/args"));
  EXPECT_TRUE(h.stdlib.has("std:env/get_var"));

  // Initially empty args
  EXPECT_EQ(h.stdlib.envArgs(), std::nullopt);

  // Set args
  h.stdlib.setArgs({"--verbose", "--port", "8080"});
  auto argsHead = h.stdlib.envArgs();
  ASSERT_TRUE(argsHead.has_value());
  auto argVals = h.stdlib.listToVector(*argsHead);
  ASSERT_EQ(argVals.size(), 3u);
  EXPECT_EQ(argVals[0], CellValue(std::string("--verbose")));
  EXPECT_EQ(argVals[1], CellValue(std::string("--port")));
  EXPECT_EQ(argVals[2], CellValue(std::string("8080")));

  // Test via call()
  auto callArgs = h.stdlib.call("std:env/args", {});
  ASSERT_EQ(callArgs.size(), 1u);
  EXPECT_NE(static_cast<CellRef>(std::get<std::int64_t>(callArgs[0])), noCell);

  // Environment variable lookup
  setenv("TEST_VORTEX_ENV_VAR", "vortex_active", 1);
  auto varVal = h.stdlib.envGetVar("TEST_VORTEX_ENV_VAR");
  ASSERT_TRUE(varVal.has_value());
  EXPECT_EQ(*varVal, "vortex_active");

  auto callVar =
      h.stdlib.call("std:env/get_var", {std::string("TEST_VORTEX_ENV_VAR")});
  ASSERT_EQ(callVar.size(), 1u);
  EXPECT_EQ(std::get<std::string>(callVar[0]), "vortex_active");

  EXPECT_EQ(h.stdlib.envGetVar("NONEXISTENT_VORTEX_VAR_XYZ_12345"),
            std::nullopt);
}

TEST(VortexStdLibTest, IoModuleOperations) {
  TestHarness h;
  EXPECT_TRUE(h.stdlib.has("std:io"));
  EXPECT_TRUE(h.stdlib.has("std:io/read_file"));
  EXPECT_TRUE(h.stdlib.has("std:io/read_stdin"));
  EXPECT_TRUE(h.stdlib.has("std:io/print"));
  EXPECT_TRUE(h.stdlib.has("std:io/eprint"));

  // Strictly no write_file primitive exists (Nelsonian invariant V9)
  EXPECT_FALSE(h.stdlib.has("std:io/write_file"));
  EXPECT_FALSE(h.stdlib.has("std:io/write"));

  // Read existing file
  auto content = h.stdlib.ioReadFile("Makefile");
  ASSERT_TRUE(content.has_value());
  EXPECT_FALSE(content->empty());

  auto callRes = h.stdlib.call("std:io/read_file", {std::string("Makefile")});
  ASSERT_EQ(callRes.size(), 1u);
  EXPECT_FALSE(std::get<std::string>(callRes[0]).empty());

  // Nonexistent file returns nullopt
  EXPECT_EQ(h.stdlib.ioReadFile("nonexistent_test_file_never_exists.xyz"),
            std::nullopt);

  // Print operations execute cleanly
  EXPECT_NO_THROW(h.stdlib.ioPrint(""));
  EXPECT_NO_THROW(h.stdlib.ioEprint(""));
  EXPECT_NO_THROW(h.stdlib.call("std:io/print", {std::string("")}));
  EXPECT_NO_THROW(h.stdlib.call("std:io/eprint", {std::string("")}));

  // Fuel consumption enforces budget
  TestHarness limitedH;
  // Give very small work budget (10 work units)
  limitedH.arena.setExecutionLimits(1000, 10, 10000);
  EXPECT_THROW(
      { (void)limitedH.stdlib.ioReadFile("Makefile"); }, std::runtime_error);
}

TEST(VortexStdLibTest, ZigzagModuleAdditions) {
  TestHarness h;
  EXPECT_TRUE(h.stdlib.has("std:zigzag"));
  EXPECT_TRUE(h.stdlib.has("std:zigzag/make_cell"));
  EXPECT_TRUE(h.stdlib.has("std:zigzag/text_of"));
  EXPECT_TRUE(h.stdlib.has("std:zigzag/set_text"));
  EXPECT_TRUE(h.stdlib.has("std:zigzag/set_value"));
  EXPECT_TRUE(h.stdlib.has("std:zigzag/resolve_dim"));
  EXPECT_TRUE(h.stdlib.has("std:zigzag/promote"));

  // zzMakeCell & zzTextOf
  CellRef c = h.stdlib.zzMakeCell("initial text").value();
  EXPECT_NE(c, noCell);
  EXPECT_EQ(h.stdlib.zzTextOf(c), "initial text");

  // zzSetText
  EXPECT_TRUE(h.stdlib.zzSetText(c, "updated text"));
  EXPECT_EQ(h.stdlib.zzTextOf(c), "updated text");

  // zzSetValue
  EXPECT_TRUE(h.stdlib.zzSetValue(c, CellValue{static_cast<std::int64_t>(42)}));
  EXPECT_EQ(h.arena.asInt64(c), 42);

  // zzResolveDim
  auto stepDim = h.stdlib.zzResolveDim("step");
  ASSERT_TRUE(stepDim.has_value());
  EXPECT_EQ(*stepDim, h.core.dims().step);

  auto dStepDim = h.stdlib.zzResolveDim("d.step");
  ASSERT_TRUE(dStepDim.has_value());
  EXPECT_EQ(*dStepDim, h.core.dims().step);

  EXPECT_EQ(h.stdlib.zzResolveDim("d.dimension_that_does_not_exist"),
            std::nullopt);

  // Call dispatches
  auto callCellRes =
      h.stdlib.call("std:zigzag/make_cell", {std::string("via_call")});
  ASSERT_EQ(callCellRes.size(), 1u);
  CellRef callCell =
      static_cast<CellRef>(std::get<std::int64_t>(callCellRes[0]));
  EXPECT_NE(callCell, noCell);

  auto callTextRes = h.stdlib.call("std:zigzag/text_of",
                                   {static_cast<std::int64_t>(callCell)});
  ASSERT_EQ(callTextRes.size(), 1u);
  EXPECT_EQ(std::get<std::string>(callTextRes[0]), "via_call");

  auto callSetTextRes =
      h.stdlib.call("std:zigzag/set_text", {static_cast<std::int64_t>(callCell),
                                            std::string("replaced_via_call")});
  ASSERT_EQ(callSetTextRes.size(), 1u);
  EXPECT_TRUE(std::get<bool>(callSetTextRes[0]));
  EXPECT_EQ(h.stdlib.zzTextOf(callCell), "replaced_via_call");

  auto callSetValueRes = h.stdlib.call(
      "std:zigzag/set_value",
      {static_cast<std::int64_t>(callCell), static_cast<std::int64_t>(999)});
  ASSERT_EQ(callSetValueRes.size(), 1u);
  EXPECT_TRUE(std::get<bool>(callSetValueRes[0]));
  EXPECT_EQ(h.arena.asInt64(callCell), 999);

  // zzCloneToChain: links to tail of clone rank of symbolOp and tail of spin
  // rank of targetCell
  CellRef symbolOp   = h.stdlib.zzMakeCell("symbol_op").value();
  CellRef targetCell = h.stdlib.zzMakeCell("target_cell").value();
  auto cloneHead     = h.stdlib.zzCloneToChain(symbolOp, targetCell);
  ASSERT_TRUE(cloneHead.has_value());
  EXPECT_NE(*cloneHead, noCell);
  EXPECT_EQ(zigzag::rankTail(h.arena, symbolOp, h.core.dims().clone),
            *cloneHead);
  EXPECT_EQ(zigzag::rankTail(h.arena, targetCell, h.core.dims().spin),
            *cloneHead);

  // Promotion without bound store returns nullopt
  EXPECT_EQ(h.stdlib.zzPromote(c, std::nullopt), std::nullopt);
}

TEST(VortexStdLibTest, MapModuleBstAndOrdering) {
  TestHarness h;
  EXPECT_TRUE(h.stdlib.has("std:map"));
  EXPECT_TRUE(h.stdlib.has("std:map/create"));
  EXPECT_TRUE(h.stdlib.has("std:map/set"));
  EXPECT_TRUE(h.stdlib.has("std:map/get"));
  EXPECT_TRUE(h.stdlib.has("std:map/has"));
  EXPECT_TRUE(h.stdlib.has("std:map/keys"));
  EXPECT_TRUE(h.stdlib.has("std:map/len"));

  CellRef map = h.stdlib.createMap();
  EXPECT_NE(map, noCell);
  EXPECT_EQ(h.stdlib.mapLen(map), 0u);
  EXPECT_TRUE(h.stdlib.mapKeys(map).empty());

  // Insert out of order
  h.stdlib.mapSet(map, "delta", static_cast<std::int64_t>(4));
  h.stdlib.mapSet(map, "alpha", static_cast<std::int64_t>(1));
  h.stdlib.mapSet(map, "charlie", static_cast<std::int64_t>(3));
  h.stdlib.mapSet(map, "bravo", static_cast<std::int64_t>(2));

  EXPECT_EQ(h.stdlib.mapLen(map), 4u);
  EXPECT_TRUE(h.stdlib.mapHas(map, "alpha"));
  EXPECT_TRUE(h.stdlib.mapHas(map, "bravo"));
  EXPECT_TRUE(h.stdlib.mapHas(map, "charlie"));
  EXPECT_TRUE(h.stdlib.mapHas(map, "delta"));
  EXPECT_FALSE(h.stdlib.mapHas(map, "echo"));

  EXPECT_EQ(h.stdlib.mapGet(map, "bravo"),
            CellValue(static_cast<std::int64_t>(2)));

  // Keys must be in strictly sorted order
  auto keys = h.stdlib.mapKeys(map);
  ASSERT_EQ(keys.size(), 4u);
  EXPECT_EQ(keys[0], CellValue(std::string("alpha")));
  EXPECT_EQ(keys[1], CellValue(std::string("bravo")));
  EXPECT_EQ(keys[2], CellValue(std::string("charlie")));
  EXPECT_EQ(keys[3], CellValue(std::string("delta")));

  // Update existing key
  h.stdlib.mapSet(map, "bravo", static_cast<std::int64_t>(200));
  EXPECT_EQ(h.stdlib.mapLen(map), 4u);
  EXPECT_EQ(h.stdlib.mapGet(map, "bravo"),
            CellValue(static_cast<std::int64_t>(200)));

  // Numeric keys in separate map
  CellRef numMap = h.stdlib.createMap();
  h.stdlib.mapSet(numMap, CellValue(static_cast<std::int64_t>(30)),
                  std::string("thirty"));
  h.stdlib.mapSet(numMap, CellValue(static_cast<std::int64_t>(10)),
                  std::string("ten"));
  h.stdlib.mapSet(numMap, CellValue(static_cast<std::int64_t>(20)),
                  std::string("twenty"));

  EXPECT_EQ(h.stdlib.mapLen(numMap), 3u);
  auto numKeys = h.stdlib.mapKeys(numMap);
  ASSERT_EQ(numKeys.size(), 3u);
  EXPECT_EQ(numKeys[0], CellValue(static_cast<std::int64_t>(10)));
  EXPECT_EQ(numKeys[1], CellValue(static_cast<std::int64_t>(20)));
  EXPECT_EQ(numKeys[2], CellValue(static_cast<std::int64_t>(30)));

  // Call dispatches
  auto callCreate = h.stdlib.call("std:map/create", {});
  ASSERT_EQ(callCreate.size(), 1u);
  CellRef callMap = static_cast<CellRef>(std::get<std::int64_t>(callCreate[0]));
  EXPECT_NE(callMap, noCell);

  h.stdlib.call("std:map/set",
                {static_cast<std::int64_t>(callMap), std::string("k1"),
                 static_cast<std::int64_t>(42)});
  auto callHas = h.stdlib.call(
      "std:map/has", {static_cast<std::int64_t>(callMap), std::string("k1")});
  ASSERT_EQ(callHas.size(), 1u);
  EXPECT_TRUE(std::get<bool>(callHas[0]));

  auto callGet = h.stdlib.call(
      "std:map/get", {static_cast<std::int64_t>(callMap), std::string("k1")});
  ASSERT_EQ(callGet.size(), 1u);
  EXPECT_EQ(std::get<std::int64_t>(callGet[0]), 42);

  auto callLen =
      h.stdlib.call("std:map/len", {static_cast<std::int64_t>(callMap)});
  ASSERT_EQ(callLen.size(), 1u);
  EXPECT_EQ(std::get<std::int64_t>(callLen[0]), 1);

  // Fuel consumption check: verify consumeWork is invoked during map
  // operations. intern() bounds the arena's whole append-only scratch buffer,
  // which already holds the bootstrapped stdlib's interned text (~14 KiB), so
  // the scratch budget must cover that baseline plus evaluation growth.
  TestHarness fuelH;
  fuelH.arena.setExecutionLimits(
      static_cast<std::uint32_t>(fuelH.arena.cellCount() + 1000), 100,
      1U << 20);
  CellRef fMap = fuelH.stdlib.createMap();
  fuelH.stdlib.mapSet(fMap, "a", static_cast<std::int64_t>(1));
  const auto remainingAfterInsert = fuelH.arena.workRemaining();
  ASSERT_TRUE(remainingAfterInsert.has_value());
  EXPECT_LT(*remainingAfterInsert, 100u);

  (void)fuelH.stdlib.mapGet(fMap, "a");
  const auto remainingAfterGet = fuelH.arena.workRemaining();
  ASSERT_TRUE(remainingAfterGet.has_value());
  EXPECT_LT(*remainingAfterGet, *remainingAfterInsert);
}

TEST(VortexStdLibTest, HypertimeModuleOperations) {
  TestHarness h;
  EXPECT_TRUE(h.stdlib.has("std:hypertime"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/lca"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/path"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/is_ancestor_of"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/depth"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/jump_ancestor"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/parent"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/next"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/children"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/branch"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/branch_letters"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/latest"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/head"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/heads"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/repoint_head"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/to_str"));
  EXPECT_TRUE(h.stdlib.has("std:hypertime/parse"));

  // Pure version string queries
  EXPECT_EQ(h.stdlib.hyperParent("2a4"), "2a3");
  EXPECT_EQ(h.stdlib.hyperParent("2a1"), "2");
  EXPECT_EQ(h.stdlib.hyperNext("2a4"), "2a5");
  EXPECT_EQ(h.stdlib.hyperBranch("2", 1), "2a1");
  EXPECT_EQ(h.stdlib.hyperBranchLetters(1), "a");
  EXPECT_EQ(h.stdlib.hyperBranchLetters(2), "b");
  EXPECT_EQ(h.stdlib.hyperBranchLetters(27), "aa");
  EXPECT_EQ(h.stdlib.hyperToStr("2a4"), "2a4");
  EXPECT_EQ(h.stdlib.hyperParse("2a4"), "2a4");
  EXPECT_EQ(h.stdlib.hyperParse("not_a_valid_microversion"), std::nullopt);

  EXPECT_TRUE(h.stdlib.hyperIsAncestorOf("2", "2a4"));
  EXPECT_FALSE(h.stdlib.hyperIsAncestorOf("2a4", "2"));

  EXPECT_EQ(h.stdlib.hyperDepth("2a2"), 4);
  EXPECT_EQ(h.stdlib.hyperJumpAncestor("2a2", 2), "2");
  EXPECT_EQ(h.stdlib.hyperLca("2a1", "2b1"), "2");

  std::vector<std::string> path = h.stdlib.hyperPath("2a1");
  ASSERT_EQ(path.size(), 3u);
  EXPECT_EQ(path[0], "1");
  EXPECT_EQ(path[1], "2");
  EXPECT_EQ(path[2], "2a1");

  // Call dispatches
  auto callParent = h.stdlib.call("std:hypertime/parent", {std::string("2a4")});
  ASSERT_EQ(callParent.size(), 1u);
  EXPECT_EQ(std::get<std::string>(callParent[0]), "2a3");

  auto callNext = h.stdlib.call("std:hypertime/next", {std::string("2a4")});
  ASSERT_EQ(callNext.size(), 1u);
  EXPECT_EQ(std::get<std::string>(callNext[0]), "2a5");

  auto callBranch = h.stdlib.call(
      "std:hypertime/branch", {std::string("2"), static_cast<std::int64_t>(1)});
  ASSERT_EQ(callBranch.size(), 1u);
  EXPECT_EQ(std::get<std::string>(callBranch[0]), "2a1");

  auto callLca = h.stdlib.call("std:hypertime/lca",
                               {std::string("2a1"), std::string("2b1")});
  ASSERT_EQ(callLca.size(), 1u);
  EXPECT_EQ(std::get<std::string>(callLca[0]), "2");

  // Store-backed queries
  xanadu::Store store;
  EXPECT_TRUE(h.stdlib.exportStandardLibraryToStore(store));
  h.stdlib.setBoundStore(&store);

  EXPECT_TRUE(h.stdlib.hyperHead().has_value());
  EXPECT_TRUE(h.stdlib.hyperLatest().has_value());
  EXPECT_FALSE(h.stdlib.hyperHeads().empty());

  auto curHead = *h.stdlib.hyperHead();
  EXPECT_TRUE(h.stdlib.hyperRepointHead(curHead));

  // zzPromote with bound store
  CellRef testCell = h.stdlib.zzMakeCell("to_promote").value();
  auto promotedVer = h.stdlib.zzPromote(testCell, std::nullopt);
  EXPECT_TRUE(promotedVer.has_value());
  EXPECT_EQ(h.stdlib.hyperHead(), promotedVer);
}

} // namespace
