#include "common/xanadu/zigzag/lens_repl.hpp"

#include <string>
#include <vector>

#include <sys/stat.h>

#include "common/xanadu/spool.hpp"
#include "common/xanadu/zigzag/lens_uri.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/meta_lens.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"
#include "common/xanadu/zigzag/router_lens.hpp"

namespace zigzag {

namespace {

/// A path-shaped arg resolves to the cell's fs:<dev>:<ino> key --
/// users think in paths, mounts are keyed by inode. Anything else is
/// already an ExternalKey and passes through.
std::string keyArg(const std::string_view arg) {
  std::string key{arg};
  struct stat st{};
  if (arg.starts_with('/') && ::stat(key.c_str(), &st) == 0) {
    key = "fs:" + std::to_string(static_cast<std::uint64_t>(st.st_dev)) + ":" +
          std::to_string(static_cast<std::uint64_t>(st.st_ino));
  }
  return key;
}

} // namespace

LensRepl::LensRepl()
    : ownCore_{std::in_place, ownArena_}, core_{&*ownCore_},
      arena_{&ownArena_} {}

LensRepl::LensRepl(vortex::VortexCore &core)
    : core_{&core}, arena_{&core.arena()} {}

std::vector<std::pair<std::string, std::string>> LensRepl::entries() const {
  std::vector<std::pair<std::string, std::string>> out;
  if (session_ == nullptr) {
    return out;
  }
  // Iterative DFS like printLensTree: contains down, seq across. Deep
  // projections must not land on the call stack.
  const auto contains = core_->findOrMintDimension("d.contains");
  const auto seq      = core_->findOrMintDimension("d.seq");
  std::vector<CellRef> stack{session_->roots().rbegin(),
                             session_->roots().rend()};
  while (!stack.empty()) {
    const auto cell = stack.back();
    stack.pop_back();
    out.emplace_back(session_->keyFor(cell).value_or(""), arena_->textOf(cell));
    // Push the children reversed so they pop in seq order.
    std::vector<CellRef> kids;
    for (auto child               = arena_->linked(cell, contains, Posward);
         child.has_value(); child = arena_->linked(*child, seq, Posward)) {
      kids.push_back(*child);
    }
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
      stack.push_back(*it);
    }
  }
  return out;
}

void printLensTree(ProjectionSession &session, vortex::VortexCore &core,
                   ArenaManifold &arena, const std::string_view label,
                   std::ostream &out, const bool withKeys) {
  const auto dim = [&](std::string_view name) {
    return core.findOrMintDimension(name);
  };
  auto attr = [&](const CellRef c, const std::string_view rank) {
    const auto a = arena.linked(c, dim(rank), Posward);
    return a.has_value() ? arena.textOf(*a) : std::string{};
  };

  out << "lens: " << label << " -> " << session.cellCount() << " cells, "
      << session.roots().size() << " root(s)\n";

  auto printTree = [&](auto &&self, const CellRef cell,
                       const int depth) -> void {
    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    out << indent << arena.textOf(cell) << "  [" << attr(cell, "d.role") << ", "
        << attr(cell, "d.mime") << "]";
    if (withKeys) {
      if (const auto k = session.keyFor(cell); k.has_value()) {
        out << "  " << *k;
      }
    }
    out << "\n";
    auto child = arena.linked(cell, dim("d.contains"), Posward);
    for (; child.has_value();
         child = arena.linked(*child, dim("d.seq"), Posward)) {
      self(self, *child, depth + 1);
    }
  };
  for (const auto root : session.roots()) {
    printTree(printTree, root, 0);
  }
}

void LensRepl::adopt(std::unique_ptr<ProjectionLens> lens, std::string label,
                     std::ostream &out) {
  // The old lens dies with its session, before any fold it borrowed is
  // released.
  session_.reset();
  metaFold_.reset();
  session_ = std::make_unique<ProjectionSession>(std::move(lens), *core_);
  label_   = std::move(label);
  printLensTree(*session_, *core_, *arena_, label_, out, /*withKeys=*/true);
}

bool LensRepl::open(const std::string_view uri, std::ostream &out,
                    const MetaSource &metaSource) {
  if (uri.starts_with("meta:")) {
    if (!metaSource) {
      out << "lens: meta: needs an open store\n";
      return true;
    }
    auto src = metaSource(uri.substr(5));
    if (!src) {
      out << "lens: " << src.error() << "\n";
      return true;
    }
    // The old session dies before the fold it could be borrowing, then
    // the new fold moves in and the lens borrows it. Not adopt() --
    // that resets metaFold_.
    session_.reset();
    metaFold_ = std::move(src->first);
    auto lens = std::make_unique<MetaLens>(std::string{uri.substr(5)},
                                           *metaFold_, *src->second);
    session_  = std::make_unique<ProjectionSession>(std::move(lens), *core_);
    label_    = std::string{uri};
    printLensTree(*session_, *core_, *arena_, label_, out, /*withKeys=*/true);
    return true;
  }
  auto lens = openLensUri(uri);
  if (!lens) {
    out << "lens: cannot open " << uri << ": " << lens.error() << "\n";
    return true;
  }
  adopt(std::move(*lens), std::string{uri}, out);
  return true;
}

std::expected<void, std::string> LensRepl::openUri(const std::string_view uri,
                                                   const MetaSource &meta) {
  if (uri.starts_with("meta:")) {
    if (!meta) {
      return std::unexpected("meta: needs an open store");
    }
    auto src = meta(uri.substr(5));
    if (!src) {
      return std::unexpected(src.error());
    }
    // The old session dies before the fold it could be borrowing, then
    // the new fold moves in and the lens borrows it.
    session_.reset();
    metaFold_ = std::move(src->first);
    auto lens = std::make_unique<MetaLens>(std::string{uri.substr(5)},
                                           *metaFold_, *src->second);
    session_  = std::make_unique<ProjectionSession>(std::move(lens), *core_);
    label_    = std::string{uri};
    return {};
  }
  auto lens = openLensUri(uri);
  if (!lens) {
    return std::unexpected("cannot open " + std::string{uri} + ": " +
                           lens.error());
  }
  session_.reset();
  metaFold_.reset();
  session_ = std::make_unique<ProjectionSession>(std::move(*lens), *core_);
  label_   = std::string{uri};
  return {};
}

std::expected<void, std::string>
LensRepl::mountKey(const std::string_view key) {
  auto *router = session_ != nullptr
                     ? dynamic_cast<RouterLens *>(session_->lensMut())
                     : nullptr;
  if (router == nullptr) {
    return std::unexpected("no router at the root -- mount needs a dir: view");
  }
  const auto opened = router->open(keyArg(key));
  if (!opened) {
    return std::unexpected("mount refused: " + opened.error());
  }
  (void)session_->refresh();
  return {};
}

std::expected<void, std::string>
LensRepl::unmountKey(const std::string_view key) {
  auto *router = session_ != nullptr
                     ? dynamic_cast<RouterLens *>(session_->lensMut())
                     : nullptr;
  if (router == nullptr) {
    return std::unexpected("no router at the root");
  }
  const auto boundary = router->mountBoundary(keyArg(key));
  if (!boundary) {
    return std::unexpected("no mount at or above " + std::string{key});
  }
  if (!router->unmount(*boundary)) {
    return std::unexpected("unmount refused");
  }
  (void)session_->refresh();
  return {};
}

std::expected<void, std::string> LensRepl::nextPage() {
  auto *w = session_ != nullptr
                ? dynamic_cast<WindowedLens *>(session_->lensMut())
                : nullptr;
  if (w == nullptr) {
    return std::unexpected("not a windowed lens");
  }
  const auto stub = w->nextStub();
  if (!stub) {
    return std::unexpected("no deferred page");
  }
  if (const auto r = w->materializePage(*stub); !r) {
    return std::unexpected("materialize failed");
  }
  (void)session_->refresh();
  return {};
}

std::expected<void, std::string> LensRepl::commit() {
  if (session_ == nullptr) {
    return std::unexpected("nothing open");
  }
  const auto r = session_->commit();
  if (!r) {
    return std::unexpected("commit refused: " + r.error());
  }
  return {};
}

void LensRepl::close() {
  session_.reset();
  metaFold_.reset();
  label_.clear();
}

bool LensRepl::run(const std::string_view line, std::ostream &out,
                   const MetaSource &metaSource) {
  const auto arg = line.size() > 5 ? line.substr(6) : std::string_view{};

  if (arg.empty()) {
    if (session_ == nullptr) {
      out << "lens: nothing open\n";
    } else {
      printLensTree(*session_, *core_, *arena_, label_, out, /*withKeys=*/true);
    }
    return true;
  }
  if (arg.starts_with("open ")) {
    return open(arg.substr(5), out, metaSource);
  }
  if (arg == "close") {
    close();
    out << "lens: closed\n";
    return true;
  }
  if (session_ == nullptr) {
    // A bare argument still means "open this" -- the one-keystroke
    // form, so ":lens /tmp/foo" does not need the extra word.
    return open(arg, out, metaSource);
  }
  if (arg.starts_with("mount ")) {
    if (const auto r = mountKey(arg.substr(6)); !r) {
      out << "lens: " << r.error() << "\n";
      return true;
    }
    printLensTree(*session_, *core_, *arena_, label_, out, /*withKeys=*/true);
    return true;
  }
  if (arg.starts_with("unmount ")) {
    if (const auto r = unmountKey(arg.substr(8)); !r) {
      out << "lens: " << r.error() << "\n";
      return true;
    }
    printLensTree(*session_, *core_, *arena_, label_, out, /*withKeys=*/true);
    return true;
  }
  if (arg == "next") {
    auto *w = dynamic_cast<WindowedLens *>(session_->lensMut());
    if (w == nullptr) {
      out << "lens: not a windowed lens\n";
      return true;
    }
    if (!w->nextStub()) {
      out << "lens: no deferred page\n";
      return true;
    }
    if (const auto r = nextPage(); !r) {
      out << "lens: " << r.error() << "\n";
      return true;
    }
    printLensTree(*session_, *core_, *arena_, label_, out, /*withKeys=*/true);
    return true;
  }
  if (arg == "commit") {
    if (const auto r = commit(); !r) {
      out << "lens: commit failed: " << r.error() << "\n";
    } else {
      out << "lens: committed\n";
    }
    return true;
  }
  // Anything else with a session open is treated as an open too --
  // swapping sources is the cheap mistake to make recoverable.
  return open(arg, out, metaSource);
}

int runLensRaster(const std::string_view uri, std::ostream &out) {
  auto lens = openLensUri(uri);
  if (!lens) {
    out << "lens: cannot open " << uri << ": " << lens.error() << "\n";
    return 1;
  }
  ArenaManifold arena;
  vortex::VortexCore core{arena};
  ProjectionSession session(std::move(*lens), core);
  printLensTree(session, core, arena, uri, out);
  return 0;
}

} // namespace zigzag
