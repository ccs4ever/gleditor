/**
 * @file lens_repl.hpp
 * @brief The ":lens" surface every Vortex frontend shares: one
 *        ProjectionSession swapped by REPL verbs, mirroring the xuzz
 *        lens gestures (open/back/next/commit) plus the close the
 *        picker owns there. Also the programmatic raster: runLensRaster
 *        gives `--lens <uri>` flags the same tree `xuzz --lens` draws.
 */
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"
#include "common/xanadu/zigzag/zzcore.hpp"

namespace xanadu {
class SpanReader;
}

namespace zigzag {

/// REPL-facing lens session. The session is rebuilt on each open; the
/// arena and core persist across swaps (a CellRef from the old session
/// is never reused).
class LensRepl {
public:
  /// Resolves a "meta:<label>" URI: the frontend's frozen fold for that
  /// store (store.rebuildManifold(version)) paired with the store as
  /// its SpanReader. The fold is moved into the session it builds and
  /// must outlive the lens -- which it does, session_ destructs first.
  using MetaSource = std::function<
      std::expected<std::pair<Manifold, const xanadu::SpanReader *>,
                    std::string>(std::string_view label)>;

  /// True when `line` is a ":lens" command (the word, or the word plus
  /// arguments).
  static bool handles(std::string_view line) noexcept {
    return line == ":lens" || line.starts_with(":lens ");
  }

  /// The :help lines a frontend prints verbatim.
  static constexpr std::string_view helpText =
      "  :lens <uri>           open a lens -- path, dir:, git:, sql:, "
      "cfg:, log:, journal:, union:, meta:<label>\n"
      "  :lens                 reprint the open projection's tree\n"
      "  :lens mount <key>     mount the delegate lens under a cell\n"
      "  :lens unmount <key>   unmount the delegate at or above a key\n"
      "  :lens next            materialize the next deferred page\n"
      "  :lens commit          write dirty cells back to the source\n"
      "  :lens close           drop the projection\n";

  /// Own fold: projections land in an internal arena+core -- the
  /// standalone surface (--lens, tests, vlace).
  LensRepl();
  /// Shared fold: projections land in `core`'s arena, so the lens's
  /// cells are cells the frontend's own language can reach -- VQL
  /// queries them by dimension, VPL ranks them, goals unify over them.
  /// The caller's core (and its arena) must outlive this object.
  explicit LensRepl(vortex::VortexCore &core);

  /// Run one :lens command, writing the reply to `out`. @p metaSource
  /// resolves meta: URIs; pass {} where the frontend holds no store.
  /// Always returns true -- a line that reaches here is a :lens
  /// command, and failures are replies, not errors to the caller.
  bool run(std::string_view line, std::ostream &out,
           const MetaSource &metaSource = {});

  /// The currently-open URI/label, or empty.
  [[nodiscard]] std::string_view label() const noexcept { return label_; }

  /// The projection's first root -- what a language binds as its entry
  /// variable ($lens, a rank head, a goal term). Nothing when no lens
  /// is open.
  [[nodiscard]] OptionalCell root() const noexcept {
    if (session_ == nullptr || session_->roots().empty()) {
      return std::nullopt;
    }
    return session_->roots().front();
  }

  /// Every projected cell as (ExternalKey, text) pairs in tree order --
  /// what a language without a cell term (vprolog) seeds as facts.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>>
  entries() const;

private:
  /// Swap in a fresh projection over `uri` and print its tree.
  bool open(std::string_view uri, std::ostream &out,
            const MetaSource &metaSource);
  /// Rebuild `session_` over `lens`; the lens is already constructed.
  void adopt(std::unique_ptr<ProjectionLens> lens, std::string label,
             std::ostream &out);

  // metaFold_ precedes session_ so the session -- which holds a
  // MetaLens borrowing the fold -- is destroyed first. core_/arena_
  // point at ownCore_/ownArena_ or at the caller's shared fold.
  ArenaManifold ownArena_;
  std::optional<vortex::VortexCore> ownCore_;
  vortex::VortexCore *core_;
  ArenaManifold *arena_;
  std::optional<Manifold> metaFold_;
  std::unique_ptr<ProjectionSession> session_;
  std::string label_;
};

/// The projected tree, in the `xuzz --headless --lens` raster shape.
/// @p withKeys appends each cell's ExternalKey -- what a REPL user
/// mounts/unmounts by.
void printLensTree(ProjectionSession &session, vortex::VortexCore &core,
                   ArenaManifold &arena, std::string_view label,
                   std::ostream &out, bool withKeys = false);

/// The shared programmatic surface: open `uri`, print its tree to
/// `out`, return a process-style status (0 = projected).
int runLensRaster(std::string_view uri, std::ostream &out);

} // namespace zigzag
