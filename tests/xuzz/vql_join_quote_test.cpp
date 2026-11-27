// M7-VQL-d2 (P11, design/vortex-io-m7-vql-d2.md): `join` over rows that are
// another store's cells returns a quote of their spans with the separator
// between them, instead of the copied text slice d returned.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

namespace fs = std::filesystem;
using xanadu::PrimediaSpan;
using xanadu::vql::QueryExecutor;
using xanadu::vql::VQLEngine;
using zigzag::CellRef;

struct Source {
  std::shared_ptr<xanadu::UserPermascroll> perma;
  std::shared_ptr<xanadu::Store> store;
  zigzag::Manifold manifold;
  std::vector<CellRef> cells;
  xanadu::MicroversionId version;
};

auto freshPermascroll(const std::string &name)
    -> std::shared_ptr<xanadu::UserPermascroll> {
  const auto dir =
      fs::temp_directory_path() /
      ("vql_join_quote_" + name + "_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  xanadu::UserPermascroll::Config config;
  config.storageDir = dir / "permascroll";
  return std::make_shared<xanadu::UserPermascroll>(config);
}

/// A store of one text cell per entry, over @p perma.
auto makeSource(const std::shared_ptr<xanadu::UserPermascroll> &perma,
                const std::vector<std::string> &texts) -> Source {
  Source source{.perma    = perma,
                .store    = std::make_shared<xanadu::Store>(perma),
                .manifold = {},
                .cells    = {},
                .version  = {}};
  auto version = source.store->sliceGenesis(xanadu::MicroversionId{});
  for (const auto &text : texts) {
    version = source.store->makeCell(version, text);
    source.cells.push_back(source.store->cellRefOf(version).value());
  }
  source.version  = version;
  source.manifold = source.store->rebuildManifold(version);
  return source;
}

struct JoinHarness : ::testing::Test {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  VQLEngine engine{core};

  auto attach(Source &source, const std::string &label) -> std::uint32_t {
    return arena.attach(zigzag::Space{.manifold = &source.manifold,
                                      .store    = source.store.get(),
                                      .reader   = source.store.get(),
                                      .label    = label});
  }
  auto proxies(const std::uint32_t space, const Source &source,
               const std::vector<std::size_t> &which) {
    std::vector<CellRef> rows;
    for (const auto at : which)
      rows.push_back(arena.proxyFor(space, source.cells[at]));
    return rows;
  }
  auto join(const std::vector<CellRef> &rows, const std::string &separator)
      -> CellRef {
    engine.setVariable("rows", rows);
    QueryExecutor executor(engine, vm, {.maxCycles = 1'000'000});
    const auto result = executor.execute("join($rows, \"" + separator + "\")");
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    if (!result || result->size() != 1U) return zigzag::noCell;
    return result->front();
  }
};

TEST_F(JoinHarness, SeveralRowsOfALoadedStoreStayQuoted) {
  auto source =
      makeSource(freshPermascroll("rows"), {"alpha", "beta", "gamma"});
  const auto space  = attach(source, "s");
  const auto rows   = proxies(space, source, {0, 1, 2});
  const auto joined = join(rows, ", ");
  ASSERT_NE(joined, zigzag::noCell);
  EXPECT_EQ(arena.textOf(joined), "alpha, beta, gamma");
  const auto run = arena.quotedRun(joined);
  ASSERT_TRUE(run.has_value()) << "joined as copied text, not a quote";
  EXPECT_EQ(run->store, source.store.get());
  ASSERT_EQ(run->spans.size(), 5U) << "row, separator, row, separator, row";
  for (std::size_t i = 0; i < 3; ++i) {
    const auto original = source.manifold.contentOf(source.cells[i]);
    ASSERT_EQ(original.size(), 1U);
    EXPECT_EQ(run->spans[2 * i], original.front()) << "row " << i;
  }
  EXPECT_EQ(arena.scratchTextOf(run->spans[1]), ", ");
  EXPECT_EQ(arena.scratchTextOf(run->spans[3]), ", ");
}

TEST_F(JoinHarness,
       PromotingTheJoinKeepsEachRowTranscludedAndTypesOnlyTheSeparators) {
  auto source =
      makeSource(freshPermascroll("promote"), {"alpha", "beta", "gamma"});
  const auto space  = attach(source, "s");
  const auto joined = join(proxies(space, source, {0, 1, 2}), ", ");
  ASSERT_NE(joined, zigzag::noCell);
  const auto before = source.perma->size();
  const auto promoted =
      zigzag::promote(*source.store, source.version, arena, joined);
  ASSERT_TRUE(promoted.has_value());
  const auto folded  = source.store->rebuildManifold(promoted->version);
  const auto cell    = promoted->cells.front();
  const auto content = folded.contentOf(cell);
  ASSERT_EQ(content.size(), 5U);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(content[2 * i],
              source.manifold.contentOf(source.cells[i]).front())
        << "row " << i << " is the original span, not a copy";
  }
  EXPECT_EQ(folded.textOf(cell, *source.store), "alpha, beta, gamma");
  EXPECT_EQ(source.perma->size() - before, 4U)
      << "the permascroll grew by the two separators and nothing else";
}

TEST_F(JoinHarness, RowsOfTwoStoresOverOnePermascrollJoinAsOneQuote) {
  const auto perma = freshPermascroll("two");
  auto first       = makeSource(perma, {"left"});
  auto second      = makeSource(perma, {"right"});
  const auto a     = attach(first, "a");
  const auto b     = attach(second, "b");
  std::vector<CellRef> rows{arena.proxyFor(a, first.cells[0]),
                            arena.proxyFor(b, second.cells[0])};
  const auto joined = join(rows, "|");
  ASSERT_NE(joined, zigzag::noCell);
  EXPECT_EQ(arena.textOf(joined), "left|right");
  EXPECT_TRUE(arena.quotedRun(joined).has_value())
      << "local spans mean the same bytes under both stores";
}

TEST_F(JoinHarness, ARowOfALoadedStoreBesideALocalSpanAndANumber) {
  auto source      = makeSource(freshPermascroll("mixed"), {"alpha"});
  const auto space = attach(source, "s");
  // A cell some earlier step made over a span of the author's permascroll.
  const auto local =
      arena.makeCell(source.manifold.contentOf(source.cells[0]).front());
  const auto count = arena.makeScalarCell(std::int64_t{42});
  const auto joined =
      join({arena.proxyFor(space, source.cells[0]), local, count}, "-");
  ASSERT_NE(joined, zigzag::noCell);
  EXPECT_EQ(arena.textOf(joined), "alpha-alpha-42");
  const auto run = arena.quotedRun(joined);
  ASSERT_TRUE(run.has_value());
  ASSERT_EQ(run->spans.size(), 5U);
  const auto original = source.manifold.contentOf(source.cells[0]).front();
  EXPECT_EQ(run->spans[0], original) << "the foreign row is quoted, not typed";
  EXPECT_EQ(run->spans[2], original);
  EXPECT_EQ(arena.scratchTextOf(run->spans.back()), "42");
}

/// A store whose one cell spans an external (live) scroll of @p bytes.
auto makeExternalSource(const std::shared_ptr<xanadu::UserPermascroll> &perma,
                        const std::string &salt, const std::string &bytes)
    -> Source {
  xanadu::Scroll scroll;
  scroll.salt = salt;
  Source source{.perma    = perma,
                .store    = std::make_shared<xanadu::Store>(perma),
                .manifold = {},
                .cells    = {},
                .version  = {}};
  const auto id = source.store->addScroll(scroll);
  source.store->setExternalLiveBytes(xanadu::liveScrollBufferKey(scroll), 0,
                                     bytes);
  auto version = source.store->sliceGenesis(xanadu::MicroversionId{});
  version      = source.store->makeCell(
      version, PrimediaSpan{.scroll = id, .start = 0, .length = bytes.size()});
  source.cells.push_back(source.store->cellRefOf(version).value());
  source.version  = version;
  source.manifold = source.store->rebuildManifold(version);
  return source;
}

TEST_F(JoinHarness, RowsWhoseExternalScrollsDisagreeAreJoinedAsText) {
  const auto perma = freshPermascroll("external");
  auto first       = makeExternalSource(perma, "first-live-author", "ext-one");
  auto second      = makeExternalSource(perma, "second-live-author", "ext-two");
  const auto a     = attach(first, "a");
  const auto b     = attach(second, "b");
  std::vector<CellRef> rows{arena.proxyFor(a, first.cells[0]),
                            arena.proxyFor(b, second.cells[0])};
  const auto joined = join(rows, "+");
  ASSERT_NE(joined, zigzag::noCell);
  EXPECT_EQ(arena.textOf(joined), "ext-one+ext-two");
  EXPECT_FALSE(arena.quotedRun(joined).has_value())
      << "two stores' external scroll ids share no namespace: text, as slice d";
}

// A space attached with no store (only a reader) names no namespace for its
// external-scroll spans, so a row of it cannot be quoted beside another
// store's; its raw spans copied into a plain cell would be read as scroll ids
// of the arena's own store.
TEST_F(JoinHarness, AStoreLessForeignRowIsTypedNotCopiedAsRawSpans) {
  const auto perma = freshPermascroll("storeless");
  auto ext         = makeExternalSource(perma, "storeless-author", "ext-bytes");
  auto plain       = makeSource(perma, {"plain"});
  const auto bare  = arena.attach(zigzag::Space{.manifold = &ext.manifold,
                                                .store    = nullptr,
                                                .reader   = ext.store.get(),
                                                .label    = "bare"});
  const auto b     = attach(plain, "b");
  const auto joined = join(
      {arena.proxyFor(bare, ext.cells[0]), arena.proxyFor(b, plain.cells[0])},
      "|");
  ASSERT_NE(joined, zigzag::noCell);
  EXPECT_EQ(arena.textOf(joined), "ext-bytes|plain");
  for (const auto &span : arena.contentOf(joined)) {
    EXPECT_TRUE(span.isLocal() || xanadu::scratchScroll == span.scroll)
        << "a foreign scroll id survived into the arena's own namespace";
  }
}

TEST_F(JoinHarness,
       AMixedDisagreeingJoinTypesEveryForeignRowAndKeepsLocalOnes) {
  const auto perma = freshPermascroll("mixeddis");
  auto first       = makeExternalSource(perma, "mix-one", "ext-one");
  auto second      = makeExternalSource(perma, "mix-two", "ext-two");
  auto plain       = makeSource(perma, {"plain"});
  const auto bare  = arena.attach(zigzag::Space{.manifold = &second.manifold,
                                                .store    = nullptr,
                                                .reader   = second.store.get(),
                                                .label    = "bare"});
  const auto a     = attach(first, "a");
  const auto p     = attach(plain, "p");
  const auto local =
      arena.makeCell(plain.manifold.contentOf(plain.cells[0]).front());
  const auto joined = join({arena.proxyFor(a, first.cells[0]),
                            arena.proxyFor(bare, second.cells[0]),
                            arena.proxyFor(p, plain.cells[0]), local},
                           ",");
  ASSERT_NE(joined, zigzag::noCell);
  EXPECT_EQ(arena.textOf(joined), "ext-one,ext-two,plain,plain");
  EXPECT_FALSE(arena.quotedRun(joined).has_value());
  for (const auto &span : arena.contentOf(joined)) {
    EXPECT_TRUE(span.isLocal() || xanadu::scratchScroll == span.scroll);
  }
}

TEST_F(JoinHarness, OneRowNoRowsAndTheLoneForeignRowAreUnchanged) {
  auto source      = makeSource(freshPermascroll("lone"), {"solo", "other"});
  const auto space = attach(source, "s");
  const auto row   = arena.proxyFor(space, source.cells[0]);
  EXPECT_EQ(join({row}, ","), row) << "the row itself, identity kept";
  const auto empty = join({}, ",");
  ASSERT_NE(empty, zigzag::noCell);
  EXPECT_EQ(arena.textOf(empty), "");
  EXPECT_FALSE(arena.quotedRun(empty).has_value());
}

TEST_F(JoinHarness, AJoinOfJoinsStaysQuoted) {
  auto source      = makeSource(freshPermascroll("nested"), {"a", "b", "c"});
  const auto space = attach(source, "s");
  const auto ab    = join(proxies(space, source, {0, 1}), "+");
  const auto abc   = join({ab, arena.proxyFor(space, source.cells[2])}, "+");
  ASSERT_NE(abc, zigzag::noCell);
  EXPECT_EQ(arena.textOf(abc), "a+b+c");
  EXPECT_TRUE(arena.quotedRun(abc).has_value());
}

TEST_F(JoinHarness, AWideJoinLeavesOnlyItsSeparatorsInScratchAndIsBounded) {
  constexpr std::size_t count = 10'000;
  std::vector<std::string> texts;
  for (std::size_t i = 0; i < count; ++i)
    texts.push_back("r" + std::to_string(i));
  auto source      = makeSource(freshPermascroll("wide"), texts);
  const auto space = attach(source, "s");
  std::vector<std::size_t> all(count);
  for (std::size_t i = 0; i < count; ++i) all[i] = i;
  const auto joined = join(proxies(space, source, all), ",");
  ASSERT_NE(joined, zigzag::noCell);
  const auto run = arena.quotedRun(joined);
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run->spans.size(), 2 * count - 1);
  std::size_t scratch = 0;
  for (const auto &span : run->spans)
    if (xanadu::scratchScroll == span.scroll) scratch += span.length;
  EXPECT_EQ(scratch, count - 1) << "no row was copied into scratch";
}

} // namespace
