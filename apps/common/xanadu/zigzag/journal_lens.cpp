/**
 * @file journal_lens.cpp
 * @brief JournalLens: systemd's journal as cursor-keyed windows, newest
 *        first (design/lens-candidates.md section 3.4). One TU, two
 *        halves like pg_backend.cpp: GLEDITOR_HAVE_SYSTEMD compiles the
 *        real lens; without it a refusing stub keeps journal: URIs
 *        parsing.
 */

#include "common/xanadu/zigzag/journal_lens.hpp"

#include <algorithm>
#include <chrono>

namespace zigzag {

namespace {

std::string lineKey(const std::string &conn, const std::string &cursor) {
  return conn + ":l:" + cursor;
}

} // namespace

std::vector<std::pair<std::string, std::optional<GenericDim>>>
JournalLens::dimensions() const {
  return {{"d.contains", GenericDim::Containment},
          {"d.seq", GenericDim::Sequence},
          {"d.role", std::nullopt},
          {"d.unit", std::nullopt},
          {"d.keystab", std::nullopt},
          {"d.winstate", std::nullopt},
          {"d.stale", std::nullopt}};
}

std::optional<ExternalKey> JournalLens::nextStub() const {
  for (const auto &p : pages_) {
    if (p.stub) {
      return p.key;
    }
  }
  return std::nullopt;
}

void JournalLens::discardMaterializedAfter(const ExternalKey &boundaryKey) {
  bool boundary = false;
  for (auto &p : pages_) {
    if (p.key == boundaryKey) {
      boundary = true;
      continue;
    }
    if (boundary) {
      p.stub = true;
      p.lines.clear();
    }
  }
}

void JournalLens::forEachNode(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  // Self-check: refresh() without a preceding browse() still sees an
  // appended journal -- sd_journal_process is one cheap call.
  if (pages_.empty() || pollChanged()) {
    (void)const_cast<JournalLens *>(this)->browse();
  }
  emit({connKey_, std::string{connKey_},
        filter_.empty()
            ? std::vector<std::pair<std::string, std::string>>{{"d.role",
                                                                "journal.conn"}}
            : std::vector<std::pair<std::string, std::string>>{{"d.role",
                                                                "journal.conn"},
                                                               {"d.unit",
                                                                filter_}},
        0});
  for (const auto &p : pages_) {
    emit(
        {p.key,
         p.stub ? std::string{"..."} + p.headCursor
                : std::string{"p:"} + p.headCursor,
         {{"d.role", "journal.page"}, {"d.winstate", p.stub ? "stub" : "live"}},
         0});
    for (const auto &line : p.lines) {
      emit({lineKey(connKey_, line.cursor),
            line.text,
            {{"d.role", "journal.line"}, {"d.keystab", "pk"}},
            0});
    }
  }
}

void JournalLens::forEachEdge(
    gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  if (pages_.empty()) {
    return;
  }
  emit({connKey_, pages_.front().key, "d.contains", Posward});
  for (std::size_t i = 1; i < pages_.size(); ++i) {
    emit({pages_[i - 1].key, pages_[i].key, "d.seq", Posward});
  }
  for (const auto &p : pages_) {
    if (p.lines.empty()) {
      continue;
    }
    emit({p.key, lineKey(connKey_, p.lines.front().cursor), "d.contains",
          Posward});
    for (std::size_t i = 1; i < p.lines.size(); ++i) {
      emit({lineKey(connKey_, p.lines[i - 1].cursor),
            lineKey(connKey_, p.lines[i].cursor), "d.seq", Posward});
    }
  }
}

bool JournalLens::forEachRoot(
    gleditor::cpp26::function_ref<void(const ExternalKey &)> emit) const {
  emit(connKey_);
  return true;
}

std::vector<std::pair<std::string, std::string>>
JournalLens::staleAttributes(const ExternalKey & /*key*/) const {
  return {{"d.stale", "1"}};
}

} // namespace zigzag

#ifdef GLEDITOR_HAVE_SYSTEMD

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unordered_set>

#include <systemd/sd-journal.h>

namespace zigzag {

namespace {

std::string pageKey(const std::string &conn, const std::string &cursor) {
  return conn + ":w:" + cursor;
}

/// "<isotime> <unit>: <MESSAGE>" -- seconds-precision UTC, matching the
/// spec's example. One entry is one line: embedded newlines become
/// spaces so MESSAGE can't fake extra lines.
constexpr std::uint64_t kMicrosecondsPerSecond{1'000'000};

std::optional<std::string> iso8601(const std::uint64_t usec) {
  const std::time_t t = static_cast<std::time_t>(usec / kMicrosecondsPerSecond);
  std::tm tm          = {};
  const std::tm *tmp  = ::gmtime_r(&t, &tm);
  char buf[32]        = {};
  if (tmp == nullptr ||
      std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", tmp) == 0) {
    return std::nullopt;
  }
  return buf;
}

} // namespace

JournalLens::JournalLens(const std::string_view unitFilter,
                         const std::size_t pageSize)
    : filter_{unitFilter}, pageSize_(std::max(pageSize, kMinimumLensPageSize)),
      connKey_(filter_.empty() ? "journal" : "journal@" + filter_) {
  int r = sd_journal_open(&j_, SD_JOURNAL_LOCAL_ONLY);
  if (r == -EPERM || r == -EACCES) {
    // No system-journal read rights: the calling user's own journal is
    // the next best source.
    r = sd_journal_open(&j_, SD_JOURNAL_CURRENT_USER);
  }
  if (r < 0) {
    openErr_ = "journal open failed: " + std::string{strerror(-r)};
    return;
  }
  if (!filter_.empty()) {
    const std::string m = "_SYSTEMD_UNIT=" + filter_;
    (void)sd_journal_add_match(j_, m.c_str(), 0);
  }
}

JournalLens::~JournalLens() {
  if (j_ != nullptr) {
    sd_journal_close(j_);
  }
}

std::optional<std::string> JournalLens::field(const char *name) const {
  const void *data = nullptr;
  std::size_t size = 0;
  if (sd_journal_get_data(j_, name, &data, &size) < 0) {
    return std::nullopt;
  }
  // get_data returns the "NAME=value" field wholesale.
  const std::string_view v{static_cast<const char *>(data), size};
  const std::string_view prefix{name};
  if (!v.starts_with(prefix) || v.size() < prefix.size() + 1) {
    return std::nullopt;
  }
  return std::string{v.substr(prefix.size() + 1)};
}

std::expected<JournalLens::JournalLine, std::string>
JournalLens::currentLine() const {
  char *cur = nullptr;
  if (sd_journal_get_cursor(j_, &cur) < 0 || cur == nullptr) {
    return std::unexpected("entry has no cursor");
  }
  JournalLine line;
  line.cursor = cur;
  std::free(cur);

  std::uint64_t usec = 0;
  std::string text;
  if (sd_journal_get_realtime_usec(j_, &usec) >= 0) {
    if (const auto timestamp = iso8601(usec)) {
      text = *timestamp;
    }
  }
  auto unit = field("_SYSTEMD_UNIT");
  if (!unit || unit->empty()) {
    unit = field("SYSLOG_IDENTIFIER");
  }
  auto msg = field("MESSAGE");
  if (msg)
    for (auto &ch : *msg) {
      if (ch == '\n') {
        ch = ' ';
      }
    }
  if (unit && !unit->empty()) {
    if (!text.empty()) text += ' ';
    text += *unit;
    text += ':';
  }
  if (msg && !msg->empty()) {
    if (!text.empty()) text += ' ';
    text += *msg;
  }
  line.text = std::move(text);
  return line;
}

std::expected<std::vector<JournalLens::JournalLine>, std::string>
JournalLens::collectForward() {
  std::vector<JournalLine> out;
  out.reserve(pageSize_);
  while (out.size() < pageSize_) {
    auto line = currentLine();
    if (!line) {
      return std::unexpected(line.error());
    }
    out.push_back(std::move(*line));
    if (out.size() < pageSize_ && sd_journal_next(j_) <= 0) {
      break;
    }
  }
  return out;
}

std::optional<std::string>
JournalLens::olderHead(const std::string &headCursor) const {
  // seek_cursor positions *before* the entry; next() lands on it. The
  // next-older window's head is pageSize_ entries further back -- a
  // short skip means the pointer is on the journal's own head, which
  // still names a valid (partial) window.
  if (sd_journal_seek_cursor(j_, headCursor.c_str()) < 0 ||
      sd_journal_next(j_) <= 0 ||
      sd_journal_previous_skip(j_, pageSize_) <= 0) {
    return std::nullopt;
  }
  char *cur = nullptr;
  if (sd_journal_get_cursor(j_, &cur) < 0 || cur == nullptr) {
    return std::nullopt;
  }
  std::string out{cur};
  std::free(cur);
  return out;
}

void JournalLens::appendStubAfter(const std::string &headCursor) {
  const auto older = olderHead(headCursor);
  if (!older.has_value()) {
    return;
  }
  JournalPage stub;
  stub.key        = pageKey(connKey_, *older);
  stub.headCursor = *older;
  pages_.push_back(std::move(stub));
}

std::expected<void, std::string> JournalLens::browse() {
  if (j_ == nullptr) {
    return std::unexpected(openErr_);
  }
  if (sd_journal_seek_tail(j_) < 0) {
    return std::unexpected("journal seek_tail failed");
  }
  // The tail window's head is pageSize_ entries back from the end; a
  // short skip lands on the journal's own head (a partial tail).
  if (sd_journal_previous_skip(j_, pageSize_) <= 0) {
    pages_.clear();
    browsed_ = true;
    return {};
  }
  char *cur = nullptr;
  if (sd_journal_get_cursor(j_, &cur) < 0 || cur == nullptr) {
    return std::unexpected("journal tail has no cursor");
  }
  JournalPage tail;
  tail.stub       = false;
  tail.headCursor = cur;
  std::free(cur);
  tail.key   = pageKey(connKey_, tail.headCursor);
  auto lines = collectForward();
  if (!lines) {
    return std::unexpected(lines.error());
  }
  tail.lines = std::move(*lines);
  ++tail.epoch;

  // Carry over live interior pages: a cursor-keyed window covers the
  // same entries after any append. A carried page whose newest entry
  // landed inside the new tail overlaps it -- drop those (their lines
  // are re-emitted under the tail's page), keep the disjoint older ones.
  std::unordered_set<std::string> tailCursors;
  for (const auto &l : tail.lines) {
    tailCursors.insert(l.cursor);
  }
  std::vector<JournalPage> pages;
  pages.push_back(std::move(tail));
  for (auto &old : pages_) {
    if (old.stub || old.lines.empty()) {
      continue;
    }
    if (!tailCursors.contains(old.lines.back().cursor)) {
      pages.push_back(std::move(old));
    }
  }
  pages_ = std::move(pages);
  // One stub trails the materialized horizon.
  appendStubAfter(pages_.back().headCursor);
  browsed_ = true;
  return {};
}

std::expected<void, std::string>
JournalLens::materializePage(const ExternalKey &windowKey) {
  if (j_ == nullptr) {
    return std::unexpected(openErr_);
  }
  for (auto &p : pages_) {
    if (p.key != windowKey || !p.stub) {
      continue;
    }
    if (sd_journal_seek_cursor(j_, p.headCursor.c_str()) < 0 ||
        sd_journal_next(j_) <= 0) {
      return std::unexpected("window head is unreachable: " + p.headCursor);
    }
    auto lines = collectForward();
    if (!lines) {
      return std::unexpected(lines.error());
    }
    p.lines = std::move(*lines);
    p.stub  = false;
    ++p.epoch;
    // Extending the horizon: stub the next-older window if this page
    // was the last and the journal reaches back further.
    if (&p == &pages_.back()) {
      appendStubAfter(p.headCursor);
    }
    return {};
  }
  return std::unexpected("unknown or live page: " + windowKey);
}

bool JournalLens::pollChanged() const {
  if (j_ == nullptr || !browsed_) {
    return false; // never browsed: the first projection does the read
  }
  const int r = sd_journal_process(j_);
  // INVALIDATE (rotation/removal) is a move too: the funnel's refresh
  // must re-seek rather than trust a stale window set.
  return r == SD_JOURNAL_APPEND || r == SD_JOURNAL_INVALIDATE;
}

std::optional<int> JournalLens::waitableFd() const noexcept {
  if (j_ == nullptr) {
    return std::nullopt;
  }
  const int fd = sd_journal_get_fd(j_);
  return fd >= 0 ? std::optional<int>{fd} : std::nullopt;
}

} // namespace zigzag

#else // !GLEDITOR_HAVE_SYSTEMD -- the refusing stub (same shape as pg)

namespace zigzag {

JournalLens::JournalLens(const std::string_view unitFilter,
                         const std::size_t pageSize)
    : filter_{unitFilter}, pageSize_(std::max(pageSize, kMinimumLensPageSize)),
      connKey_(filter_.empty() ? "journal" : "journal@" + filter_),
      openErr_{"journal: this build has no libsystemd -- rebuild with "
               "GLEDITOR_JOURNAL_SD=1 (the Makefile probes pkg-config "
               "libsystemd)"} {}

JournalLens::~JournalLens() = default;

std::expected<JournalLens::JournalLine, std::string>
JournalLens::currentLine() const {
  return std::unexpected(openErr_);
}

std::expected<std::vector<JournalLens::JournalLine>, std::string>
JournalLens::collectForward() {
  return std::unexpected(openErr_);
}

std::optional<std::string>
JournalLens::olderHead(const std::string & /*headCursor*/) const {
  return std::nullopt;
}

std::optional<std::string> JournalLens::field(const char * /*name*/) const {
  return std::nullopt;
}

void JournalLens::appendStubAfter(const std::string & /*headCursor*/) {}

std::expected<void, std::string> JournalLens::browse() {
  return std::unexpected(openErr_);
}

std::expected<void, std::string>
JournalLens::materializePage(const ExternalKey & /*windowKey*/) {
  return std::unexpected(openErr_);
}

bool JournalLens::pollChanged() const { return false; }

std::optional<int> JournalLens::waitableFd() const noexcept {
  return std::nullopt;
}

} // namespace zigzag

#endif // GLEDITOR_HAVE_SYSTEMD
