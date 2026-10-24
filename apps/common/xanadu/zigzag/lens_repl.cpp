#include "common/xanadu/zigzag/lens_repl.hpp"

#include <string>
#include <vector>

#include "common/xanadu/spool.hpp"
#include "common/xanadu/zigzag/lens_uri.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/meta_lens.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"
#include "common/xanadu/zigzag/router_lens.hpp"

namespace zigzag {

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
  session_ = std::make_unique<ProjectionSession>(std::move(lens), core_);
  label_   = std::move(label);
  printLensTree(*session_, core_, arena_, label_, out, /*withKeys=*/true);
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
    session_  = std::make_unique<ProjectionSession>(std::move(lens), core_);
    label_    = std::string{uri};
    printLensTree(*session_, core_, arena_, label_, out, /*withKeys=*/true);
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

bool LensRepl::run(const std::string_view line, std::ostream &out,
                   const MetaSource &metaSource) {
  const auto arg = line.size() > 5 ? line.substr(6) : std::string_view{};

  if (arg.empty()) {
    if (session_ == nullptr) {
      out << "lens: nothing open\n";
    } else {
      printLensTree(*session_, core_, arena_, label_, out, /*withKeys=*/true);
    }
    return true;
  }
  if (arg.starts_with("open ")) {
    return open(arg.substr(5), out, metaSource);
  }
  if (arg == "close") {
    session_.reset();
    metaFold_.reset();
    label_.clear();
    out << "lens: closed\n";
    return true;
  }
  if (session_ == nullptr) {
    // A bare argument still means "open this" -- the one-keystroke
    // form, so ":lens /tmp/foo" does not need the extra word.
    return open(arg, out, metaSource);
  }
  if (arg.starts_with("mount ")) {
    auto *router = dynamic_cast<RouterLens *>(session_->lensMut());
    if (router == nullptr) {
      out << "lens: no router at the root -- mount needs a dir: view\n";
      return true;
    }
    const auto opened = router->open(std::string{arg.substr(6)});
    if (!opened) {
      out << "lens: mount refused: " << opened.error() << "\n";
      return true;
    }
    (void)session_->refresh();
    printLensTree(*session_, core_, arena_, label_, out, /*withKeys=*/true);
    return true;
  }
  if (arg.starts_with("unmount ")) {
    auto *router = dynamic_cast<RouterLens *>(session_->lensMut());
    if (router == nullptr) {
      out << "lens: no router at the root\n";
      return true;
    }
    const auto boundary = router->mountBoundary(std::string{arg.substr(8)});
    if (!boundary) {
      out << "lens: no mount at or above " << arg.substr(8) << "\n";
      return true;
    }
    if (!router->unmount(*boundary)) {
      out << "lens: unmount refused\n";
      return true;
    }
    (void)session_->refresh();
    printLensTree(*session_, core_, arena_, label_, out, /*withKeys=*/true);
    return true;
  }
  if (arg == "next") {
    auto *w = dynamic_cast<WindowedLens *>(session_->lensMut());
    if (w == nullptr) {
      out << "lens: not a windowed lens\n";
      return true;
    }
    const auto stub = w->nextStub();
    if (!stub) {
      out << "lens: no deferred page\n";
      return true;
    }
    if (const auto r = w->materializePage(*stub); !r) {
      out << "lens: materialize failed\n";
      return true;
    }
    (void)session_->refresh();
    printLensTree(*session_, core_, arena_, label_, out, /*withKeys=*/true);
    return true;
  }
  if (arg == "commit") {
    const auto c = session_->commit();
    if (!c) {
      out << "lens: commit failed: " << c.error() << "\n";
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
