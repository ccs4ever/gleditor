/**
 * @file config_lens_test.cpp
 * @brief ConfigLens: a JSON config document as a bounded enumeration
 *        projection (design/lens-candidates.md section 3.2).
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
#include "common/xanadu/zigzag/config_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace {

namespace fs = std::filesystem;
using zigzag::CellRef;
using zigzag::ConfigLens;
using zigzag::noCell;
using zigzag::OptionalCell;
using zigzag::Posward;
using zigzag::ProjectionSession;
using zigzag::vortex::VortexCore;

struct ProjectedConfig {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  std::unique_ptr<ProjectionSession> session;

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
  /// The member cell named @p key under @p parent (objects only).
  OptionalCell member(CellRef parent, std::string_view key) {
    for (const auto c : children(parent)) {
      if (attr(c, "d.key") == key) {
        return c;
      }
    }
    return std::nullopt;
  }
};

const char *kDoc = R"json({
  "render": {"backend": "vulkan", "vsync": true},
  "ui":     {"scale": 1.5, "fontSize": 16},
  "recent": ["/tmp/a.db", "/tmp/b.db"],
  "notes":  null
})json";

std::unique_ptr<ProjectedConfig> project(std::string_view doc) {
  auto lens = ConfigLens::fromText("/etc/demo.json", doc);
  EXPECT_TRUE(lens.has_value()) << lens.error();
  auto p     = std::make_unique<ProjectedConfig>();
  p->session = std::make_unique<ProjectionSession>(std::move(*lens), p->core);
  return p;
}

TEST(ConfigLensTest, ProjectsATypedTree) {
  const auto p = project(kDoc);
  ASSERT_TRUE(p != nullptr);
  const auto file = p->session->roots().front();
  EXPECT_EQ(p->attr(file, "d.role"), "cfg.file");
  EXPECT_EQ(p->attr(file, "d.path"), "/etc/demo.json");

  const auto render = p->member(file, "render");
  ASSERT_TRUE(render.has_value());
  EXPECT_EQ(p->attr(*render, "d.role"), "cfg.obj");
  EXPECT_EQ(p->attr(*render, "d.type"), "object");
  const auto backend = p->member(*render, "backend");
  ASSERT_TRUE(backend.has_value());
  EXPECT_EQ(p->attr(*backend, "d.role"), "cfg.val");
  EXPECT_EQ(p->attr(*backend, "d.type"), "string");
  EXPECT_EQ(p->arena.textOf(*backend), "vulkan");

  // Scalars carry their JSON type into the cell itself.
  const auto scale = p->member(*p->member(file, "ui"), "scale");
  ASSERT_TRUE(scale.has_value());
  const auto bits = p->arena.asDouble(*scale);
  ASSERT_TRUE(bits.has_value());
  EXPECT_DOUBLE_EQ(*bits, 1.5);
  const auto fontSize = p->member(*p->member(file, "ui"), "fontSize");
  EXPECT_EQ(p->arena.asInt64(*fontSize).value_or(-1), 16);
  const auto vsync = p->member(*render, "vsync");
  EXPECT_EQ(p->attr(*vsync, "d.type"), "bool");
}

TEST(ConfigLensTest, ArraysAreIndexedMembers) {
  const auto p      = project(kDoc);
  const auto recent = p->member(p->session->roots().front(), "recent");
  ASSERT_TRUE(recent.has_value());
  EXPECT_EQ(p->attr(*recent, "d.role"), "cfg.arr");
  const auto items = p->children(*recent);
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(p->attr(items[0], "d.index"), "0");
  EXPECT_EQ(p->attr(items[1], "d.index"), "1");
  EXPECT_EQ(p->arena.textOf(items[0]), "/tmp/a.db");
  EXPECT_EQ(p->arena.textOf(items[1]), "/tmp/b.db");
}

TEST(ConfigLensTest, PathKeysAreStableAcrossRefresh) {
  // The on-disk path exercises the real re-read on refresh().
  const auto path = fs::temp_directory_path() /
                    ("cfg-lens-" + std::to_string(::getpid()) + ".json");
  {
    std::ofstream f(path);
    f << R"({"a": {"b": 1}, "c": [true]})";
  }
  ProjectedConfig p;
  p.session = std::make_unique<ProjectionSession>(
      std::make_unique<ConfigLens>(path.string()), p.core);
  const auto a     = p.member(p.session->roots().front(), "a");
  const auto bCell = p.member(*a, "b");
  ASSERT_TRUE(bCell.has_value());
  const auto bKey = p.session->keyFor(*bCell);
  ASSERT_TRUE(bKey.has_value());
  EXPECT_EQ(*bKey, "cfg:" + path.string() + ":/a/b");

  // An edit that changes the value but not the path restates the same
  // cell's content (path-key identity is what makes applyContentChange's
  // contract satisfiable at all).
  {
    std::ofstream f(path);
    f << R"({"a": {"b": 2}, "c": [true]})";
  }
  (void)p.session->refresh();
  const auto b2 = p.session->cellFor(*bKey);
  ASSERT_TRUE(b2.has_value());
  EXPECT_EQ(*b2, *bCell);
  EXPECT_EQ(p.arena.asInt64(*b2).value_or(-1), 2);

  // A removed path tombstones rather than resurrecting: union identity
  // keeps the cell, staleAttributes marks it.
  {
    std::ofstream f(path);
    f << R"({"a": {}})";
  }
  (void)p.session->refresh();
  const auto tombstoned = p.session->cellFor(*bKey);
  ASSERT_TRUE(tombstoned.has_value());
  EXPECT_EQ(p.attr(*tombstoned, "d.stale"), "1");
  const auto cKey  = "cfg:" + path.string() + ":/c";
  const auto cCell = p.session->cellFor(cKey);
  ASSERT_TRUE(cCell.has_value());
  EXPECT_EQ(p.attr(*cCell, "d.stale"), "1");

  fs::remove(path);
}

TEST(ConfigLensTest, CommitWritesAScalarBackToTheFile) {
  const auto path = fs::temp_directory_path() /
                    ("cfg-write-" + std::to_string(::getpid()) + ".json");
  {
    std::ofstream f{path};
    f << kDoc;
  }
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  auto session = std::make_unique<ProjectionSession>(
      std::make_unique<ConfigLens>(path.string()), core);
  const auto widthKey = "cfg:" + path.string() + ":/ui/fontSize";
  ASSERT_TRUE(session->cellFor(widthKey).has_value());
  // The write path: editContent records the diff, commit() crosses it
  // back through applyContentChange, and refresh() restates.
  ASSERT_TRUE(session->editContent(widthKey, "24").has_value());
  ASSERT_TRUE(session->commit().has_value());
  std::ifstream after{path};
  std::string disk{std::istreambuf_iterator<char>{after}, {}};
  EXPECT_NE(disk.find("\"fontSize\": 24"), std::string::npos);
  // And the projection restated the source's truth (JSON typed the
  // edit a number -- d.type reflects it).
  const auto w = session->cellFor(widthKey);
  ASSERT_TRUE(w.has_value());
  const auto t = core.arena().linked(*w, core.findOrMintDimension("d.type"),
                                     zigzag::Posward);
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(core.arena().textOf(*t), "int");
  fs::remove(path);
}

TEST(ConfigLensTest, CommitRefusesContainerAndBadScalars) {
  const auto path = fs::temp_directory_path() /
                    ("cfg-wrefuse-" + std::to_string(::getpid()) + ".json");
  {
    std::ofstream f{path};
    f << kDoc;
  }
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  auto session = std::make_unique<ProjectionSession>(
      std::make_unique<ConfigLens>(path.string()), core);
  const auto objKey   = "cfg:" + path.string() + ":/render";
  const auto widthKey = "cfg:" + path.string() + ":/ui/fontSize";
  // A container cell's content is a rendered label -- not writable;
  // canApply refuses it at editContent, before anything dirties.
  EXPECT_FALSE(session->editContent(objKey, "{}").has_value());
  EXPECT_EQ(session->dirtyCount(), 0u);
  // A non-integer on an int field is a writable cell with an
  // unwritable value -- the refusal waits for commit.
  ASSERT_TRUE(session->editContent(widthKey, "abc").has_value());
  EXPECT_FALSE(session->commit().has_value());
  fs::remove(path);
}

TEST(ConfigLensTest, ParseRejectsMalformedJson) {
  EXPECT_FALSE(ConfigLens::fromText("x", "{").has_value());
  EXPECT_FALSE(ConfigLens::fromText("x", "[1,]").has_value());
  EXPECT_FALSE(ConfigLens::fromText("x", "{\"a\":}").has_value());
  EXPECT_FALSE(ConfigLens::fromText("x", "01").has_value());
  EXPECT_FALSE(ConfigLens::fromText("x", "1e").has_value());
  EXPECT_TRUE(ConfigLens::fromText("x", "null").has_value());
  EXPECT_TRUE(ConfigLens::fromText("x", "\"s\\u0041\"").has_value());
}

} // namespace
