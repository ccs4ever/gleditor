/**
 * @file git_lens.cpp
 * @brief GitLens: a repository's refs and commit DAG as a windowed
 *        projection (design/lens-candidates.md section 3.1).
 */

#include "common/xanadu/zigzag/git_lens.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace zigzag {

namespace {

/// Single-quote a path for a shell command line.
std::string shellQuote(const std::string &s) {
  std::string out{"'"};
  for (const char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

std::string commitKey(const std::string &conn, const std::string &sha) {
  return conn + ":c:" + sha;
}

} // namespace

GitLens::GitLens(std::string repoPath, const std::size_t pageSize)
    : repo_(std::move(repoPath)), pageSize_(pageSize == 0 ? 1 : pageSize),
      connKey_("git:" + repo_), headWatch_(repo_ + "/.git/HEAD"),
      packedWatch_(repo_ + "/.git/packed-refs") {}

std::vector<std::pair<std::string, std::optional<GenericDim>>>
GitLens::dimensions() const {
  return {{"d.contains", GenericDim::Containment},
          {"d.seq", GenericDim::Sequence},
          {"d.ref", GenericDim::Reference},
          {"d.role", std::nullopt},
          {"d.path", std::nullopt},
          {"d.sha", std::nullopt},
          {"d.author", std::nullopt},
          {"d.date", std::nullopt},
          {"d.parent", std::nullopt},
          {"d.keystab", std::nullopt},
          {"d.winstate", std::nullopt},
          {"d.stale", std::nullopt}};
}

std::expected<std::string, std::string>
GitLens::run(const std::string_view args) const {
  const std::string cmd =
      "git -C " + shellQuote(repo_) + " " + std::string{args} + " 2>&1";
  std::array<char, 4096> buf{};
  std::string out;
  FILE *pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) {
    return std::unexpected("popen failed");
  }
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) !=
         nullptr) {
    out += buf.data();
  }
  const int rc = ::pclose(pipe);
  if (rc != 0) {
    return std::unexpected("git " + std::string{args} + ": " + out);
  }
  return out;
}

std::expected<std::vector<GitCommit>, std::string>
GitLens::logPage(const std::size_t skip, const std::size_t count) const {
  // %x1f fields: sha, parents, author, ISO date, subject. Trailing
  // newline splits; empty repo HEAD makes rev-list fail -- callers
  // tolerate an empty window.
  const auto out = run("rev-list HEAD --max-count=" + std::to_string(count) +
                       " --skip=" + std::to_string(skip) +
                       " --format=%H%x1f%P%x1f%an%x1f%aI%x1f%s");
  if (!out) {
    return std::unexpected(out.error());
  }
  std::vector<GitCommit> commits;
  std::istringstream in{*out};
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line.rfind("commit ", 0) == 0) {
      continue; // rev-list --format prepends 'commit <sha>'
    }
    std::vector<std::string> f;
    std::size_t at = 0;
    while (true) {
      const auto tab = line.find('\x1f', at);
      f.push_back(line.substr(at, tab == std::string::npos ? std::string::npos
                                                           : tab - at));
      if (tab == std::string::npos) {
        break;
      }
      at = tab + 1;
    }
    if (f.size() >= 5) {
      commits.push_back({f[0], f[1], f[2], f[3], f[4]});
    }
  }
  return commits;
}

namespace {

/// HEAD's target: "ref: refs/heads/main" -> <repo>/.git/refs/heads/main;
/// a detached sha keeps the literal file path (its own watch target).
std::string resolvedRefFile(const std::string &repo) {
  std::ifstream head(repo + "/.git/HEAD");
  std::string line;
  if (!std::getline(head, line)) {
    return {};
  }
  if (line.starts_with("ref: ")) {
    return repo + "/.git/" + line.substr(5);
  }
  return repo + "/.git/HEAD"; // detached: HEAD's sha is the stamp
}

std::optional<std::pair<std::int64_t, std::uint64_t>>
fileStamp(const std::string &path) {
  std::error_code ec;
  const auto wt = std::filesystem::last_write_time(path, ec);
  const auto sz = std::filesystem::file_size(path, ec);
  if (ec) {
    return std::nullopt;
  }
  return std::pair{wt.time_since_epoch().count(),
                   static_cast<std::uint64_t>(sz)};
}

} // namespace

bool GitLens::pollChanged() const {
  // The resolved branch ref is watched where it exists; a commit
  // rewrites the ref file, a checkout rewrites HEAD (and re-points
  // the ref watch). packed-refs covers `git pack-refs`.
  const std::string refFile = resolvedRefFile(repo_);
  if (!refFile.empty() && refFile != refPath_) {
    refPath_  = refFile;
    refWatch_ = std::make_unique<FileWatch>(refFile);
  }
  const auto refNow  = refFile.empty() ? std::nullopt : fileStamp(refFile);
  const auto headNow = fileStamp(repo_ + "/.git/HEAD");
  // First call seeds the stamps -- nothing moved yet.
  if (!seeded_) {
    seeded_    = true;
    stamp_     = refNow;
    headStamp_ = headNow;
    return false;
  }
  const bool moved   = (refNow != stamp_) || (headNow != headStamp_);
  stamp_             = refNow;
  headStamp_         = headNow;
  const bool watched = headWatch_.dirty() || packedWatch_.dirty() ||
                       (refWatch_ && refWatch_->dirty());
  return moved || watched;
}

std::optional<int> GitLens::waitableFd() const noexcept {
  if (refWatch_ && refWatch_->waitableFd().has_value()) {
    return refWatch_->waitableFd();
  }
  return headWatch_.waitableFd();
}

std::expected<void, std::string> GitLens::browse() {
  const auto count = run("rev-list HEAD --count");
  if (!count) {
    return std::unexpected(count.error());
  }
  total_ = std::stoull(*count);
  pages_.clear();
  commits_.clear();
  for (std::size_t skip = 0; skip < total_; skip += pageSize_) {
    LogPage p;
    p.key   = connKey_ + ":p:" + std::to_string(skip);
    p.skip  = skip;
    p.count = total_ - skip > pageSize_ ? pageSize_ : total_ - skip;
    pages_.push_back(std::move(p));
  }
  // Refs: HEAD's visible branch/tag tips, re-enumerated per browse.
  refs_.clear();
  if (const auto out = run("for-each-ref '--format=%(refname) %(objectname)'");
      out.has_value()) {
    std::istringstream in{*out};
    std::string line;
    while (std::getline(in, line)) {
      const auto sp = line.find(' ');
      if (sp != std::string::npos && sp + 1 < line.size()) {
        refs_.push_back({line.substr(0, sp), line.substr(sp + 1)});
      }
    }
  }
  if (!pages_.empty()) {
    return materializePage(pages_.front().key);
  }
  return {};
}

std::expected<void, std::string>
GitLens::materializePage(const ExternalKey &windowKey) {
  for (auto &p : pages_) {
    if (p.key != windowKey || !p.stub) {
      continue;
    }
    auto page = logPage(p.skip, p.count);
    if (!page) {
      return std::unexpected(page.error());
    }
    p.shas.clear();
    for (auto &c : *page) {
      p.shas.push_back(c.sha);
      commits_.push_back(std::move(c));
    }
    p.stub = false;
    return {};
  }
  return std::unexpected("unknown or live page: " + windowKey);
}

void GitLens::discardMaterializedAfter(const ExternalKey &boundaryKey) {
  bool boundary = false;
  for (auto &p : pages_) {
    if (p.key == boundaryKey) {
      boundary = true;
      continue;
    }
    if (boundary) {
      p.stub = true;
      p.shas.clear();
    }
  }
}

void GitLens::forEachNode(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  if (pages_.empty() && refs_.empty()) {
    (void)const_cast<GitLens *>(this)->browse();
  }
  emit({connKey_,
        std::string{repo_},
        {{"d.role", "git.repo"}, {"d.path", repo_}},
        0});
  for (const auto &r : refs_) {
    emit({connKey_ + ":ref:" + r.name, r.name, {{"d.role", "git.ref"}}, 0});
  }
  for (const auto &p : pages_) {
    emit({p.key,
          p.stub ? std::string{"..."} + std::to_string(p.skip)
                 : std::string{"p:"} + std::to_string(p.skip),
          {{"d.role", "git.page"}, {"d.winstate", p.stub ? "stub" : "live"}},
          0});
  }
  for (const auto &c : commits_) {
    emit({commitKey(connKey_, c.sha),
          c.subject,
          {{"d.role", "git.commit"},
           {"d.sha", c.sha},
           {"d.author", c.author},
           {"d.date", c.date},
           {"d.keystab", "pk"}},
          0});
  }
  // A ref's tip must exist as a cell even when its page is a stub --
  // emit a minimal commit node for any unmaterialized target so the
  // d.ref edge's far end mints this epoch.
  for (const auto &r : refs_) {
    const auto key = commitKey(connKey_, r.target);
    bool minted    = false;
    for (const auto &c : commits_) {
      if (commitKey(connKey_, c.sha) == key) {
        minted = true;
        break;
      }
    }
    if (!minted) {
      emit(
          {key,
           r.target,
           {{"d.role", "git.commit"}, {"d.sha", r.target}, {"d.keystab", "pk"}},
           0});
    }
  }
}

void GitLens::forEachEdge(
    gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  // repo -> refs, then repo -> log pages.
  for (std::size_t i = 0; i < refs_.size(); ++i) {
    const auto refKey = connKey_ + ":ref:" + refs_[i].name;
    if (i == 0) {
      emit({connKey_, refKey, "d.contains", Posward});
    } else {
      emit({connKey_ + ":ref:" + refs_[i - 1].name, refKey, "d.seq", Posward});
    }
    // ref -> its target commit (re-asserted each epoch: tips move,
    // union identity is exactly what makes that safe).
    emit({refKey, commitKey(connKey_, refs_[i].target), "d.ref", Posward});
  }
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    if (i == 0 && refs_.empty()) {
      emit({connKey_, pages_[0].key, "d.contains", Posward});
    } else if (i == 0) {
      emit({connKey_ + ":ref:" + refs_.back().name, pages_[0].key, "d.seq",
            Posward});
    } else {
      emit({pages_[i - 1].key, pages_[i].key, "d.seq", Posward});
    }
    for (std::size_t j = 0; j < pages_[i].shas.size(); ++j) {
      const auto ck = commitKey(connKey_, pages_[i].shas[j]);
      if (j == 0) {
        emit({pages_[i].key, ck, "d.contains", Posward});
      } else {
        emit(
            {commitKey(connKey_, pages_[i].shas[j - 1]), ck, "d.seq", Posward});
      }
    }
  }
  // The DAG itself: each materialized commit -> its materialized
  // parents (an edge's ends must both be minted this epoch).
  std::unordered_set<std::string> materialized;
  for (const auto &c : commits_) {
    materialized.insert(c.sha);
  }
  for (const auto &c : commits_) {
    if (c.parents.empty()) {
      continue;
    }
    std::istringstream ps{c.parents};
    std::string parent;
    while (ps >> parent) {
      if (materialized.contains(parent)) {
        emit({commitKey(connKey_, c.sha), commitKey(connKey_, parent),
              "d.parent", Posward});
      }
    }
  }
}

bool GitLens::forEachRoot(
    gleditor::cpp26::function_ref<void(const ExternalKey &)> emit) const {
  emit(connKey_);
  return true;
}

std::vector<std::pair<std::string, std::string>>
GitLens::staleAttributes(const ExternalKey & /*key*/) const {
  return {{"d.stale", "1"}};
}

} // namespace zigzag
