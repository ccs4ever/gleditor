#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/sinks/ostream_sink.h>

#include <gleditor/logging.hpp>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/page/coalesce.hpp"

namespace {

using xanadu::LayoutConfig;
using xanadu::view::CoalesceBody;
using xanadu::view::CoalesceTie;
using xanadu::view::PageRef;
using xanadu::view::TensionCoalesce;

constexpr float kWidth  = 800.0F;
constexpr float kHeight = 1000.0F;

/// The settings a fresh system://layout is seeded with.
const LayoutConfig &settings() {
  static const LayoutConfig config;
  return config;
}

TensionCoalesce strategy() {
  return {settings().physics.toTensionParams(), settings().pageBase};
}

float gap() { return settings().pageBase.coalesceGap; }
float tolerance() { return settings().pageBase.levelTolerance; }

/// The foreground row LinkBeams lays out today (beams.cpp, docSlots): the
/// first document's centre at zero, each next one its half width, the gap
/// and the previous half width further on. With physics off, the default,
/// this row is what users see while a link is active: documents keep their
/// slots and the one brought over moves only up or down.
std::vector<float> docSlots(const std::vector<float> &widths) {
  std::vector<float> slots(widths.size(), 0.0F);
  for (std::size_t d = 1; d < widths.size(); ++d) {
    slots[d] =
        slots[d - 1] + (0.5F * widths[d - 1]) + (0.5F * widths[d]) + gap();
  }
  return slots;
}

/// One body per document, as LinkBeams loads its engine: every document of
/// the row, the anchor pinned.
std::vector<CoalesceBody> row(const std::vector<float> &widths,
                              const std::uint32_t anchor) {
  const auto slots = docSlots(widths);
  std::vector<CoalesceBody> bodies;
  for (std::uint32_t d = 0; d < widths.size(); ++d) {
    bodies.push_back({.page   = {.document = d},
                      .home   = {slots[d], 0.0F, 0.0F},
                      .width  = widths[d],
                      .height = kHeight,
                      .pinned = d == anchor});
  }
  return bodies;
}

CoalesceTie tie(const std::uint32_t from, const std::uint32_t to,
                const float fromHeight, const float toHeight) {
  return {.from       = from,
          .to         = to,
          .fromHeight = fromHeight,
          .toHeight   = toHeight,
          .gap        = gap()};
}

/// Where a passage stands, in the placement's y-up units.
float passageY(const CoalesceBody &body, const float height) {
  return body.position.y + (0.5F * body.height) - height;
}

void expectLevel(const std::vector<CoalesceBody> &bodies,
                 const std::vector<CoalesceTie> &ties) {
  for (const auto &t : ties) {
    EXPECT_NEAR(passageY(bodies[t.from], t.fromHeight),
                passageY(bodies[t.to], t.toHeight), tolerance())
        << "tie " << t.from << " to " << t.to;
  }
}

void expectApart(const std::vector<CoalesceBody> &bodies) {
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    for (std::size_t j = i + 1; j < bodies.size(); ++j) {
      const auto &a = bodies[i];
      const auto &b = bodies[j];
      const bool acrossX =
          std::abs(a.position.x - b.position.x) < 0.5F * (a.width + b.width);
      const bool acrossY =
          std::abs(a.position.y - b.position.y) < 0.5F * (a.height + b.height);
      EXPECT_FALSE(acrossX && acrossY) << "bodies " << i << " and " << j;
    }
  }
}

/// Parity with the row: every document in its slot, every tied passage
/// level with the anchor's, every untied document at its home height.
void expectRowParity(const std::vector<CoalesceBody> &bodies,
                     const std::vector<CoalesceTie> &ties,
                     const std::vector<float> &widths) {
  const auto slots = docSlots(widths);
  for (std::size_t d = 0; d < bodies.size(); ++d) {
    EXPECT_NEAR(bodies[d].position.x, slots[d], tolerance()) << "doc " << d;
    EXPECT_EQ(bodies[d].position.z, bodies[d].home.z);
    const bool tied = std::ranges::any_of(
        ties, [d](const auto &t) { return t.from == d || t.to == d; });
    if (!tied) {
      EXPECT_EQ(bodies[d].position.y, bodies[d].home.y) << "doc " << d;
    }
  }
  expectLevel(bodies, ties);
}

TEST(CoalesceTest, twoDocumentsMatchTheRowUsersSee) {
  const std::vector widths{kWidth, kWidth};
  const auto s = strategy();

  auto bodies = row(widths, 0);
  const std::vector ties{tie(0, 1, 300.0F, 700.0F)};
  s.solve(bodies, ties);
  expectRowParity(bodies, ties, widths);
  EXPECT_EQ(bodies[0].position, bodies[0].home);
  EXPECT_FLOAT_EQ(bodies[1].position.y, 400.0F);

  // The reader at the right-hand document: the left one is not dragged
  // across the row to the anchor's right.
  auto mirrored = row(widths, 1);
  const std::vector back{tie(1, 0, 700.0F, 300.0F)};
  s.solve(mirrored, back);
  expectRowParity(mirrored, back, widths);
}

TEST(CoalesceTest, threeDocumentsMatchTheRowUsersSee) {
  const auto s = strategy();

  // Ends on both sides of the reader, in documents of three widths.
  const std::vector widths{600.0F, kWidth, 1000.0F};
  auto between = row(widths, 1);
  const std::vector ties{tie(1, 0, 450.0F, 120.0F), tie(1, 2, 450.0F, 880.0F)};
  s.solve(between, ties);
  expectRowParity(between, ties, widths);

  // A document between the two ends takes no part and keeps its slot; the
  // far end does not land on it.
  const std::vector same{kWidth, kWidth, kWidth};
  auto across = row(same, 0);
  const std::vector far{tie(0, 2, 200.0F, 600.0F)};
  s.solve(across, far);
  expectRowParity(across, far, same);
}

TEST(CoalesceTest, manyDocumentsMatchTheRowUsersSee) {
  const auto s = strategy();

  // One link with an end in each of five documents beside the reader's,
  // solved at once rather than a pair a frame.
  const std::vector widths(6, kWidth);
  auto rightwards = row(widths, 0);
  std::vector<CoalesceTie> ties;
  for (std::uint32_t d = 1; d < widths.size(); ++d) {
    ties.push_back(tie(0, d, 500.0F, 150.0F * static_cast<float>(d)));
  }
  s.solve(rightwards, ties);
  expectRowParity(rightwards, ties, widths);

  // The reader in the middle of seven, with ends on both sides.
  const std::vector seven(7, kWidth);
  auto both = row(seven, 3);
  std::vector<CoalesceTie> spread;
  for (std::uint32_t d = 0; d < seven.size(); ++d) {
    if (d != 3) {
      spread.push_back(
          tie(3, d, 250.0F, 100.0F + (110.0F * static_cast<float>(d))));
    }
  }
  s.solve(both, spread);
  expectRowParity(both, spread, seven);
}

TEST(CoalesceTest, theSameInputGivesTheSamePositions) {
  const auto s = strategy();
  const std::vector widths(5, kWidth);
  std::vector<CoalesceTie> ties;
  for (std::uint32_t d = 0; d < widths.size(); ++d) {
    if (d != 2) {
      ties.push_back(tie(2, d, 400.0F, 90.0F * static_cast<float>(d + 1)));
    }
  }

  auto first = row(widths, 2);
  s.solve(first, ties);

  // Positions left anywhere by an earlier solve or an animation are not read:
  // a solve starts from the row every time.
  auto second = row(widths, 2);
  for (auto &body : second) {
    body.position = {-12345.0F, 6789.0F, 42.0F};
  }
  s.solve(second, ties);
  auto third = first;
  s.solve(third, ties);

  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].position, second[i].position) << "body " << i;
    EXPECT_EQ(first[i].position, third[i].position) << "body " << i;
  }
}

TEST(CoalesceTest, pagesOfOneColumnAreLevelledAndKeptApart) {
  const auto s = strategy();
  // The anchor is the first page of one document; four pages of the next
  // document hold the other ends. Their homes share one column, so the
  // springs pull them all to one place.
  std::vector<CoalesceBody> bodies{
      {.page   = {.document = 0, .page = 0},
       .home   = {0.0F, 0.0F, 0.0F},
       .width  = kWidth,
       .height = kHeight,
       .pinned = true},
  };
  std::vector<CoalesceTie> ties;
  for (std::uint32_t p = 0; p < 4; ++p) {
    bodies.push_back(
        {.page   = {.document = 1, .page = p},
         .home   = {kWidth + gap(), -static_cast<float>(p) * 1032.0F, 0.0F},
         .width  = kWidth,
         .height = kHeight});
    ties.push_back(
        tie(0, p + 1, 480.0F, 300.0F + (50.0F * static_cast<float>(p))));
  }
  s.solve(bodies, ties);

  EXPECT_EQ(bodies[0].position, bodies[0].home);
  expectLevel(bodies, ties);
  expectApart(bodies);
  // In the order the column had them, outward from the anchor.
  for (std::size_t i = 2; i < bodies.size(); ++i) {
    EXPECT_GT(bodies[i].position.x, bodies[i - 1].position.x);
  }
}

TEST(CoalesceTest, noTwoPagesOverlap) {
  const auto s = strategy();
  // Pages of different sizes, some sharing a column, ends on both sides of
  // the anchor and a chain of ties two deep.
  std::vector<CoalesceBody> bodies{
      {.home   = {0.0F, 0.0F, 0.0F},
       .width  = 900.0F,
       .height = 1200.0F,
       .pinned = true},
      {.home = {-1300.0F, 0.0F, 0.0F}, .width = 500.0F, .height = 700.0F},
      {.home = {-1300.0F, -900.0F, 0.0F}, .width = 500.0F, .height = 700.0F},
      {.home = {1400.0F, 0.0F, 0.0F}, .width = 1100.0F, .height = 900.0F},
      {.home = {1400.0F, -1000.0F, 0.0F}, .width = 1100.0F, .height = 900.0F},
      {.home = {2900.0F, 0.0F, 0.0F}, .width = 600.0F, .height = 1000.0F},
  };
  const std::vector ties{
      tie(0, 1, 100.0F, 600.0F), tie(0, 2, 1100.0F, 50.0F),
      tie(0, 3, 600.0F, 450.0F), tie(0, 4, 600.0F, 800.0F),
      tie(3, 5, 450.0F, 30.0F),
  };
  s.solve(bodies, ties);

  EXPECT_EQ(bodies[0].position, bodies[0].home);
  expectLevel(bodies, ties);
  expectApart(bodies);
  // Each side stays on its side.
  EXPECT_LT(bodies[1].position.x, 0.0F);
  EXPECT_LT(bodies[2].position.x, 0.0F);
  for (std::size_t i = 3; i < bodies.size(); ++i) {
    EXPECT_GT(bodies[i].position.x, 0.0F);
  }
}

TEST(CoalesceTest, aTieThatNamesNoPageIsIgnored) {
  const auto s = strategy();
  const std::vector widths{kWidth, kWidth};
  auto bodies = row(widths, 0);
  const std::vector ties{tie(0, 1, 300.0F, 300.0F), tie(0, 9, 1.0F, 2.0F),
                         tie(1, 1, 5.0F, 6.0F)};
  s.solve(bodies, ties);
  expectRowParity(bodies, {ties.front()}, widths);
}

/// Captures what the strategy says on its category, and puts it back.
class LayoutLog {
public:
  LayoutLog()
      : logger_(gleditor::logging::category("view.layout")),
        previousSinks_(logger_->sinks()), previousLevel_(logger_->level()) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_);
    sink->set_pattern("%v");
    logger_->sinks() = {std::move(sink)};
    logger_->set_level(spdlog::level::debug);
  }
  LayoutLog(const LayoutLog &)            = delete;
  LayoutLog &operator=(const LayoutLog &) = delete;
  LayoutLog(LayoutLog &&)                 = delete;
  LayoutLog &operator=(LayoutLog &&)      = delete;
  ~LayoutLog() {
    logger_->sinks() = std::move(previousSinks_);
    logger_->set_level(previousLevel_);
  }
  [[nodiscard]] std::string text() const { return messages_.str(); }

private:
  std::ostringstream messages_;
  std::shared_ptr<spdlog::logger> logger_;
  std::vector<spdlog::sink_ptr> previousSinks_;
  spdlog::level::level_enum previousLevel_;
};

// Spike S2 measured 478 steps for the slowest of today's scenes; the cap is
// there for a scene that never settles, not to cut a normal one short.
TEST(CoalesceTest, aManyEndedLinkSettlesBeforeTheCap) {
  const LayoutLog log;
  const auto s = strategy();
  const std::vector widths(6, kWidth);
  auto bodies = row(widths, 0);
  std::vector<CoalesceTie> ties;
  for (std::uint32_t d = 1; d < widths.size(); ++d) {
    ties.push_back(tie(0, d, 500.0F, 500.0F));
  }
  s.solve(bodies, ties);
  EXPECT_NE(log.text().find("settled after"), std::string::npos) << log.text();
}

} // namespace
