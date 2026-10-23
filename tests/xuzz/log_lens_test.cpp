/**
 * @file log_lens_test.cpp
 * @brief LogLens: a log file's tail as ordinal-paged windows -- the
 *        second WindowedLens (design/lens-candidates.md section 3.4).
 */
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/log_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace {

namespace fs = std::filesystem;
using zigzag::CellRef;
using zigzag::LogLens;
using zigzag::OptionalCell;
using zigzag::Posward;
using zigzag::ProjectionSession;
using zigzag::vortex::VortexCore;

struct ProjectedLog {
  fs::path path;
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  LogLens *lens{nullptr};
  std::unique_ptr<ProjectionSession> session;

  /// @p n lines named line-0 .. line-(n-1); @p pageSize lines a page.
  explicit ProjectedLog(std::size_t n, std::size_t pageSize = 3) {
    path = fs::temp_directory_path() /
           ("log-lens-" + std::to_string(::getpid()) + ".log");
    {
      std::ofstream f(path);
      for (std::size_t i = 0; i < n; ++i) {
        f << "line-" << i << "\n";
      }
    }
    auto l  = std::make_unique<LogLens>(path.string(), pageSize);
    lens    = l.get();
    session = std::make_unique<ProjectionSession>(std::move(l), core);
  }
  ~ProjectedLog() { fs::remove(path); }

  CellRef dim(std::string_view n) { return core.findOrMintDimension(n); }
  OptionalCell child(CellRef c) {
    return arena.linked(c, dim("d.contains"), Posward);
  }
  OptionalCell next(CellRef c) {
    return arena.linked(c, dim("d.seq"), Posward);
  }
  std::vector<CellRef> children(CellRef c) {
    std::vector<CellRef> out;
    for (auto at = child(c); at.has_value(); at = next(*at)) {
      out.push_back(*at);
    }
    return out;
  }
  std::string attr(CellRef c, std::string_view rank) {
    const auto a = arena.linked(c, dim(rank), Posward);
    return a.has_value() ? arena.textOf(*a) : "";
  }
};

TEST(LogLensTest, TailWindowIsPageZeroWithOlderStubs) {
  // 8 lines, 3 a page -> tail page [5,8) live, stubs at 2 and 0.
  ProjectedLog p{8, 3};
  const auto file = p.session->roots().front();
  EXPECT_EQ(p.attr(file, "d.role"), "log.file");
  const auto pages = p.children(file);
  ASSERT_EQ(pages.size(), 3u);
  EXPECT_EQ(p.attr(pages[0], "d.winstate"), "live");
  EXPECT_EQ(p.attr(pages[1], "d.winstate"), "stub");
  EXPECT_EQ(p.attr(pages[2], "d.winstate"), "stub");
  // The tail page's lines, in file order, ordinal-keyed.
  const auto lines = p.children(pages[0]);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(p.arena.textOf(lines[0]), "line-5");
  EXPECT_EQ(p.arena.textOf(lines[2]), "line-7");
  EXPECT_EQ(p.attr(lines[0], "d.index"), "5");
  EXPECT_EQ(p.attr(lines[0], "d.keystab"), "ord");
}

TEST(LogLensTest, MaterializePageFetchesOlderLines) {
  ProjectedLog p{8, 3};
  const auto file  = p.session->roots().front();
  const auto pages = p.children(file);
  ASSERT_EQ(pages.size(), 3u);
  // The stub keys name their head ordinal: log:<path>:w:<firstLine>.
  const auto stubKey = "log:" + p.path.string() + ":w:2";
  EXPECT_TRUE(p.session->keyFor(pages[1]).has_value());
  EXPECT_EQ(*p.session->keyFor(pages[1]), stubKey);

  ASSERT_TRUE(p.lens->materializePage(stubKey).has_value());
  (void)p.session->refresh();
  const auto stubCell = p.session->cellFor(stubKey);
  ASSERT_TRUE(stubCell.has_value());
  EXPECT_EQ(p.attr(*stubCell, "d.winstate"), "live");
  const auto lines = p.children(*stubCell);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(p.arena.textOf(lines[0]), "line-2");
  EXPECT_EQ(p.arena.textOf(lines[2]), "line-4");
}

TEST(LogLensTest, LineOrdinalsSurviveAppends) {
  ProjectedLog p{5, 3};
  const auto file = p.session->roots().front();
  const auto tail = p.children(file);
  ASSERT_FALSE(tail.empty());
  const auto line3Key = "log:" + p.path.string() + ":l:3";
  const auto line3    = p.session->cellFor(line3Key);
  ASSERT_TRUE(line3.has_value());
  EXPECT_EQ(p.arena.textOf(*line3), "line-3");
  // An append shifts the tail's page membership but not the line's key.
  {
    std::ofstream f(p.path, std::ios::app);
    f << "line-5\n";
  }
  ASSERT_TRUE(p.lens->browse().has_value());
  (void)p.session->refresh();
  const auto again = p.session->cellFor(line3Key);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, *line3);
  EXPECT_EQ(p.arena.textOf(*again), "line-3");
}

TEST(LogLensTest, PollChangedSeesAnAppendAndRefreshReprojects) {
  ProjectedLog p{5, 3};
  EXPECT_FALSE(p.lens->pollChanged());
  {
    std::ofstream f(p.path, std::ios::app);
    f << "line-5\nline-6\n";
  }
  // The evented hint fires: the file's stamp moved since browse.
  EXPECT_TRUE(p.lens->pollChanged());
  // And refresh() self-checks: forEachNode re-browses a moved file even
  // without the app's funnel calling browse() first.
  (void)p.session->refresh();
  const auto line5 = p.session->cellFor("log:" + p.path.string() + ":l:5");
  ASSERT_TRUE(line5.has_value());
  EXPECT_EQ(p.arena.textOf(*line5), "line-5");
  EXPECT_FALSE(p.lens->pollChanged()); // stamp re-taken at re-browse
}

TEST(LogLensTest, RefreshIfChangedIsTheFunnel) {
  ProjectedLog p{5, 3};
  // The app's pull loop calls refreshIfChanged() -- it refreshes iff
  // the lens reports a moved source.
  EXPECT_FALSE(p.session->refreshIfChanged());
  {
    std::ofstream f(p.path, std::ios::app);
    f << "line-5\n";
  }
  EXPECT_TRUE(p.session->refreshIfChanged());
  EXPECT_TRUE(
      p.session->cellFor("log:" + p.path.string() + ":l:5").has_value());
  EXPECT_FALSE(p.session->refreshIfChanged()); // settled: no second run
}

} // namespace
