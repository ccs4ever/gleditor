#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine engine{coordinator};
  zigzag::vortex::VortexVM vm{core};
  QueryExecutor executor{engine, vm};
  const DimRef items{coordinator.resolveDimension("d.items")};
  const DimRef score{coordinator.resolveDimension("d.score")};
  const DimRef tag{coordinator.resolveDimension("d.tag")};
  const DimRef video{coordinator.resolveDimension("d.video")};
  const DimRef codec{coordinator.resolveDimension("d.codec")};
  const DimRef name{coordinator.resolveDimension("d.name")};

  CellRef item(const std::string_view text) {
    const CellRef cell = arena.makeCell(text);
    const CellRef tail = zigzag::rankTail(arena, core.home(), items);
    zigzag::expectWritten(arena.link(tail, items, DimVector::POS, cell));
    return cell;
  }

  void link(const CellRef from, const DimRef dim, const DimVector direction,
            const CellRef to) {
    zigzag::expectWritten(arena.link(from, dim, direction, to));
  }

  auto execute(const std::string_view source) {
    return executor.execute(source);
  }
};

TEST(VQLShapeTargetVMTest,
     LiteralTargetsUseExactTypedAndInferredScalarComparisons) {
  Harness h;
  const auto intAboveDouble = h.item("int above 2^53");
  const auto intAtDouble    = h.item("int at 2^53");
  const auto doubleAt       = h.item("double at 2^53");
  const auto typedDisplay   = h.item("typed custom display");
  const auto numericText    = h.item("numeric text");
  const auto falseValue     = h.item("false value");
  const auto negativeZero   = h.item("negative zero");
  const auto positiveZero   = h.item("positive zero");

  h.link(intAboveDouble, h.score, DimVector::POS,
         h.arena.makeScalarCell(INT64_C(9007199254740993)));
  h.link(intAtDouble, h.score, DimVector::POS,
         h.arena.makeScalarCell(INT64_C(9007199254740992)));
  h.link(doubleAt, h.score, DimVector::POS, h.arena.makeScalarCell(0x1p53));
  const auto custom = h.arena.makeCell("display says seven");
  zigzag::expectWritten(
      h.arena.setValueBits(custom, xanadu::ValueKind::Int64,
                           std::bit_cast<std::uint64_t>(std::int64_t{7})));
  h.link(typedDisplay, h.score, DimVector::POS, custom);
  h.link(numericText, h.score, DimVector::POS, h.arena.makeCell("7"));
  h.link(falseValue, h.score, DimVector::POS, h.arena.makeScalarCell(false));
  h.link(negativeZero, h.score, DimVector::POS, h.arena.makeScalarCell(-0.0));
  h.link(positiveZero, h.score, DimVector::POS, h.arena.makeScalarCell(0.0));

  for (const auto &[query, expected] :
       std::vector<std::pair<std::string, std::vector<CellRef>>>{
           {"##/d.items[{+d.score: 9007199254740992.0}]",
            {intAtDouble, doubleAt}},
           {"##/d.items[{+d.score: 9007199254740993}]", {intAboveDouble}},
           {"##/d.items[{+d.score: 7}]", {typedDisplay, numericText}},
           {"##/d.items[{+d.score: false}]", {falseValue}},
           {"##/d.items[{+d.score: -0.0}]", {negativeZero, positiveZero}},
       }) {
    SCOPED_TRACE(query);
    const auto result = h.execute(query);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, expected);
  }
}

TEST(VQLShapeTargetVMTest, NestedTargetsCheckImmediateNeighborsRecursively) {
  Harness h;
  const auto positiveMatch = h.item("positive match");
  const auto negativeMatch = h.item("negative match");
  const auto eitherMatch   = h.item("either match");
  const auto wrong         = h.item("wrong immediate neighbor");
  const auto missingVideo  = h.item("missing video");
  const auto noCodec       = h.item("video without codec");
  const auto deepMatch     = h.item("deep match");
  const auto falseCodec    = h.item("false codec");

  const auto addVideo = [&](const CellRef item, const DimVector direction,
                            const std::string_view codecText) {
    const auto video = h.arena.makeCell("video");
    h.link(item, h.video, direction, video);
    if (!codecText.empty()) {
      const auto codecCell = h.arena.makeCell(codecText);
      h.link(video, h.codec, DimVector::POS, codecCell);
      const auto value = h.arena.makeCell(codecText);
      h.link(codecCell, h.name, DimVector::POS, value);
    }
    return video;
  };

  addVideo(positiveMatch, DimVector::POS, "h264");
  addVideo(negativeMatch, DimVector::NEG, "h264");
  addVideo(eitherMatch, DimVector::POS, "av1");
  addVideo(eitherMatch, DimVector::NEG, "h264");
  const auto wrongVideo = addVideo(wrong, DimVector::POS, "");
  const auto laterVideo = h.arena.makeCell("later video");
  h.link(wrongVideo, h.video, DimVector::POS, laterVideo);
  const auto laterCodec = h.arena.makeCell("later codec");
  h.link(laterVideo, h.codec, DimVector::POS, laterCodec);
  h.link(laterCodec, h.name, DimVector::POS, h.arena.makeCell("h264"));
  addVideo(noCodec, DimVector::POS, "");
  const auto deepVideo = h.arena.makeCell("deep video");
  const auto deepCodec = h.arena.makeCell("deep codec");
  h.link(deepMatch, h.video, DimVector::POS, deepVideo);
  h.link(deepVideo, h.codec, DimVector::POS, deepCodec);
  h.link(deepCodec, h.name, DimVector::POS, h.arena.makeCell("High"));
  const auto falseVideo = h.arena.makeCell("false video");
  const auto falseCell  = h.arena.makeScalarCell(false);
  h.link(falseCodec, h.video, DimVector::POS, falseVideo);
  h.link(falseVideo, h.codec, DimVector::POS, falseCell);

  for (const auto &[query, expected] :
       std::vector<std::pair<std::string, std::vector<CellRef>>>{
           {"##/d.items[{+d.video: {+d.codec: \"h264\"}}]", {positiveMatch}},
           {"##/d.items[{~d.video: {+d.codec: \"h264\"}}]",
            {positiveMatch, negativeMatch, eitherMatch}},
           {"##/d.items[{+d.video: {+d.codec: {+d.name: \"High\"}}}]",
            {deepMatch}},
           {"##/d.items[{+d.video: {+d.codec: false}}]", {falseCodec}},
           {"##/d.items[{+d.video: {!d.codec}}]", {wrong, noCodec}},
           {"##/d.items[{!d.video: {+d.codec: \"h264\"}}]", {missingVideo}},
       }) {
    SCOPED_TRACE(query);
    const auto result = h.execute(query);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, expected);
  }
}

TEST(VQLShapeTargetVMTest,
     TargetFilteringPreservesDuplicatesOrderClampAndCaptures) {
  Harness h;
  const auto first    = h.item("first");
  const auto skip     = h.item("skip");
  const auto last     = h.item("last");
  const auto firstTag = h.arena.makeCell("yes");
  const auto skipTag  = h.arena.makeCell("yes");
  const auto lastTag  = h.arena.makeCell("yes");
  h.link(first, h.tag, DimVector::POS, firstTag);
  h.link(skip, h.tag, DimVector::POS, skipTag);
  h.link(last, h.tag, DimVector::POS, lastTag);

  const auto clamped =
      h.execute("##/d.items[{+d.tag: \"yes\"}][. != \"skip\"][-1]");
  ASSERT_TRUE(clamped) << clamped.error().message;
  EXPECT_EQ(*clamped, (std::vector<CellRef>{last}));

  const auto duplicates = h.executor.execute("./d.items[{+d.tag: \"yes\"}]",
                                             {h.core.home(), h.core.home()});
  ASSERT_TRUE(duplicates) << duplicates.error().message;
  EXPECT_EQ(*duplicates,
            (std::vector<CellRef>{first, skip, last, first, skip, last}));

  const auto captured = h.execute(
      "def reveal($item: Cell { +d.tag: $value: String }): String := $value; "
      "##/d.items[{+d.tag: \"yes\"}]/reveal()");
  ASSERT_TRUE(captured) << captured.error().message;
  EXPECT_EQ(*captured, (std::vector<CellRef>{firstTag, skipTag, lastTag}));

  const auto frozen = h.execute(
      "let $selected := /d.items[{+d.tag: \"yes\"}]; "
      "let $frozen := /$selected; let $selected := /d.absent; ##/$frozen");
  ASSERT_TRUE(frozen) << frozen.error().message;
  EXPECT_EQ(*frozen, (std::vector<CellRef>{first, skip, last}));
}

TEST(VQLShapeTargetVMTest,
     ForeignNestedTargetProbesPreserveIdentityAndDoNotWalkOuterRank) {
  auto store   = std::make_shared<Store>(std::make_shared<UserPermascroll>());
  auto version = store->sliceGenesis({});
  const auto itemsAxis = store->makeDimension(version, "d.items");
  version              = itemsAxis.version;
  const auto videoAxis = store->makeDimension(version, "d.video");
  version              = videoAxis.version;
  const auto codecAxis = store->makeDimension(version, "d.codec");
  version              = codecAxis.version;
  const auto nameAxis  = store->makeDimension(version, "d.name");
  version              = nameAxis.version;
  const auto makeCell  = [&](const std::string_view text) {
    version = store->makeCell(version, text);
    return store->cellRefOf(version).value();
  };
  const auto row       = makeCell("row");
  const auto nextRow   = makeCell("next row");
  const auto video     = makeCell("video");
  const auto nextVideo = makeCell("next video");
  const auto codec     = makeCell("h264");
  const auto home      = *store->homeCell();
  version = store->setLink(version, home, itemsAxis.dim, DimVector::POS, row);
  version =
      store->setLink(version, row, itemsAxis.dim, DimVector::POS, nextRow);
  version = store->setLink(version, row, videoAxis.dim, DimVector::POS, video);
  version =
      store->setLink(version, video, videoAxis.dim, DimVector::POS, nextVideo);
  version =
      store->setLink(version, video, codecAxis.dim, DimVector::POS, codec);
  version = store->setLink(version, codec, nameAxis.dim, DimVector::POS,
                           makeCell("h264"));
  const auto sourceHead = store->latest();

  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  coordinator.addStore("foreign", "primary", store, version);
  const auto anchor  = *coordinator.resolveNamedStore("foreign");
  const auto foreign = arena.foreignOf(anchor);
  ASSERT_TRUE(foreign);
  const auto space = foreign->space;
  EXPECT_FALSE(arena.findExistingProxy(space, nextVideo));

  QueryExecutor executor(coordinator, vm);
  const auto result =
      executor.execute("##foreign/d.items[{+d.video: {+d.codec: \"h264\"}}]");
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  const auto actual = arena.foreignOf(result->front());
  ASSERT_TRUE(actual);
  EXPECT_EQ(actual->index, row);
  EXPECT_FALSE(arena.findExistingProxy(space, nextVideo));
  EXPECT_EQ(store->latest(), sourceHead);
}

TEST(VQLShapeTargetVMTest, ForeignScalarBitsAndCloneMastersRemainExact) {
  auto store   = std::make_shared<Store>(std::make_shared<UserPermascroll>());
  auto version = store->sliceGenesis({});
  const auto items = store->makeDimension(version, "d.items");
  version          = items.version;
  const auto score = store->makeDimension(version, "d.score");
  version          = score.version;
  const auto clone = store->makeDimension(version, "d.clone");
  version          = clone.version;
  const auto make  = [&](const std::string_view text) {
    version = store->makeCell(version, text);
    return store->cellRefOf(version).value();
  };
  const auto exactRow  = make("exact integer");
  const auto doubleRow = make("double");
  const auto cloneRow  = make("clone");
  const auto falseRow  = make("false");
  const auto exact     = make("misleading display");
  version              = store->setValue(
      version, exact, store->userPermascroll().append("0"), ValueKind::Int64,
      std::bit_cast<std::uint64_t>(INT64_C(9007199254740993)));
  const auto real = make("misleading double");
  version =
      store->setValue(version, real, store->userPermascroll().append("wrong"),
                      ValueKind::Double, std::bit_cast<std::uint64_t>(0x1p53));
  const auto falseValue = make("misleading bool");
  version               = store->setValue(version, falseValue,
                                          store->userPermascroll().append("true"),
                                          ValueKind::Bool, 0);
  const auto cloneValue = make("clone display");
  version =
      store->setLink(version, exact, clone.dim, DimVector::POS, cloneValue);
  const std::vector<CellRef> rows{exactRow, doubleRow, cloneRow, falseRow};
  const std::vector<CellRef> values{exact, real, cloneValue, falseValue};
  auto tail = *store->homeCell();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    version = store->setLink(version, tail, items.dim, DimVector::POS, rows[i]);
    version =
        store->setLink(version, rows[i], score.dim, DimVector::POS, values[i]);
    tail = rows[i];
  }
  version               = store->makeOpHandle(version, exact, "opaque handle");
  const auto handle     = store->cellRefOf(version).value();
  const auto sourceHead = store->latest();
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  coordinator.addStore("foreign", "primary", store, version);
  const auto anchor = *coordinator.resolveNamedStore("foreign");
  const auto space  = arena.foreignOf(anchor)->space;
  QueryExecutor executor(coordinator, vm);
  for (const auto &[query, expected] :
       std::vector<std::pair<std::string, std::vector<CellRef>>>{
           {"##foreign/d.items[{+d.score: 9007199254740993}]",
            {exactRow, cloneRow}},
           {"##foreign/d.items[{+d.score: 9007199254740992.0}]", {doubleRow}},
           {"##foreign/d.items[{+d.score: false}]", {falseRow}}}) {
    SCOPED_TRACE(query);
    const auto result = executor.execute(query);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
      ASSERT_TRUE(arena.foreignOf((*result)[i]));
      EXPECT_EQ(arena.foreignOf((*result)[i])->index, expected[i]);
    }
  }
  QueryExecutor handles(coordinator, vm, {},
                        {{"handle", {arena.proxyFor(space, handle)}}});
  const auto refused = handles.execute("if $handle = 0 return ##");
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().message.find("structural handles"),
            std::string::npos);
  EXPECT_EQ(store->latest(), sourceHead);
}

TEST(VQLShapeTargetVMTest, RuntimeBudgetFailureRecoversAfterReset) {
  Harness h;
  const auto matched = h.item("matched");
  h.link(matched, h.tag, DimVector::POS, h.arena.makeCell("yes"));

  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile("##/d.items[{+d.tag: \"yes\"}]");
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_TRUE(compiled.cursor);
  ASSERT_TRUE(compiled.entryOpcode);

  const auto beforeCount = h.arena.cellCount();
  h.arena.setExecutionLimits(100'000, 0, 100'000);
  const auto exhausted = h.vm.run(*compiled.cursor, 100'000);
  EXPECT_FALSE(exhausted.success);
  EXPECT_NE(exhausted.errorMessage.find("budget"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), beforeCount);

  h.arena.setExecutionLimits(100'000, 100'000, 100'000);
  h.vm.setCursorOpcode(*compiled.cursor, compiled.entryOpcode);
  const auto retried = h.vm.run(*compiled.cursor, 100'000);
  ASSERT_TRUE(retried.success) << retried.errorMessage;
  ASSERT_EQ(compiled.resultSlots.size(), 1u);
  EXPECT_EQ(h.vm.resultCells(compiled.resultSlots.front()),
            (std::vector<CellRef>{matched}));
}

TEST(VQLShapeTargetVMTest, DynamicTargetsRemainExplicitCompileRefusals) {
  Harness h;
  for (const auto query : {
           "let $expected := %\"yes\"; ##/d.items[{+d.tag: $expected}]",
           "let $expected := %\"yes\"; ##/d.items[{!d.tag: $expected}]",
       }) {
    SCOPED_TRACE(query);
    const auto result = h.execute(query);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, QueryExecutionError::Category::Compile);
  }
}

} // namespace
} // namespace xanadu::vql
