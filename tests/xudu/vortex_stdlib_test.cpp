/**
 * @file vortex_stdlib_test.cpp
 * @brief Unit tests for the Vortex Standard Library in Vortex (Stage 2).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "xudu/core/vortex.hpp"
#include "xudu/core/vortex_stdlib.hpp"
#include "zigzag/core/arena_manifold.hpp"

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

  EXPECT_FALSE(h.stdlib.has("std:nonexistent"));
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
  h.core.value(inParam, 0, -1, static_cast<std::int64_t>(20));
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

  CellRef listHead = h.stdlib.createList(items);
  EXPECT_NE(listHead, noCell);
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
  CellRef restoredHead = h.stdlib.popFront(newHead);
  EXPECT_EQ(h.stdlib.listLength(restoredHead), 3u);

  // Convert to vector
  auto vec = h.stdlib.listToVector(restoredHead);
  ASSERT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0], CellValue(static_cast<std::int64_t>(10)));
  EXPECT_EQ(vec[1], CellValue(static_cast<std::int64_t>(20)));
  EXPECT_EQ(vec[2], CellValue(static_cast<std::int64_t>(30)));
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
  CellRef grid =
      h.stdlib.createGrid(3, 4, dRow, dCol, static_cast<std::int64_t>(0));
  EXPECT_NE(grid, noCell);

  // Set cell at (1, 2) to 42
  h.stdlib.setGrid(grid, 1, 2, dRow, dCol, static_cast<std::int64_t>(42));
  EXPECT_EQ(h.stdlib.getGrid(grid, 1, 2, dRow, dCol),
            CellValue(static_cast<std::int64_t>(42)));

  // Spatial Transposition: simply swap dRow and dCol dimensions!
  // Cell at (1, 2) in original grid is at (2, 1) in transposed view!
  EXPECT_EQ(h.stdlib.getGrid(grid, 2, 1, dCol, dRow),
            CellValue(static_cast<std::int64_t>(42)));
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
  CellRef listHead = h.stdlib.createList(nums, inDim);

  // Map: square (x * x)
  CellRef squaredHead = h.stdlib.map(
      listHead, inDim, outDim, [](const CellValue &v) -> CellValue {
        std::int64_t x = std::get<std::int64_t>(v);
        return x * x;
      });
  auto squaredVec = h.stdlib.listToVector(squaredHead, outDim);
  ASSERT_EQ(squaredVec.size(), 5u);
  EXPECT_EQ(squaredVec[0], CellValue(static_cast<std::int64_t>(1)));
  EXPECT_EQ(squaredVec[1], CellValue(static_cast<std::int64_t>(4)));
  EXPECT_EQ(squaredVec[2], CellValue(static_cast<std::int64_t>(9)));
  EXPECT_EQ(squaredVec[3], CellValue(static_cast<std::int64_t>(16)));
  EXPECT_EQ(squaredVec[4], CellValue(static_cast<std::int64_t>(25)));

  // Filter: evens only
  DimRef filterDim  = h.arena.makeCell("d.filter");
  CellRef evensHead = h.stdlib.filter(
      squaredHead, outDim, filterDim, [](const CellValue &v) -> bool {
        return std::get<std::int64_t>(v) % 2 == 0;
      });
  auto evensVec = h.stdlib.listToVector(evensHead, filterDim);
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
  CellRef zipped =
      h.stdlib.zip(squaredHead, evensHead, outDim, filterDim, zipDim);
  EXPECT_NE(zipped, noCell);
}

} // namespace
