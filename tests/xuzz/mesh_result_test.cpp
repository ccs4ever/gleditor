#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/mesh_result.hpp"

namespace zigzag::vortex::mesh {
namespace {

constexpr EvaluationBudget budget{1'000'000, 100'000, 100, 1'000'000};
constexpr std::string_view repeatedNamedRest = R"(
  let $big := 9007199254740993;
  let $f := \($head: Int, ...$rest: Int) $rest;
  return $f(1, $big, $big, rest: $big)
)";

class TemporaryDirectory {
public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("mesh_result_native_" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

std::optional<CellRef> linkedByName(const ArenaManifold &arena, CellRef cell,
                                    std::string_view name) {
  for (const auto &edge : arena.dimensionsOf(cell))
    if (arena.textOf(edge.dim) == name) return edge.pos;
  return std::nullopt;
}

std::optional<DimRef> dimensionByName(const ArenaManifold &arena, CellRef cell,
                                      std::string_view name) {
  for (const auto &edge : arena.dimensionsOf(cell))
    if (arena.textOf(edge.dim) == name) return edge.dim;
  return std::nullopt;
}

void expectDecodeError(const EvaluatedValue &value,
                       std::string_view expectedMessage) {
  const auto decoded = readResultCells(value);
  ASSERT_FALSE(decoded);
  EXPECT_NE(decoded.error().find(expectedMessage), std::string::npos)
      << decoded.error();
}

std::vector<CellRef> orderedWrappers(const ArenaManifold &arena, CellRef root) {
  std::vector<CellRef> wrappers;
  const auto items = dimensionByName(arena, root, "d.mesh.items");
  EXPECT_TRUE(items);
  if (!items) return wrappers;

  auto record = arena.linked(root, *items, DimVector::POS);
  while (record) {
    const auto value = dimensionByName(arena, *record, "d.mesh.value");
    EXPECT_TRUE(value);
    if (!value) break;
    const auto wrapper = arena.linked(*record, *value, DimVector::POS);
    EXPECT_TRUE(wrapper);
    if (!wrapper) break;
    wrappers.push_back(*wrapper);
    record = arena.linked(*record, *items, DimVector::POS);
  }
  return wrappers;
}

TEST(MeshResultTest, RepeatedNamedRestRowsKeepMultiplicityAndIdentity) {
  auto result = evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(result) << result.error();

  auto cells = readResultCells(*result);
  ASSERT_TRUE(cells) << cells.error();
  ASSERT_EQ(cells->size(), 3U);
  EXPECT_EQ((*cells)[0], (*cells)[1]);
  EXPECT_EQ((*cells)[1], (*cells)[2]);
  for (const CellRef cell : *cells) {
    EXPECT_EQ(result->context->arena.valueKindOf(cell),
              xanadu::ValueKind::Int64);
    EXPECT_EQ(result->context->arena.asInt64(cell), 9007199254740993LL);
  }

  const auto wrappers = orderedWrappers(result->context->arena, result->root);
  ASSERT_EQ(wrappers.size(), 3U);
  EXPECT_NE(wrappers[0], wrappers[1]);
  EXPECT_NE(wrappers[1], wrappers[2]);
  EXPECT_NE(wrappers[0], wrappers[2]);
  for (const CellRef wrapper : wrappers) {
    EXPECT_EQ(result->context->arena.valueKindOf(wrapper),
              xanadu::ValueKind::OpHandle);
    EXPECT_EQ(readOccurrenceTarget(result->context->arena, wrapper),
              (*cells)[0]);
  }
}

TEST(MeshResultTest,
     IntegerIntegralDoubleAndCanonicalZeroKeepExactTypesAndBits) {
  auto result = evaluate(Frontend::Vquery,
                         "let $values := \\(...$rest) $rest; "
                         "return $values(4, 4.0, -0.0)",
                         budget);
  ASSERT_TRUE(result) << result.error();
  auto cells = readResultCells(*result);
  ASSERT_TRUE(cells) << cells.error();
  ASSERT_EQ(cells->size(), 3U);

  const auto &arena = result->context->arena;
  EXPECT_EQ(arena.valueKindOf((*cells)[0]), xanadu::ValueKind::Int64);
  EXPECT_EQ(arena.asInt64((*cells)[0]), 4);
  EXPECT_EQ(arena.valueKindOf((*cells)[1]), xanadu::ValueKind::Double);
  EXPECT_EQ(arena.asDouble((*cells)[1]), 4.0);
  EXPECT_EQ(arena.valueKindOf((*cells)[2]), xanadu::ValueKind::Double);
  ASSERT_TRUE(arena.slot((*cells)[2]));
  EXPECT_EQ(arena.slot((*cells)[2])->valueBits,
            std::bit_cast<std::uint64_t>(0.0));
}

TEST(MeshResultTest, EmptyRowsAndCommonFrontendsUseTheVersionedProducer) {
  auto empty = evaluate(Frontend::Vprolog, "answer(1). ?- answer(2).", budget);
  ASSERT_TRUE(empty) << empty.error();
  auto noCells = readResultCells(*empty);
  ASSERT_TRUE(noCells) << noCells.error();
  EXPECT_TRUE(noCells->empty());
  EXPECT_FALSE(
      linkedByName(empty->context->arena, empty->root, "d.mesh.items"));
  const auto count =
      linkedByName(empty->context->arena, empty->root, "d.mesh.count");
  ASSERT_TRUE(count);
  EXPECT_EQ(empty->context->arena.valueKindOf(*count),
            xanadu::ValueKind::Int64);
  EXPECT_EQ(empty->context->arena.asInt64(*count), 0);
  const auto unrelatedSchemaCell =
      empty->context->arena.makeCell("d.mesh.value");
  EXPECT_TRUE(empty->context->arena.contains(unrelatedSchemaCell));
  EXPECT_TRUE(readResultCells(*empty));

  const std::array sources{
      std::pair{Frontend::Vpl, std::string_view{"1 + 2"}},
      std::pair{Frontend::Vquery, std::string_view{"##/d.remote%\"three\""}},
      std::pair{Frontend::Vprolog,
                std::string_view{"answer(3). ?- answer(X)."}},
      std::pair{Frontend::Vlace, std::string_view{"1 + 2"}}};
  for (const auto &[frontend, source] : sources) {
    SCOPED_TRACE(source);
    auto evaluated = evaluate(frontend, source, budget);
    ASSERT_TRUE(evaluated) << evaluated.error();
    auto rows = readResultCells(*evaluated);
    ASSERT_TRUE(rows) << rows.error();
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_NE(evaluated->context->arena.valueKindOf(rows->front()),
              xanadu::ValueKind::ForeignOpHandle);
  }

  VortexProgram program{.cells        = {{},
                                         {},
                                         {CellValue{std::int64_t{20}}, {}},
                                         {CellValue{std::int64_t{22}}, {}},
                                         {}},
                        .instructions = {{0, OpcodeKind::Add, {2, 3}, {4}, 1},
                                         {1, OpcodeKind::Halt, {}, {}, {}}},
                        .entry        = 0,
                        .outputs      = {{4}}};
  auto artifact = sealVortexProgram(program);
  ASSERT_TRUE(artifact) << artifact.error();
  auto vortex = evaluate(Frontend::Vortex, *artifact, budget);
  ASSERT_TRUE(vortex) << vortex.error();
  auto vortexRows = readResultCells(*vortex);
  ASSERT_TRUE(vortexRows) << vortexRows.error();
  ASSERT_EQ(vortexRows->size(), 1U);
  EXPECT_EQ(vortex->context->arena.asInt64(vortexRows->front()), 42);
}

TEST(MeshResultTest, SharedVlaceConstructorArgumentsKeepBothOccurrences) {
  auto result = evaluate(Frontend::Vlace,
                         "let shared = (1, 2) in (shared, shared)", budget);
  ASSERT_TRUE(result) << result.error();
  auto rows = readResultCells(*result);
  ASSERT_TRUE(rows) << rows.error();
  ASSERT_EQ(rows->size(), 1U);

  const auto &arena   = result->context->arena;
  const CellRef tuple = rows->front();
  ASSERT_EQ(arena.textOf(tuple), "Tuple");
  const auto arguments = dimensionByName(arena, tuple, "d.mesh.arguments");
  ASSERT_TRUE(arguments);
  auto first = arena.linked(tuple, *arguments, DimVector::POS);
  ASSERT_TRUE(first);
  const auto value = dimensionByName(arena, *first, "d.mesh.value");
  ASSERT_TRUE(value);
  const auto firstWrapper = arena.linked(*first, *value, DimVector::POS);
  ASSERT_TRUE(firstWrapper);
  auto second = arena.linked(*first, *arguments, DimVector::POS);
  ASSERT_TRUE(second);
  const auto secondValue = dimensionByName(arena, *second, "d.mesh.value");
  ASSERT_TRUE(secondValue);
  const auto secondWrapper =
      arena.linked(*second, *secondValue, DimVector::POS);
  ASSERT_TRUE(secondWrapper);
  EXPECT_NE(*firstWrapper, *secondWrapper);

  const auto firstTarget  = readOccurrenceTarget(arena, *firstWrapper);
  const auto secondTarget = readOccurrenceTarget(arena, *secondWrapper);
  ASSERT_TRUE(firstTarget) << firstTarget.error();
  ASSERT_TRUE(secondTarget) << secondTarget.error();
  EXPECT_EQ(*firstTarget, *secondTarget);
  EXPECT_EQ(arena.textOf(*firstTarget), "Tuple");
  const auto firstArgument =
      dimensionByName(arena, *firstTarget, "d.mesh.arguments");
  ASSERT_TRUE(firstArgument);
  auto scalarRecord =
      arena.linked(*firstTarget, *firstArgument, DimVector::POS);
  ASSERT_TRUE(scalarRecord);
  const auto scalarValue =
      dimensionByName(arena, *scalarRecord, "d.mesh.value");
  ASSERT_TRUE(scalarValue);
  const auto scalarWrapper =
      arena.linked(*scalarRecord, *scalarValue, DimVector::POS);
  ASSERT_TRUE(scalarWrapper);
  const auto scalarTarget = readOccurrenceTarget(arena, *scalarWrapper);
  ASSERT_TRUE(scalarTarget) << scalarTarget.error();
  EXPECT_EQ(arena.asInt64(*scalarTarget), 1);
}

TEST(MeshResultTest, OneOccurrenceUnwrapDoesNotRecurseIntoMarkedReferents) {
  auto result = evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(result) << result.error();
  auto rows = readResultCells(*result);
  ASSERT_TRUE(rows) << rows.error();
  ASSERT_FALSE(rows->empty());
  auto &arena         = result->context->arena;
  const auto semantic = rows->front();

  const auto firstWrapper = orderedWrappers(arena, result->root).front();
  const auto relocation =
      dimensionByName(arena, firstWrapper, "d.mesh.relocation");
  ASSERT_TRUE(relocation);
  const auto marker = arena.makeCell("cell");
  ASSERT_TRUE(
      arena.setValueBits(semantic, xanadu::ValueKind::OpHandle, semantic));
  auto unmarkedTarget = readOccurrenceTarget(arena, firstWrapper);
  ASSERT_TRUE(unmarkedTarget) << unmarkedTarget.error();
  EXPECT_EQ(*unmarkedTarget, semantic);
  ASSERT_TRUE(arena.link(semantic, *relocation, DimVector::POS, marker));

  auto target = readOccurrenceTarget(arena, firstWrapper);
  ASSERT_TRUE(target) << target.error();
  EXPECT_EQ(*target, semantic);
  EXPECT_EQ(arena.valueKindOf(*target), xanadu::ValueKind::OpHandle);
  EXPECT_EQ(arena.handleTarget(*target), semantic);
}

TEST(MeshResultTest,
     OldRootBadWrappersCyclesAndMetadataBudgetsRefuseWholeGraph) {
  auto result = evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(result) << result.error();
  auto &arena = result->context->arena;
  auto rows   = readResultCells(*result);
  ASSERT_TRUE(rows) << rows.error();
  ASSERT_EQ(rows->size(), 3U);

  const auto originalRootText = arena.textOf(result->root);
  const auto originalSpan     = arena.intern("mesh:result");
  const std::array oldRoot{originalSpan};
  ASSERT_TRUE(arena.setContent(result->root, oldRoot));
  expectDecodeError(*result, "unsupported result root version");
  const auto restoredSpan = arena.intern(originalRootText);
  const std::array restoredRoot{restoredSpan};
  ASSERT_TRUE(arena.setContent(result->root, restoredRoot));
  ASSERT_TRUE(readResultCells(*result));

  const auto wrappers = orderedWrappers(arena, result->root);
  ASSERT_EQ(wrappers.size(), 3U);
  const auto relocation =
      dimensionByName(arena, wrappers.front(), "d.mesh.relocation");
  ASSERT_TRUE(relocation);
  const auto oldMarker =
      arena.linked(wrappers.front(), *relocation, DimVector::POS);
  ASSERT_TRUE(oldMarker);
  ASSERT_TRUE(arena.link(wrappers.front(), *relocation, DimVector::POS));
  expectDecodeError(*result, "malformed occurrence marker rank");

  ASSERT_TRUE(
      arena.link(wrappers.front(), *relocation, DimVector::POS, *oldMarker));
  const auto badMarkerText = arena.intern("not-cell");
  const std::array badMarkerContent{badMarkerText};
  ASSERT_TRUE(arena.setContent(*oldMarker, badMarkerContent));
  expectDecodeError(*result, "invalid occurrence marker");
  const auto goodMarkerText = arena.intern("cell");
  const std::array goodMarkerContent{goodMarkerText};
  ASSERT_TRUE(arena.setContent(*oldMarker, goodMarkerContent));
  ASSERT_TRUE(arena.setValueBits(wrappers.front(), xanadu::ValueKind::None, 0));
  expectDecodeError(*result, "value wrapper is not an OpHandle");

  ASSERT_TRUE(arena.setValueBits(wrappers.front(), xanadu::ValueKind::OpHandle,
                                 std::numeric_limits<CellRef>::max()));
  expectDecodeError(*result, "value wrapper target is absent");

  auto fresh = evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(fresh) << fresh.error();
  auto freshWrappers = orderedWrappers(fresh->context->arena, fresh->root);
  ASSERT_EQ(freshWrappers.size(), 3U);
  const auto items =
      dimensionByName(fresh->context->arena, fresh->root, "d.mesh.items");
  ASSERT_TRUE(items);
  auto last = fresh->context->arena.linked(fresh->root, *items, DimVector::POS);
  ASSERT_TRUE(last);
  while (const auto next =
             fresh->context->arena.linked(*last, *items, DimVector::POS))
    last = next;
  ASSERT_TRUE(
      fresh->context->arena.link(*last, *items, DimVector::POS, fresh->root));
  expectDecodeError(*fresh, "malformed item rank origin");

  auto duplicate = evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(duplicate) << duplicate.error();
  const auto duplicateItems = dimensionByName(duplicate->context->arena,
                                              duplicate->root, "d.mesh.items");
  ASSERT_TRUE(duplicateItems);
  const auto duplicateDimension =
      duplicate->context->arena.makeCell("d.mesh.items");
  const auto extraRecord = duplicate->context->arena.makeCell();
  ASSERT_TRUE(duplicate->context->arena.link(
      duplicate->root, duplicateDimension, DimVector::POS, extraRecord));
  expectDecodeError(*duplicate, "duplicate schema dimension");

  auto limited = evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(limited) << limited.error();
  ResultReadLimits limits;
  limits.maxItems = 2;
  auto tooMany    = readResultCells(*limited, limits);
  ASSERT_FALSE(tooMany);
  EXPECT_NE(tooMany.error().find("item count limit"), std::string::npos);
  limits                      = {};
  limits.maxMetadataRelations = 1;
  auto tooMuchWork            = readResultCells(*limited, limits);
  ASSERT_FALSE(tooMuchWork);
  EXPECT_NE(tooMuchWork.error().find("metadata work budget"),
            std::string::npos);
  limits                      = {};
  limits.maxMetadataTextBytes = 1;
  auto tooMuchText            = readResultCells(*limited, limits);
  ASSERT_FALSE(tooMuchText);
  EXPECT_NE(tooMuchText.error().find("metadata text budget"),
            std::string::npos);

  auto malformedCountRank =
      evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(malformedCountRank) << malformedCountRank.error();
  auto &countRankArena = malformedCountRank->context->arena;
  const auto countDimension =
      dimensionByName(countRankArena, malformedCountRank->root, "d.mesh.count");
  const auto countCell =
      linkedByName(countRankArena, malformedCountRank->root, "d.mesh.count");
  ASSERT_TRUE(countDimension);
  ASSERT_TRUE(countCell);
  const auto extraCountCell = countRankArena.makeCell();
  ASSERT_TRUE(countRankArena.link(*countCell, *countDimension, DimVector::POS,
                                  extraCountCell));
  expectDecodeError(*malformedCountRank, "missing or malformed item count");

  auto malformedWrapperRank =
      evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(malformedWrapperRank) << malformedWrapperRank.error();
  auto &wrapperRankArena = malformedWrapperRank->context->arena;
  const auto firstWrapper =
      orderedWrappers(wrapperRankArena, malformedWrapperRank->root).front();
  const auto wrapperRankItems = dimensionByName(
      wrapperRankArena, malformedWrapperRank->root, "d.mesh.items");
  ASSERT_TRUE(wrapperRankItems);
  const auto firstRecord = wrapperRankArena.linked(
      malformedWrapperRank->root, *wrapperRankItems, DimVector::POS);
  ASSERT_TRUE(firstRecord);
  const auto valueDimension =
      dimensionByName(wrapperRankArena, *firstRecord, "d.mesh.value");
  ASSERT_TRUE(valueDimension);
  const auto extraValue = wrapperRankArena.makeCell();
  ASSERT_TRUE(wrapperRankArena.link(firstWrapper, *valueDimension,
                                    DimVector::POS, extraValue));
  expectDecodeError(*malformedWrapperRank,
                    "row must have one value occurrence");

  auto malformedCount = evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(malformedCount) << malformedCount.error();
  auto &countArena = malformedCount->context->arena;
  auto count = linkedByName(countArena, malformedCount->root, "d.mesh.count");
  ASSERT_TRUE(count);
  ASSERT_TRUE(countArena.setValueBits(*count, xanadu::ValueKind::Double, 3));
  expectDecodeError(*malformedCount, "item count limit or type failure");

  auto zeroCountWithRows =
      evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(zeroCountWithRows) << zeroCountWithRows.error();
  auto &zeroArena = zeroCountWithRows->context->arena;
  const auto zeroCount =
      linkedByName(zeroArena, zeroCountWithRows->root, "d.mesh.count");
  ASSERT_TRUE(zeroCount);
  ASSERT_TRUE(zeroArena.setValueBits(*zeroCount, xanadu::ValueKind::Int64, 0));
  expectDecodeError(*zeroCountWithRows, "rows exceed declared item count");

  auto absentCount = evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(absentCount) << absentCount.error();
  std::optional<DimRef> absentCountDimension;
  for (const auto &edge :
       absentCount->context->arena.dimensionsOf(absentCount->root))
    if (absentCount->context->arena.textOf(edge.dim) == "d.mesh.count")
      absentCountDimension = edge.dim;
  ASSERT_TRUE(absentCountDimension);
  ASSERT_TRUE(absentCount->context->arena.link(
      absentCount->root, *absentCountDimension, DimVector::POS));
  expectDecodeError(*absentCount, "missing or malformed item count");

  auto missingRank = evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(missingRank) << missingRank.error();
  std::optional<DimRef> itemsDimension;
  for (const auto &edge :
       missingRank->context->arena.dimensionsOf(missingRank->root))
    if (missingRank->context->arena.textOf(edge.dim) == "d.mesh.items")
      itemsDimension = edge.dim;
  ASSERT_TRUE(itemsDimension);
  ASSERT_TRUE(missingRank->context->arena.link(
      missingRank->root, *itemsDimension, DimVector::POS));
  expectDecodeError(*missingRank, "item rank is shorter than declared");

  auto truncated = evaluate(Frontend::Vquery, repeatedNamedRest, budget);
  ASSERT_TRUE(truncated) << truncated.error();
  const auto truncatedItems = dimensionByName(truncated->context->arena,
                                              truncated->root, "d.mesh.items");
  ASSERT_TRUE(truncatedItems);
  auto first = truncated->context->arena.linked(
      truncated->root, *truncatedItems, DimVector::POS);
  ASSERT_TRUE(first);
  auto second =
      truncated->context->arena.linked(*first, *truncatedItems, DimVector::POS);
  ASSERT_TRUE(second);
  ASSERT_TRUE(
      truncated->context->arena.link(*second, *truncatedItems, DimVector::POS));
  expectDecodeError(*truncated, "item rank is shorter than declared");
}

TEST(MeshResultTest,
     WrapperOverheadExhaustionDiscardsPrivateEvaluationAndCanRetry) {
  TemporaryDirectory temporary;
  auto scroll =
      std::make_shared<xanadu::UserPermascroll>(xanadu::UserPermascroll::Config{
          .storageDir = temporary.path() / "permascroll"});
  xanadu::Store source(scroll);
  auto version              = source.sliceGenesis({});
  auto snapshot             = source.rebuildManifold(version);
  const auto operationCount = source.opRecords().size();
  auto complete =
      evaluate(Frontend::Vquery, repeatedNamedRest, budget, &snapshot, &source);
  ASSERT_TRUE(complete) << complete.error();
  const auto neededCells = complete->context->arena.cellCount();
  ASSERT_GT(neededCells, 1U);
  auto tooSmall     = budget;
  tooSmall.maxCells = neededCells - 1;
  auto failed       = evaluate(Frontend::Vquery, repeatedNamedRest, tooSmall,
                               &snapshot, &source);
  ASSERT_FALSE(failed);
  EXPECT_EQ(source.opRecords().size(), operationCount);

  auto retry =
      evaluate(Frontend::Vquery, repeatedNamedRest, budget, &snapshot, &source);
  ASSERT_TRUE(retry) << retry.error();
  auto rows = readResultCells(*retry);
  ASSERT_TRUE(rows) << rows.error();
  ASSERT_EQ(rows->size(), 3U);
  EXPECT_EQ((*rows)[0], (*rows)[2]);
  EXPECT_EQ(source.opRecords().size(), operationCount);
  EXPECT_FALSE(snapshot.dimensionNamed("d.mesh.value", source));
}

} // namespace
} // namespace zigzag::vortex::mesh
