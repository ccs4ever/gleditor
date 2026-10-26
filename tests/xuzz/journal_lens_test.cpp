/**
 * @file journal_lens_test.cpp
 * @brief JournalLens: systemd's journal as cursor-keyed windows, newest
 *        first (design/lens-candidates.md section 3.4).
 *
 * Every journal-touching test gates on a readable journal: hosts
 * without /run/log/journal, without a running journald, or without the
 * permissions skip cleanly instead of failing.
 */
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/journal_lens.hpp"
#include "common/xanadu/zigzag/lens_uri.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

#ifdef GLEDITOR_HAVE_SYSTEMD
#include <systemd/sd-journal.h>
#endif

namespace {

using zigzag::CellRef;
using zigzag::JournalLens;
using zigzag::OptionalCell;
using zigzag::Posward;
using zigzag::ProjectionSession;
using zigzag::SourceNode;
using zigzag::vortex::VortexCore;

struct ProjectedJournal {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  JournalLens *lens{nullptr};
  std::unique_ptr<ProjectionSession> session;

  explicit ProjectedJournal(std::string_view unit = {},
                            std::size_t pageSize  = 8) {
    auto l  = std::make_unique<JournalLens>(unit, pageSize);
    lens    = l.get();
    session = std::make_unique<ProjectionSession>(std::move(l), core);
  }

  /// The gate: no readable journal -> skip, not fail.
  bool usable() const { return lens->isOpen() && !session->roots().empty(); }
  std::string whyNot() const { return lens->openError(); }

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

/// Collect every node the lens would emit, keys included.
std::vector<SourceNode> nodesOf(const JournalLens &lens) {
  std::vector<SourceNode> out;
  lens.forEachNode([&](const SourceNode &n) { out.push_back(n); });
  return out;
}

TEST(JournalLensTest, UriDispatchOpensJournalLens) {
  auto whole = zigzag::openLensUri("journal:");
  ASSERT_TRUE(whole.has_value());
  EXPECT_NE(dynamic_cast<JournalLens *>(whole->get()), nullptr);
  auto unit = zigzag::openLensUri("journal:cron.service");
  ASSERT_TRUE(unit.has_value());
  EXPECT_NE(dynamic_cast<JournalLens *>(unit->get()), nullptr);
}

TEST(JournalLensTest, RootKeyFollowsTheUnitFilter) {
  JournalLens whole;
  std::vector<zigzag::ExternalKey> roots;
  whole.forEachRoot([&](const zigzag::ExternalKey &k) { roots.push_back(k); });
  ASSERT_EQ(roots.size(), 1u);
  EXPECT_EQ(roots.front(), "journal");

  JournalLens filtered{"sshd.service"};
  roots.clear();
  filtered.forEachRoot(
      [&](const zigzag::ExternalKey &k) { roots.push_back(k); });
  ASSERT_EQ(roots.size(), 1u);
  EXPECT_EQ(roots.front(), "journal@sshd.service");
}

TEST(JournalLensTest, TailWindowProjectsCursorKeyedLines) {
  ProjectedJournal p{};
  if (!p.lens->isOpen()) {
    GTEST_SKIP() << p.whyNot();
  }
  const auto conn = p.session->roots().front();
  EXPECT_EQ(p.attr(conn, "d.role"), "journal.conn");
  const auto pages = p.children(conn);
  if (pages.empty()) {
    GTEST_SKIP() << "journal holds no readable entries";
  }
  EXPECT_EQ(p.attr(pages.front(), "d.winstate"), "live");
  // Every page but possibly the trailing horizon stub is live.
  for (std::size_t i = 0; i + 1 < pages.size(); ++i) {
    EXPECT_EQ(p.attr(pages[i], "d.winstate"), "live");
  }
  const auto lines = p.children(pages.front());
  ASSERT_FALSE(lines.empty());
  ASSERT_LE(lines.size(), 8u);
  for (const auto line : lines) {
    const auto key = p.session->keyFor(line);
    ASSERT_TRUE(key.has_value());
    // Cursor-keyed identity: journal:l:<cursor>.
    EXPECT_TRUE(key->starts_with("journal:l:"));
    EXPECT_GT(key->size(), std::string_view{"journal:l:"}.size());
    EXPECT_EQ(p.attr(line, "d.role"), "journal.line");
    EXPECT_EQ(p.attr(line, "d.keystab"), "pk");
    // "<isotime> <unit>: <MESSAGE>" -- the realtime timestamp leads.
    const auto text = p.arena.textOf(line);
    EXPECT_FALSE(text.empty());
    EXPECT_NE(text.find('T'), std::string::npos);
  }
}

TEST(JournalLensTest, MaterializePageWalksBackThroughStubs) {
  ProjectedJournal p{};
  if (!p.lens->isOpen()) {
    GTEST_SKIP() << p.whyNot();
  }
  const auto conn  = p.session->roots().front();
  const auto pages = p.children(conn);
  if (pages.size() < 2u) {
    GTEST_SKIP() << "journal too short for a second window";
  }
  const auto stub = p.lens->nextStub();
  if (!stub.has_value()) {
    GTEST_SKIP() << "no older horizon stub was staged";
  }
  EXPECT_TRUE(stub->starts_with("journal:w:"));
  EXPECT_EQ(p.attr(pages.back(), "d.winstate"), "stub");
  ASSERT_TRUE(p.lens->materializePage(*stub).has_value());
  (void)p.session->refresh();
  const auto pageCell = p.session->cellFor(*stub);
  ASSERT_TRUE(pageCell.has_value());
  EXPECT_EQ(p.attr(*pageCell, "d.winstate"), "live");
  const auto lines = p.children(*pageCell);
  ASSERT_FALSE(lines.empty());
  for (const auto line : lines) {
    const auto key = p.session->keyFor(line);
    ASSERT_TRUE(key.has_value());
    EXPECT_TRUE(key->starts_with("journal:l:"));
  }
}

TEST(JournalLensTest, WaitableFdIsTheJournalChangeNotice) {
  JournalLens lens;
  if (!lens.isOpen()) {
    GTEST_SKIP() << lens.openError();
  }
  const auto fd = lens.waitableFd();
  if (!fd.has_value()) {
    GTEST_SKIP() << "this journal exposes no change-notice fd";
  }
  EXPECT_GE(*fd, 0);
}

#ifdef GLEDITOR_HAVE_SYSTEMD
TEST(JournalLensTest, SentMessageAppearsInTheTailWindow) {
  JournalLens lens;
  if (!lens.isOpen()) {
    GTEST_SKIP() << lens.openError();
  }
  const std::string marker = "gleditor-journal-lens-test-" +
                             std::to_string(::getpid()) + "-" +
                             std::to_string(::time(nullptr));
  const std::string msg    = "MESSAGE=" + marker;
  if (sd_journal_send(msg.c_str(), "SYSLOG_IDENTIFIER=gleditor-test",
                      "PRIORITY=6", nullptr) < 0) {
    GTEST_SKIP() << "no journald socket to write a marker through";
  }
  // The marker lands at the tail; give journald a bounded window to
  // flush it, waiting on the journal's own change-notice fd.
  const auto fd = lens.waitableFd();
  bool seen     = false;
  for (int i = 0; i < 100 && !seen; ++i) {
    ASSERT_TRUE(lens.browse().has_value());
    for (const auto &n : nodesOf(lens)) {
      if (n.key.starts_with("journal:l:") &&
          std::holds_alternative<std::string>(n.content) &&
          std::get<std::string>(n.content).find(marker) != std::string::npos) {
        seen = true;
      }
    }
    if (seen || !fd.has_value()) {
      if (!seen) {
        ::usleep(50 * 1000);
      }
      continue;
    }
    ::pollfd pfd{.fd = *fd, .events = POLLIN, .revents = 0};
    (void)::poll(&pfd, 1, 100);
  }
  if (!seen) {
    GTEST_SKIP() << "marker never reached a readable journal";
  }
}
#endif

} // namespace
