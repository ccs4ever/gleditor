#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <spdlog/sinks/ostream_sink.h>

#include <gleditor/logging.hpp>

#include "common/xanadu/view/view_records.hpp"

namespace {

using xanadu::view::DropTarget;
using xanadu::view::EdgeKind;
using xanadu::view::LayoutSink;
using xanadu::view::MotionHint;
using xanadu::view::PlacedEdge;
using xanadu::view::PlacedFrame;
using xanadu::view::PlacedItem;
using xanadu::view::placedPose;
using xanadu::view::SubjectId;
using xanadu::view::SubjectKind;

/// Captures what the sink says on its category, and puts the logger back.
class ViewLayoutLog {
public:
  ViewLayoutLog()
      : logger_(gleditor::logging::category("view.layout")),
        previousSinks_(logger_->sinks()), previousLevel_(logger_->level()) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_);
    sink->set_pattern("%v");
    logger_->sinks() = {std::move(sink)};
    logger_->set_level(spdlog::level::debug);
  }
  ViewLayoutLog(const ViewLayoutLog &)            = delete;
  ViewLayoutLog &operator=(const ViewLayoutLog &) = delete;
  ViewLayoutLog(ViewLayoutLog &&)                 = delete;
  ViewLayoutLog &operator=(ViewLayoutLog &&)      = delete;
  ~ViewLayoutLog() {
    logger_->sinks() = std::move(previousSinks_);
    logger_->set_level(previousLevel_);
  }

  [[nodiscard]] std::string text() const { return messages_.str(); }
  void reset() {
    messages_.str({});
    messages_.clear();
  }

private:
  std::ostringstream messages_;
  std::shared_ptr<spdlog::logger> logger_;
  std::vector<spdlog::sink_ptr> previousSinks_;
  spdlog::level::level_enum previousLevel_;
};

/// Far more of each record than any first allocation holds.
constexpr std::uint32_t kMany = 10'000;

void fill(LayoutSink &sink) {
  for (std::uint32_t i = 0; i < kMany; ++i) {
    const auto frame =
        sink.push(PlacedFrame{.id = SubjectId::viewCell(i, 1), .count = i});
    const auto item = sink.push(PlacedItem{.id    = SubjectId::cell(i),
                                           .width = static_cast<float>(i),
                                           .frame = frame});
    sink.push(PlacedEdge{.from     = SubjectId::cell(i),
                         .to       = SubjectId::cell(i + 1),
                         .kind     = EdgeKind::Strand,
                         .relation = i,
                         .label    = item})
        ->push(DropTarget{.axis = i})
        ->push(MotionHint{.id = SubjectId::cell(i)});
  }
}

} // namespace

TEST(ViewRecordsTest, SinkGrowsAndNeverDropsARecord) {
  const ViewLayoutLog log;
  LayoutSink sink;
  fill(sink);

  ASSERT_EQ(sink.items().size(), kMany);
  ASSERT_EQ(sink.frames().size(), kMany);
  ASSERT_EQ(sink.edges().size(), kMany);
  ASSERT_EQ(sink.dropTargets().size(), kMany);
  ASSERT_EQ(sink.hints().size(), kMany);
  for (std::uint32_t i = 0; i < kMany; ++i) {
    EXPECT_EQ(sink.items()[i].id, SubjectId::cell(i));
    EXPECT_EQ(sink.items()[i].frame, i);
    EXPECT_EQ(sink.frames()[i].count, i);
    EXPECT_EQ(sink.edges()[i].relation, i);
    EXPECT_EQ(sink.edges()[i].label, i);
    EXPECT_EQ(sink.dropTargets()[i].axis, i);
  }
}

TEST(ViewRecordsTest, SinkLogsEachGrowthOnItsCategory) {
  ViewLayoutLog log;
  LayoutSink sink;
  fill(sink);
  const auto grown = log.text();
  for (const char *kind :
       {"items", "frames", "edges", "drop targets", "motion hints"}) {
    EXPECT_NE(grown.find(std::string("grew its ") + kind), std::string::npos)
        << kind << " grew silently:\n"
        << grown;
  }

  // A sink that has reached its size is reused without growing: the steady
  // state of a layout allocates nothing and so says nothing.
  log.reset();
  sink.clear();
  EXPECT_TRUE(sink.items().empty());
  EXPECT_TRUE(sink.hints().empty());
  fill(sink);
  EXPECT_EQ(log.text(), "");
  EXPECT_EQ(sink.items().size(), kMany);
}

TEST(ViewRecordsTest, PushAnswersTheIndexARecordIsNamedBy) {
  LayoutSink sink;
  EXPECT_EQ(sink.push(PlacedItem{}), 0U);
  EXPECT_EQ(sink.push(PlacedItem{}), 1U);
  EXPECT_EQ(sink.push(PlacedFrame{}), 0U);
  EXPECT_EQ(sink.clear()->push(PlacedItem{}), 0U);
}

TEST(ViewRecordsTest, OneCellInTwoPlacesIsTwoSubjects) {
  EXPECT_NE(SubjectId::cell(7, 0), SubjectId::cell(7, 1));
  EXPECT_EQ(SubjectId::cell(7, 1), SubjectId::cell(7, 1));
}

TEST(ViewRecordsTest, ARealCellKeepsItsIdentityAcrossATossAndAViewCellDoesNot) {
  // A toss truncates the derived arena, so the next generation reuses its
  // indices: ref 3 of epoch 1 and ref 3 of epoch 2 are different cells.
  EXPECT_NE(SubjectId::viewCell(3, 1), SubjectId::viewCell(3, 2));
  // A real cell carries no epoch, so it tweens across the toss.
  EXPECT_EQ(SubjectId::cell(3).epoch, 0U);
  // A view cell is never the real cell of the same number.
  EXPECT_NE(SubjectId::viewCell(3, 0), SubjectId::cell(3));
}

TEST(ViewRecordsTest, APageNamesItsDocumentAndPlace) {
  const auto id = SubjectId::page(2, 5);
  EXPECT_EQ(id.kind, SubjectKind::Page);
  EXPECT_EQ(id.value >> 32U, 2U);
  EXPECT_EQ(id.value & 0xFFFF'FFFFU, 5U);
  EXPECT_NE(SubjectId::page(2, 5), SubjectId::page(5, 2));
  EXPECT_NE(SubjectId::page(0, 0), SubjectId::document(0));
}

TEST(ViewRecordsTest, EqualSubjectsHashEqualAndDistinctOnesSpread) {
  const std::hash<SubjectId> hash;
  EXPECT_EQ(hash(SubjectId::viewCell(9, 4, 1)),
            hash(SubjectId::viewCell(9, 4, 1)));
  std::unordered_set<std::size_t> seen;
  for (std::uint32_t ref = 0; ref < 64; ++ref) {
    for (std::uint64_t epoch = 1; epoch < 4; ++epoch) {
      seen.insert(hash(SubjectId::viewCell(ref, epoch)));
    }
  }
  EXPECT_EQ(seen.size(), 64U * 3U);
}

TEST(ViewRecordsTest, AThingInAFrameIsPlacedRelativeToIt) {
  LayoutSink sink;
  const auto quarterTurn =
      glm::angleAxis(glm::radians(90.0F), glm::vec3{0.0F, 0.0F, 1.0F});
  const auto document = sink.push(PlacedFrame{.centre = {100.0F, 0.0F, 0.0F}});
  const auto pack     = sink.push(PlacedFrame{.centre      = {10.0F, 0.0F, 0.0F},
                                              .orientation = quarterTurn,
                                              .parent      = document});
  const PlacedItem part{.centre = {1.0F, 0.0F, 0.0F}, .frame = pack};
  sink.push(part);

  const auto pose = placedPose(sink, part);
  ASSERT_TRUE(pose);
  EXPECT_NEAR(pose->centre.x, 110.0F, 1e-4F);
  EXPECT_NEAR(pose->centre.y, 1.0F, 1e-4F);

  // Moving the document carries the pack and the part with it.
  LayoutSink moved;
  moved.push(PlacedFrame{.centre = {0.0F, 50.0F, 0.0F}});
  moved.push(sink.frames()[pack]);
  const auto movedPose = placedPose(moved, part);
  ASSERT_TRUE(movedPose);
  EXPECT_NEAR(movedPose->centre.x, 10.0F, 1e-4F);
  EXPECT_NEAR(movedPose->centre.y, 51.0F, 1e-4F);
}

TEST(ViewRecordsTest, AFrameThatCannotBePlacedIsNotPlacedAtTheOrigin) {
  LayoutSink sink;
  sink.push(PlacedFrame{.parent = 1U});
  sink.push(PlacedFrame{.parent = 0U});
  EXPECT_FALSE(placedPose(sink, PlacedItem{.frame = 0U}));
  EXPECT_FALSE(placedPose(sink, PlacedItem{.frame = 7U}));
  EXPECT_TRUE(placedPose(sink, PlacedItem{}));
}
