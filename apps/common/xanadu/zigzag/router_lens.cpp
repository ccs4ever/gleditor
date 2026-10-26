/**
 * @file router_lens.cpp
 * @brief RouterLens: per-cell delegate mounts over a filesystem
 *        projection -- cross-lens composition.
 */

#include "common/xanadu/zigzag/router_lens.hpp"

#include <algorithm>
#include <unordered_set>

#include "common/xanadu/zigzag/config_lens.hpp"
#include "common/xanadu/zigzag/git_lens.hpp"
#include "common/xanadu/zigzag/journal_lens.hpp"
#include "common/xanadu/zigzag/log_lens.hpp"
#include "common/xanadu/zigzag/sql_lens.hpp"

namespace zigzag {

namespace {

/// Is @p ancestor a strict path ancestor of @p p? Component-wise prefix
/// comparison on the lexical normal forms.
bool pathContains(const std::filesystem::path &ancestor,
                  const std::filesystem::path &p) {
  const auto a = ancestor.lexically_normal();
  const auto b = p.lexically_normal();
  auto ia      = a.begin();
  auto ib      = b.begin();
  for (; ia != a.end(); ++ia, ++ib) {
    if (ib == b.end() || *ia != *ib) {
      return false;
    }
  }
  return ib != b.end();
}

} // namespace

RouterLens::RouterLens(std::filesystem::path root) : base_(std::move(root)) {}

bool RouterLens::Mount::owns(const ExternalKey &key) const {
  for (const auto &root : roots) {
    if (key == root) {
      return true;
    }
    const std::string prefix = root.ends_with(':') ? root : root + ':';
    if (key.starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

std::expected<std::unique_ptr<ProjectionLens>, std::string>
RouterLens::delegateFor(const std::filesystem::path &path) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    if (!std::filesystem::exists(path / ".git", ec)) {
      return std::unexpected("no delegate lens: not a git worktree: " +
                             path.string());
    }
    auto lens = std::make_unique<GitLens>(path.string());
    if (const auto r = lens->browse(); !r) {
      return std::unexpected(r.error());
    }
    return lens;
  }
  const auto ext = path.extension().string();
  if (ext == ".log") {
    auto lens = std::make_unique<LogLens>(path.string());
    if (const auto r = lens->browse(); !r) {
      return std::unexpected(r.error());
    }
    return lens;
  }
  if (ext == ".json") {
    // ConfigLens reads lazily inside forEachNode and renders a parse or
    // read failure as cell content, so there is nothing to refuse here.
    return std::unique_ptr<ProjectionLens>{
        std::make_unique<ConfigLens>(path.string())};
  }
  if (ext == ".db" || ext == ".sqlite" || ext == ".sqlite3") {
    // A SQLite database mounts a browse of its first table (rows
    // paged; the file's own stamp/watch drives pollChanged).
    auto lens = openSqlUri(path.string());
    if (!lens) {
      return std::unexpected(lens.error());
    }
    const auto table = std::ranges::find_if(
        (*lens)->tables(), [](const SqlTable &t) { return !t.isView; });
    if (table == (*lens)->tables().end()) {
      return std::unexpected("no base tables: " + path.string());
    }
    if (const auto r = (*lens)->browse(table->name); !r) {
      return std::unexpected(r.error());
    }
    return std::unique_ptr<ProjectionLens>{std::move(*lens)};
  }
  // A systemd unit filename opens that unit's journal -- one hop to
  // the evented log source; the whole journal through a `journal`
  // entry (file or dir).
  if (const std::string name = path.filename().string();
      name == "journal" || ext == ".service" || ext == ".socket" ||
      ext == ".timer" || ext == ".target" || ext == ".mount" ||
      ext == ".scope" || ext == ".slice") {
    auto lens = std::make_unique<JournalLens>(name == "journal" ? "" : name);
    if (const auto r = lens->browse(); !r) {
      return std::unexpected(r.error());
    }
    return lens;
  }
  return std::unexpected("no delegate lens classifies " + path.string());
}

std::expected<ExternalKey, std::string>
RouterLens::open(const ExternalKey &cellKey) {
  const auto path = base_.pathFor(cellKey);
  if (!path.has_value()) {
    return std::unexpected("not a filesystem cell: " + cellKey);
  }
  for (const auto &m : mounts_) {
    if (m.mountKey == cellKey) {
      return m.roots.front(); // already mounted -- the same root again
    }
  }
  auto delegate = delegateFor(*path);
  if (!delegate) {
    return std::unexpected(delegate.error());
  }
  Mount m{.mountKey = cellKey, .path = *path, .lens = std::move(*delegate)};
  m.lens->forEachRoot(
      [&](const ExternalKey &root) { m.roots.push_back(root); });
  if (m.roots.empty()) {
    return std::unexpected("delegate lens declares no roots: " +
                           path->string());
  }
  const ExternalKey rootKey = m.roots.front();
  mounts_.push_back(std::move(m));
  return rootKey;
}

std::optional<ExternalKey>
RouterLens::mountBoundary(const ExternalKey &key) const {
  for (const auto &m : mounts_) {
    if (m.mountKey == key) {
      return m.mountKey; // focus sits on the mount cell itself
    }
  }
  for (const auto &m : mounts_) {
    if (m.owns(key)) {
      return m.mountKey; // focus is inside a mounted delegate
    }
  }
  // An fs cell beneath a mounted path: the deepest containing mount is
  // the boundary one hop out.
  if (const auto p = base_.pathFor(key); p.has_value()) {
    std::optional<ExternalKey> best;
    std::size_t bestDepth = 0;
    for (const auto &m : mounts_) {
      if (!pathContains(m.path, *p)) {
        continue;
      }
      const auto depth =
          static_cast<std::size_t>(std::distance(m.path.begin(), m.path.end()));
      if (!best.has_value() || depth > bestDepth) {
        best      = m.mountKey;
        bestDepth = depth;
      }
    }
    return best;
  }
  return std::nullopt;
}

std::expected<void, std::string>
RouterLens::unmount(const ExternalKey &mountKey) {
  const auto it = std::ranges::find_if(
      mounts_, [&](const Mount &m) { return m.mountKey == mountKey; });
  if (it == mounts_.end()) {
    return std::unexpected("no mount at " + mountKey);
  }
  mounts_.erase(it);
  return {};
}

bool RouterLens::isMounted(const ExternalKey &key) const {
  return std::ranges::any_of(mounts_,
                             [&](const Mount &m) { return m.mountKey == key; });
}

std::vector<std::pair<std::string, std::optional<GenericDim>>>
RouterLens::dimensions() const {
  // Base first, mounts after, first-wins on name (UnionLens's rule --
  // mounts join over time, so this answer grows between calls and the
  // session mints late-declared dims lazily through edges/attributes).
  std::vector<std::pair<std::string, std::optional<GenericDim>>> out =
      base_.dimensions();
  std::unordered_set<std::string> seen;
  for (const auto &[name, g] : out) {
    seen.insert(name);
  }
  for (const auto &m : mounts_) {
    for (const auto &dim : m.lens->dimensions()) {
      if (seen.insert(dim.first).second) {
        out.push_back(dim);
      }
    }
  }
  return out;
}

void RouterLens::forEachNode(
    const gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  base_.forEachNode(emit);
  for (const auto &m : mounts_) {
    m.lens->forEachNode(emit);
  }
}

void RouterLens::forEachEdge(
    const gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  base_.forEachEdge(emit);
  for (const auto &m : mounts_) {
    m.lens->forEachEdge(emit);
  }
  // The synthetic mount edges, emitted last so mount -> delegate-root
  // wins the cell's one posward d.contains slot over the base's own
  // first-child edge.
  const std::string contains{genericDimName(GenericDim::Containment)};
  const std::string seq{genericDimName(GenericDim::Sequence)};
  for (const auto &m : mounts_) {
    emit({m.mountKey, m.roots.front(), contains, Posward});
    ExternalKey tail = m.roots.front();
    for (std::size_t i = 1; i < m.roots.size(); ++i) {
      emit({tail, m.roots[i], seq, Posward});
      tail = m.roots[i];
    }
    // Splice the mount cell's displaced filesystem child run behind the
    // delegate roots: [delegate roots..., fs children...] stays one
    // navigable run and unmounting reverts it cleanly.
    const auto kids = base_.childKeys(m.mountKey);
    if (!kids.empty()) {
      emit({tail, kids.front(), seq, Posward});
    }
  }
}

bool RouterLens::forEachRoot(
    const gleditor::cpp26::function_ref<void(const ExternalKey &)> emit) const {
  const auto root = base_.rootKey();
  if (!root.has_value()) {
    return false; // unreadable root -- let structural election answer
  }
  emit(*root);
  return true;
}

std::vector<std::pair<std::string, std::string>>
RouterLens::staleAttributes(const ExternalKey &key) const {
  if (base_.pathFor(key).has_value()) {
    return base_.staleAttributes(key);
  }
  for (const auto &m : mounts_) {
    if (m.owns(key)) {
      return m.lens->staleAttributes(key);
    }
  }
  // An unmounted delegate's orphans get the same marker the delegate
  // lenses stamp themselves.
  return {{"d.stale", "1"}};
}

bool RouterLens::pollChanged() const {
  if (base_.pollChanged()) {
    return true;
  }
  for (const auto &m : mounts_) {
    if (m.lens->pollChanged()) {
      return true;
    }
  }
  return false;
}

std::optional<int> RouterLens::waitableFd() const noexcept {
  if (const auto fd = base_.waitableFd(); fd.has_value()) {
    return fd;
  }
  for (const auto &m : mounts_) {
    if (const auto fd = m.lens->waitableFd(); fd.has_value()) {
      return fd;
    }
  }
  return std::nullopt;
}

bool RouterLens::canApply(const ExternalKey &node) const {
  if (base_.pathFor(node).has_value()) {
    return base_.canApply(node); // fs cells stay read-only today
  }
  for (const auto &m : mounts_) {
    if (m.owns(node)) {
      return m.lens->canApply(node);
    }
  }
  return false;
}

std::expected<ExternalKey, std::string>
RouterLens::applyContentChange(const ExternalKey &node,
                               const std::string_view newContent) {
  if (base_.pathFor(node).has_value()) {
    return base_.applyContentChange(node, newContent); // read-only refuse
  }
  for (const auto &m : mounts_) {
    if (m.owns(node)) {
      return m.lens->applyContentChange(node, newContent);
    }
  }
  return std::unexpected("no mounted lens owns " + node);
}

std::expected<void, std::string>
RouterLens::materializePage(const ExternalKey &windowKey) {
  for (const auto &m : mounts_) {
    if (!m.owns(windowKey)) {
      continue;
    }
    if (auto *w = dynamic_cast<WindowedLens *>(m.lens.get()); w != nullptr) {
      return w->materializePage(windowKey);
    }
    return std::unexpected("mounted lens is not windowed: " + windowKey);
  }
  return std::unexpected("no mounted lens owns " + windowKey);
}

void RouterLens::discardMaterializedAfter(const ExternalKey &boundaryKey) {
  // Broadcast: each windowed mount ignores keys that are not its own
  // (a foreign boundary never matches a page key), so this is safe.
  for (const auto &m : mounts_) {
    if (auto *w = dynamic_cast<WindowedLens *>(m.lens.get()); w != nullptr) {
      w->discardMaterializedAfter(boundaryKey);
    }
  }
}

std::optional<ExternalKey> RouterLens::nextStub() const {
  for (const auto &m : mounts_) {
    if (const auto *w = dynamic_cast<const WindowedLens *>(m.lens.get());
        w != nullptr) {
      if (auto stub = w->nextStub(); stub.has_value()) {
        return stub;
      }
    }
  }
  return std::nullopt;
}

std::expected<WindowedLens *, std::string>
RouterLens::windowedFor(const ExternalKey &mountKey) {
  const auto it = std::ranges::find_if(
      mounts_, [&](const Mount &m) { return m.mountKey == mountKey; });
  if (it == mounts_.end()) {
    return std::unexpected("no mount at " + mountKey);
  }
  auto *w = dynamic_cast<WindowedLens *>(it->lens.get());
  if (w == nullptr) {
    return std::unexpected("mounted lens is not windowed: " + mountKey);
  }
  return w;
}

std::expected<std::optional<ExternalKey>, std::string>
RouterLens::nextStubAt(const ExternalKey &mountKey) const {
  const auto it = std::ranges::find_if(
      mounts_, [&](const Mount &m) { return m.mountKey == mountKey; });
  if (it == mounts_.end()) {
    return std::unexpected("no mount at " + mountKey);
  }
  const auto *w = dynamic_cast<const WindowedLens *>(it->lens.get());
  if (w == nullptr) {
    return std::unexpected("mounted lens is not windowed: " + mountKey);
  }
  return w->nextStub();
}

} // namespace zigzag
