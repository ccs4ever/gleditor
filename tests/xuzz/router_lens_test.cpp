/**
 * @file router_lens_test.cpp
 * @brief RouterLens: cross-lens composition -- a filesystem view that
 *        mounts GitLens/LogLens/ConfigLens under the focused cell, one
 *        hop in and one hop out.
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
#include "common/xanadu/zigzag/filesystem_lens.hpp"
#include "common/xanadu/zigzag/lens_uri.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"
#include "common/xanadu/zigzag/router_lens.hpp"

namespace {

namespace fs = std::filesystem;
using zigzag::CellRef;
using zigzag::ExternalKey;
using zigzag::FilesystemLens;
using zigzag::Negward;
using zigzag::OptionalCell;
using zigzag::Posward;
using zigzag::ProjectionSession;
using zigzag::RouterLens;
using zigzag::vortex::VortexCore;

/// A scratch directory holding a git worktree, a log file, a JSON
/// config and a plain text file -- one classifiable cell per delegate.
struct RouterFixture {
  fs::path dir;
  fs::path repo;
  fs::path logFile;
  fs::path jsonFile;
  fs::path txtFile;

  RouterFixture() {
    dir = fs::temp_directory_path() /
          ("router-lens-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    repo = dir / "repo";
    fs::create_directories(repo);
    run("git init -q -b main " + repo.string());
    run("git -C " + repo.string() + " config user.email t@t && git -C " +
        repo.string() + " config user.name t");
    for (int i = 1; i <= 3; ++i) {
      run("git -C " + repo.string() + " commit -q --allow-empty -m c" +
          std::to_string(i));
    }

    logFile = dir / "app.log";
    {
      std::ofstream f(logFile);
      for (int i = 0; i < 70; ++i) { // > LogLens's 64-line page: a stub
        f << "line-" << i << "\n";
      }
    }

    jsonFile = dir / "settings.json";
    {
      std::ofstream f(jsonFile);
      f << "{\"name\":\"demo\",\"limit\":3}\n";
    }

    txtFile = dir / "notes.txt";
    {
      std::ofstream f(txtFile);
      f << "plain text, no delegate\n";
    }
  }
  ~RouterFixture() { fs::remove_all(dir); }

  static void run(const std::string &cmd) {
    EXPECT_EQ(::system(cmd.c_str()), 0) << cmd;
  }
};

struct ProjectedRouter {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  RouterLens *lens{nullptr};
  std::unique_ptr<ProjectionSession> session;

  explicit ProjectedRouter(const fs::path &root) {
    auto l  = std::make_unique<RouterLens>(root);
    lens    = l.get();
    session = std::make_unique<ProjectionSession>(std::move(l), core);
  }
  CellRef dim(std::string_view n) { return core.findOrMintDimension(n); }
  OptionalCell child(CellRef c) {
    return arena.linked(c, dim("d.contains"), Posward);
  }
  OptionalCell next(CellRef c) {
    return arena.linked(c, dim("d.seq"), Posward);
  }
  OptionalCell parentOf(CellRef c) {
    return arena.linked(c, dim("d.contains"), Negward);
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
  /// The child of @p parent whose cell text is @p name.
  OptionalCell childNamed(CellRef parent, const std::string &name) {
    for (const auto c : children(parent)) {
      if (arena.textOf(c) == name) {
        return c;
      }
    }
    return std::nullopt;
  }
};

TEST(RouterLensTest, BrowseShowsTheFilesystemProjection) {
  RouterFixture fix;
  ProjectedRouter p{fix.dir};
  ASSERT_EQ(p.session->roots().size(), 1u);
  const auto root = p.session->roots().front();
  EXPECT_EQ(p.attr(root, "d.role"), "dir");
  EXPECT_TRUE(p.childNamed(root, "repo").has_value());
  EXPECT_TRUE(p.childNamed(root, "app.log").has_value());
  EXPECT_TRUE(p.childNamed(root, "settings.json").has_value());
  EXPECT_TRUE(p.childNamed(root, "notes.txt").has_value());
  EXPECT_EQ(p.lens->mountCount(), 0u);
}

TEST(RouterLensTest, OpenGitWorktreeMountsUnderTheDirCell) {
  RouterFixture fix;
  ProjectedRouter p{fix.dir};
  const auto repoCell = p.childNamed(p.session->roots().front(), "repo");
  ASSERT_TRUE(repoCell.has_value());
  const auto repoKey = p.session->keyFor(*repoCell);
  ASSERT_TRUE(repoKey.has_value());

  const auto opened = p.lens->open(*repoKey);
  ASSERT_TRUE(opened.has_value()) << opened.error();
  EXPECT_EQ(*opened, "git:" + fix.repo.string());
  (void)p.session->refresh();

  // One hop in: the delegate root IS the dir cell's d.contains child
  // (it head-splices ahead of the fs children on d.seq).
  const auto gitRoot = p.session->cellFor(*opened);
  ASSERT_TRUE(gitRoot.has_value());
  EXPECT_EQ(p.attr(*gitRoot, "d.role"), "git.repo");
  const auto head = p.child(*repoCell);
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(*head, *gitRoot);

  // The dir's own fs child (.git) is still in the same child run,
  // seq-chained behind the delegate root.
  const auto kids = p.children(*repoCell);
  ASSERT_GE(kids.size(), 2u);
  EXPECT_EQ(kids.front(), *gitRoot);
  EXPECT_EQ(p.arena.textOf(kids[1]), ".git");

  // The delegate's own structure arrived: refs, a live page, the DAG.
  bool sawRef = false;
  CellRef page{};
  for (const auto k : p.children(*gitRoot)) {
    if (p.attr(k, "d.role") == "git.ref") sawRef = true;
    if (p.attr(k, "d.role") == "git.page") page = k;
  }
  EXPECT_TRUE(sawRef);
  const auto commits = p.children(page);
  ASSERT_EQ(commits.size(), 3u);
  EXPECT_EQ(p.arena.textOf(commits[0]), "c3");
  const auto parent = p.arena.linked(commits[0], p.dim("d.parent"), Posward);
  ASSERT_TRUE(parent.has_value());
  EXPECT_EQ(p.arena.textOf(*parent), "c2");

  // And back out through the same cells: the git root's negward
  // d.contains is the dir cell it hangs under.
  const auto up = p.parentOf(*gitRoot);
  ASSERT_TRUE(up.has_value());
  EXPECT_EQ(*up, *repoCell);
  EXPECT_EQ(p.lens->mountCount(), 1u);
}

TEST(RouterLensTest, UnmountRestoresTheFilesystemOnlyView) {
  RouterFixture fix;
  ProjectedRouter p{fix.dir};
  const auto repoCell = p.childNamed(p.session->roots().front(), "repo");
  ASSERT_TRUE(repoCell.has_value());
  const auto repoKey = p.session->keyFor(*repoCell);
  ASSERT_TRUE(repoKey.has_value());
  ASSERT_TRUE(p.lens->open(*repoKey).has_value());
  (void)p.session->refresh();
  const auto gitRoot = p.session->cellFor("git:" + fix.repo.string());
  ASSERT_TRUE(gitRoot.has_value());

  ASSERT_TRUE(p.lens->unmount(*repoKey).has_value());
  (void)p.session->refresh();

  // The fs child run is whole again: .git is the d.contains child.
  const auto head = p.child(*repoCell);
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(p.arena.textOf(*head), ".git");
  for (const auto k : p.children(*repoCell)) {
    EXPECT_NE(p.attr(k, "d.role"), "git.repo");
  }
  // The delegate's cells stay in the arena, stale-marked by the
  // absent-key rule.
  EXPECT_EQ(p.attr(*gitRoot, "d.stale"), "1");
  // The mount itself is gone.
  EXPECT_EQ(p.lens->mountCount(), 0u);
  EXPECT_FALSE(p.lens->isMounted(*repoKey));
  // The fs cells around the mount were never touched.
  EXPECT_TRUE(p.childNamed(p.session->roots().front(), "app.log").has_value());
}

TEST(RouterLensTest, OpenLogFileHangsPagesUnderTheFileCell) {
  RouterFixture fix;
  ProjectedRouter p{fix.dir};
  const auto fileCell = p.childNamed(p.session->roots().front(), "app.log");
  ASSERT_TRUE(fileCell.has_value());
  const auto fileKey = p.session->keyFor(*fileCell);
  ASSERT_TRUE(fileKey.has_value());

  const auto opened = p.lens->open(*fileKey);
  ASSERT_TRUE(opened.has_value()) << opened.error();
  EXPECT_EQ(*opened, "log:" + fix.logFile.string());
  (void)p.session->refresh();

  // The file cell had no fs children, so the delegate root is its
  // direct d.contains child.
  const auto logRoot = p.session->cellFor(*opened);
  ASSERT_TRUE(logRoot.has_value());
  EXPECT_EQ(p.attr(*logRoot, "d.role"), "log.file");
  const auto head = p.child(*fileCell);
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(*head, *logRoot);

  // Page 0 is the live tail; the router forwards nextStub so the
  // next-page gesture still reaches the mounted windowed lens.
  const auto pages = p.children(*logRoot);
  ASSERT_FALSE(pages.empty());
  EXPECT_EQ(p.attr(pages.front(), "d.winstate"), "live");
  const auto stub = p.lens->nextStub();
  ASSERT_TRUE(stub.has_value());
  ASSERT_TRUE(p.lens->materializePage(*stub).has_value());
  (void)p.session->refresh();
  const auto stubCell = p.session->cellFor(*stub);
  ASSERT_TRUE(stubCell.has_value());
  EXPECT_EQ(p.attr(*stubCell, "d.winstate"), "live");
}

TEST(RouterLensTest, OpenJsonFileMountsConfigUnderItsCell) {
  RouterFixture fix;
  ProjectedRouter p{fix.dir};
  const auto fileCell =
      p.childNamed(p.session->roots().front(), "settings.json");
  ASSERT_TRUE(fileCell.has_value());
  const auto fileKey = p.session->keyFor(*fileCell);
  ASSERT_TRUE(fileKey.has_value());

  const auto opened = p.lens->open(*fileKey);
  ASSERT_TRUE(opened.has_value()) << opened.error();
  (void)p.session->refresh();

  const auto cfgRoot = p.session->cellFor(*opened);
  ASSERT_TRUE(cfgRoot.has_value());
  EXPECT_EQ(p.attr(*cfgRoot, "d.role"), "cfg.file");
  const auto head = p.child(*fileCell);
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(*head, *cfgRoot);
  // Members hang under the delegate root.
  const auto members = p.children(*cfgRoot);
  ASSERT_EQ(members.size(), 2u);
  EXPECT_EQ(p.attr(members[0], "d.key"), "name");
}

TEST(RouterLensTest, TwoMountsCoexistInOneProjection) {
  RouterFixture fix;
  ProjectedRouter p{fix.dir};
  const auto root     = p.session->roots().front();
  const auto repoCell = p.childNamed(root, "repo");
  const auto logCell  = p.childNamed(root, "app.log");
  ASSERT_TRUE(repoCell.has_value() && logCell.has_value());
  ASSERT_TRUE(p.lens->open(*p.session->keyFor(*repoCell)).has_value());
  ASSERT_TRUE(p.lens->open(*p.session->keyFor(*logCell)).has_value());
  (void)p.session->refresh();

  EXPECT_EQ(p.attr(*p.child(*repoCell), "d.role"), "git.repo");
  EXPECT_EQ(p.attr(*p.child(*logCell), "d.role"), "log.file");
  EXPECT_EQ(p.lens->mountCount(), 2u);
  // Both mount roots are under their cells, not in roots().
  EXPECT_EQ(p.session->roots().size(), 1u);
}

TEST(RouterLensTest, MountBoundaryFindsTheHopOut) {
  RouterFixture fix;
  ProjectedRouter p{fix.dir};
  const auto root     = p.session->roots().front();
  const auto repoCell = p.childNamed(root, "repo");
  const auto txtCell  = p.childNamed(root, "notes.txt");
  ASSERT_TRUE(repoCell.has_value() && txtCell.has_value());
  const auto repoKey = *p.session->keyFor(*repoCell);
  ASSERT_TRUE(p.lens->open(repoKey).has_value());
  (void)p.session->refresh();

  // The mount cell is its own boundary.
  EXPECT_EQ(p.lens->mountBoundary(repoKey), repoKey);
  // A delegate-owned key resolves to the mount it hangs under.
  const auto commitKey = "git:" + fix.repo.string() + ":c:x";
  EXPECT_EQ(p.lens->mountBoundary(commitKey), repoKey);
  // An fs cell inside the mounted tree resolves by path ancestry.
  const auto dotGit = p.childNamed(*repoCell, ".git");
  ASSERT_TRUE(dotGit.has_value());
  const auto dotGitKey = p.session->keyFor(*dotGit);
  ASSERT_TRUE(dotGitKey.has_value());
  EXPECT_EQ(p.lens->mountBoundary(*dotGitKey), repoKey);
  // An unmounted fs cell has no boundary.
  EXPECT_FALSE(p.lens->mountBoundary(*p.session->keyFor(*txtCell)).has_value());
}

TEST(RouterLensTest, OpenRefusesWhatNoDelegateClassifies) {
  RouterFixture fix;
  ProjectedRouter p{fix.dir};
  const auto root = p.session->roots().front();
  const auto txt  = p.childNamed(root, "notes.txt");
  ASSERT_TRUE(txt.has_value());
  EXPECT_FALSE(p.lens->open(*p.session->keyFor(*txt)).has_value());
  // A plain directory that is not a worktree refuses too.
  const auto plainDir = fix.dir / "plain";
  fs::create_directories(plainDir);
  // Re-walk: the fixture built the lens before plain/ existed, so open
  // it through a fresh projection instead.
  ProjectedRouter q{fix.dir};
  const auto pd = q.childNamed(q.session->roots().front(), "plain");
  ASSERT_TRUE(pd.has_value());
  EXPECT_FALSE(q.lens->open(*q.session->keyFor(*pd)).has_value());
  // A delegate key is not an fs cell.
  const auto repoCell = p.childNamed(root, "repo");
  ASSERT_TRUE(p.lens->open(*p.session->keyFor(*repoCell)).has_value());
  (void)p.session->refresh();
  EXPECT_FALSE(p.lens->open("git:" + fix.repo.string()).has_value());
  // Unmounting a non-mount refuses.
  EXPECT_FALSE(p.lens->unmount(*p.session->keyFor(*txt)).has_value());
}

TEST(RouterLensTest, OpenLensUriRoutesDirectoriesToTheRouter) {
  RouterFixture fix;
  auto byDirScheme = zigzag::openLensUri("dir:" + fix.dir.string());
  ASSERT_TRUE(byDirScheme.has_value());
  EXPECT_NE(dynamic_cast<RouterLens *>(byDirScheme->get()), nullptr);

  auto bareDir = zigzag::openLensUri(fix.dir.string());
  ASSERT_TRUE(bareDir.has_value());
  EXPECT_NE(dynamic_cast<RouterLens *>(bareDir->get()), nullptr);

  auto bareFile = zigzag::openLensUri(fix.txtFile.string());
  ASSERT_TRUE(bareFile.has_value());
  EXPECT_NE(dynamic_cast<FilesystemLens *>(bareFile->get()), nullptr);
  EXPECT_EQ(dynamic_cast<RouterLens *>(bareFile->get()), nullptr);
}

} // namespace
