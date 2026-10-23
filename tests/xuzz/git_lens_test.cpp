/**
 * @file git_lens_test.cpp
 * @brief GitLens: a repository's refs and commit DAG as a windowed
 *        projection (design/lens-candidates.md section 3.1).
 */
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/git_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace {

namespace fs = std::filesystem;
using zigzag::CellRef;
using zigzag::GitLens;
using zigzag::OptionalCell;
using zigzag::Posward;
using zigzag::ProjectionSession;
using zigzag::vortex::VortexCore;

/// A scratch repo with @p n empty commits c1..cN on branch main.
struct GitFixture {
  fs::path repo;
  GitFixture(std::size_t n) {
    repo =
        fs::temp_directory_path() / ("git-lens-" + std::to_string(::getpid()));
    fs::remove_all(repo);
    fs::create_directories(repo);
    run("git init -q -b main " + repo.string());
    run("git -C " + repo.string() + " config user.email t@t && git -C " +
        repo.string() + " config user.name t");
    for (std::size_t i = 1; i <= n; ++i) {
      run("git -C " + repo.string() + " commit -q --allow-empty -m c" +
          std::to_string(i));
    }
  }
  ~GitFixture() { fs::remove_all(repo); }
  static void run(const std::string &cmd) {
    EXPECT_EQ(::system(cmd.c_str()), 0) << cmd;
  }
  /// HEAD's sha.
  std::string headSha() const {
    FILE *pipe =
        ::popen(("git -C " + repo.string() + " rev-parse HEAD").c_str(), "r");
    std::string out;
    char buf[128];
    if (fgets(buf, sizeof buf, pipe)) {
      out = buf;
    }
    ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
      out.pop_back();
    }
    return out;
  }
};

struct ProjectedGit {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  GitLens *lens{nullptr};
  std::unique_ptr<ProjectionSession> session;

  ProjectedGit(const std::string &repo, std::size_t pageSize = 3) {
    auto l  = std::make_unique<GitLens>(repo, pageSize);
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

TEST(GitLensTest, ProjectsRefsAndCommitPages) {
  GitFixture repo{5};
  ProjectedGit p{repo.repo.string(), 3};
  const auto root = p.session->roots().front();
  EXPECT_EQ(p.attr(root, "d.role"), "git.repo");
  const auto kids = p.children(root);
  // First children are the refs; then the log pages.
  ASSERT_FALSE(kids.empty());
  EXPECT_EQ(p.attr(kids[0], "d.role"), "git.ref");
  EXPECT_EQ(p.arena.textOf(kids[0]), "refs/heads/main");
  // A ref -> d.ref -> the tip commit cell.
  const auto tip = p.arena.linked(kids[0], p.dim("d.ref"), Posward);
  ASSERT_TRUE(tip.has_value());
  EXPECT_EQ(p.attr(*tip, "d.role"), "git.commit");
  EXPECT_EQ(p.attr(*tip, "d.sha"), repo.headSha());
  // Two pages for 5 commits at 3/page: the live page then a stub.
  CellRef page0 = kids[0];
  for (const auto k : kids) {
    if (p.attr(k, "d.role") == "git.page") {
      page0 = k;
      break;
    }
  }
  EXPECT_EQ(p.attr(page0, "d.winstate"), "live");
  const auto commits = p.children(page0);
  ASSERT_EQ(commits.size(), 3u);
  EXPECT_EQ(p.arena.textOf(commits[0]), "c5");
  EXPECT_EQ(p.attr(commits[0], "d.keystab"), "pk");
  // The newest commit's parent edge reaches c4.
  const auto parent = p.arena.linked(commits[0], p.dim("d.parent"), Posward);
  ASSERT_TRUE(parent.has_value());
  EXPECT_EQ(p.arena.textOf(*parent), "c4");
}

TEST(GitLensTest, StubPageMaterializesOlderCommits) {
  GitFixture repo{5};
  ProjectedGit p{repo.repo.string(), 3};
  const auto stubKey = "git:" + repo.repo.string() + ":p:3";
  const auto stub    = p.session->cellFor(stubKey);
  ASSERT_TRUE(stub.has_value());
  EXPECT_EQ(p.attr(*stub, "d.winstate"), "stub");
  ASSERT_TRUE(p.lens->materializePage(stubKey).has_value());
  (void)p.session->refresh();
  const auto live = p.session->cellFor(stubKey);
  EXPECT_EQ(p.attr(*live, "d.winstate"), "live");
  const auto commits = p.children(*live);
  ASSERT_EQ(commits.size(), 2u);
  EXPECT_EQ(p.arena.textOf(commits[0]), "c2");
  EXPECT_EQ(p.arena.textOf(commits[1]), "c1");
}

} // namespace
