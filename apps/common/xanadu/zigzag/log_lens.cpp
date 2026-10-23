/**
 * @file log_lens.cpp
 * @brief LogLens: a log file's tail as ordinal-paged windows
 *        (design/lens-candidates.md section 3.4).
 */

#include "common/xanadu/zigzag/log_lens.hpp"

#include <fstream>
#include <sstream>

namespace zigzag {

namespace {

std::string pageKey(const std::string &conn, const std::size_t firstLine) {
  return conn + ":w:" + std::to_string(firstLine);
}

std::string lineKey(const std::string &conn, const std::size_t ordinal) {
  return conn + ":l:" + std::to_string(ordinal);
}

} // namespace

LogLens::LogLens(std::string path, const std::size_t pageSize)
    : path_(std::move(path)), pageSize_(pageSize == 0 ? 1 : pageSize),
      connKey_("log:" + path_) {}

std::vector<std::pair<std::string, std::optional<GenericDim>>>
LogLens::dimensions() const {
  return {{"d.contains", GenericDim::Containment},
          {"d.seq", GenericDim::Sequence},
          {"d.role", std::nullopt},
          {"d.index", std::nullopt},
          {"d.path", std::nullopt},
          {"d.keystab", std::nullopt},
          {"d.winstate", std::nullopt},
          {"d.stale", std::nullopt}};
}

std::expected<std::uint64_t, std::string> LogLens::lineCount() const {
  std::ifstream f(path_);
  if (!f) {
    return std::unexpected("unreadable: " + path_);
  }
  std::uint64_t n = 0;
  std::string line;
  while (std::getline(f, line)) {
    ++n;
  }
  return n;
}

std::expected<std::vector<std::string>, std::string>
LogLens::readLines(const std::size_t first, const std::size_t count) const {
  std::ifstream f(path_);
  if (!f) {
    return std::unexpected("unreadable: " + path_);
  }
  std::vector<std::string> out;
  out.reserve(count);
  std::string line;
  std::size_t n = 0;
  while (std::getline(f, line)) {
    if (n >= first && out.size() < count) {
      out.push_back(line);
    }
    if (++n >= first + count) {
      break;
    }
  }
  return out;
}

std::expected<void, std::string> LogLens::browse() {
  const auto total = lineCount();
  if (!total) {
    return std::unexpected(total.error());
  }
  totalLines_ = *total;
  pages_.clear();
  // Page 0 is the tail -- the "most recent N" window. Older pages are
  // stubs keyed on their head ordinal; the file's head is the boundary.
  std::size_t first = totalLines_ > pageSize_ ? totalLines_ - pageSize_ : 0;
  while (true) {
    LogPage p;
    p.key       = pageKey(connKey_, first);
    p.firstLine = first;
    p.count = totalLines_ - first > pageSize_ ? pageSize_ : totalLines_ - first;
    p.stub  = true;
    pages_.push_back(std::move(p));
    if (first == 0) {
      break;
    }
    first = first > pageSize_ ? first - pageSize_ : 0;
  }
  if (!pages_.empty()) {
    return materializePage(pages_.front().key);
  }
  return {};
}

std::expected<void, std::string>
LogLens::materializePage(const ExternalKey &windowKey) {
  for (auto &p : pages_) {
    if (p.key != windowKey || !p.stub) {
      continue;
    }
    auto lines = readLines(p.firstLine, p.count);
    if (!lines) {
      return std::unexpected(lines.error());
    }
    p.lines = std::move(*lines);
    p.stub  = false;
    ++p.epoch;
    return {};
  }
  return std::unexpected("unknown or live page: " + windowKey);
}

void LogLens::discardMaterializedAfter(const ExternalKey &boundaryKey) {
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

void LogLens::forEachNode(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  if (pages_.empty()) {
    (void)const_cast<LogLens *>(this)->browse();
  }
  emit({connKey_,
        std::string{path_},
        {{"d.role", "log.file"}, {"d.path", path_}},
        0});
  for (const auto &p : pages_) {
    emit({p.key,
          p.stub ? std::string{"..."} + std::to_string(p.firstLine)
                 : std::string{"p:"} + std::to_string(p.firstLine),
          {{"d.role", "log.page"}, {"d.winstate", p.stub ? "stub" : "live"}},
          0});
    std::size_t ord = p.firstLine;
    for (const auto &line : p.lines) {
      emit({lineKey(connKey_, ord),
            line,
            {{"d.role", "log.line"},
             {"d.index", std::to_string(ord)},
             {"d.keystab", "ord"}},
            0});
      ++ord;
    }
  }
}

void LogLens::forEachEdge(
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
    emit({p.key, lineKey(connKey_, p.firstLine), "d.contains", Posward});
    for (std::size_t i = 1; i < p.lines.size(); ++i) {
      emit({lineKey(connKey_, p.firstLine + i - 1),
            lineKey(connKey_, p.firstLine + i), "d.seq", Posward});
    }
  }
}

bool LogLens::forEachRoot(
    gleditor::cpp26::function_ref<void(const ExternalKey &)> emit) const {
  emit(connKey_);
  return true;
}

std::vector<std::pair<std::string, std::string>>
LogLens::staleAttributes(const ExternalKey & /*key*/) const {
  return {{"d.stale", "1"}};
}

} // namespace zigzag
