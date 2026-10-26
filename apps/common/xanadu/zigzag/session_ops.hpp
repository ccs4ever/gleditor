/**
 * @file session_ops.hpp
 * @brief The mount/unmount/next-page/commit/edit/revert gesture
 *        helpers every session-driven frontend shares -- one copy of
 *        the gesture + refresh() + dirty-gate protocol
 *        (design/vlace-lens-bridge.md §4, P7).
 *
 * The helpers take the session and ExternalKey-space arguments; WHICH
 * cell names which key is the caller's job (the ":lens" REPL parses a
 * key string; vlace's runIO resolves a Cell through
 * ProjectionSession::ownsCell and keyFor). The mutating gestures are
 * dirty-gated INSIDE the helper -- mount/unmount/next-page refuse
 * while the session holds uncommitted edits, so every caller inherits
 * the rule and the copies cannot drift. sessionEdit/sessionCommitAll/
 * sessionRevert are exempt: a second edit must land, and commit/revert
 * are the gate's only exits (§4's edit/commit rule). A gesture's
 * read-only half -- the mountBoundary/nextStub/nextStubAt resolution
 * -- runs BEFORE the gate so a refused, idempotent, or exhausted
 * gesture reports honestly instead of dirty-blocked (the same rule
 * that leaves lensStub ungated).
 *
 * xuzz keeps its own four gesture copies
 * (xuzz_app.cpp's materializeNextStub/openFocusedDelegate/
 * backOutOfMount/commitDirty) and does not call in here -- its clobber
 * fix is the dirty-preserving restateContent, not the gate.
 */
#ifndef COMMON_XANADU_ZIGZAG_SESSION_OPS_HPP
#define COMMON_XANADU_ZIGZAG_SESSION_OPS_HPP

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "common/xanadu/zigzag/projection_session.hpp"
#include "common/xanadu/zigzag/router_lens.hpp"

namespace zigzag {

/// The dirty-gate probe the mutating gestures share: Err naming the
/// pending-edit count while `session` holds uncommitted edits.
[[nodiscard]] inline std::expected<void, std::string>
sessionDirty(const ProjectionSession &session) {
  if (const auto n = session.dirtyCount(); n != 0) {
    return std::unexpected("session holds " + std::to_string(n) +
                           " pending edit(s) -- commit or revert first");
  }
  return {};
}

/// Mount the delegate lens under @p key's cell and refresh() so the
/// projection shows it; Ok carries the delegate's root key (the cell
/// the reader lands under). Re-opening an already-mounted cell is
/// idempotent and wins over the dirty-gate: it mutates nothing, so it
/// is refused only by RouterLens::open itself.
[[nodiscard]] inline std::expected<ExternalKey, std::string>
sessionMountAt(ProjectionSession &session, const ExternalKey &key) {
  auto *router = dynamic_cast<RouterLens *>(session.lensMut());
  if (router == nullptr) {
    return std::unexpected("no router at the root -- mount needs a dir: view");
  }
  if (!router->isMounted(key)) {
    if (const auto gate = sessionDirty(session); !gate.has_value()) {
      return std::unexpected(gate.error());
    }
  }
  const auto opened = router->open(key);
  if (!opened.has_value()) {
    return std::unexpected("mount refused: " + opened.error());
  }
  (void)session.refresh();
  return *opened;
}

/// Drop the mount at-or-above @p key (mountBoundary resolves "one hop
/// out" for a delegate or mounted-descendant key) and refresh(). A key
/// with no boundary refuses before the gate -- nothing would mutate.
[[nodiscard]] inline std::expected<void, std::string>
sessionUnmountAt(ProjectionSession &session, const ExternalKey &key) {
  auto *router = dynamic_cast<RouterLens *>(session.lensMut());
  if (router == nullptr) {
    return std::unexpected("no router at the root");
  }
  const auto boundary = router->mountBoundary(key);
  if (!boundary.has_value()) {
    return std::unexpected("no mount at or above " + key);
  }
  if (const auto gate = sessionDirty(session); !gate.has_value()) {
    return gate;
  }
  if (const auto dropped = router->unmount(*boundary); !dropped.has_value()) {
    return std::unexpected("unmount refused: " + dropped.error());
  }
  (void)session.refresh();
  return {};
}

/// Materialize the next deferred page and refresh(); Ok carries the
/// materialized stub's key, Ok(nullopt) is exhaustion.
/// @p scope scopes the stub search to one mount: a mount key from
/// RouterLens::mountBoundary, resolved through nextStubAt/windowedFor
/// (the unscoped WindowedLens::nextStub is first-wins across mounts).
/// nullopt scope is session-level -- the whole lens must be
/// WindowedLens, which RouterLens already is. The stub lookup is a
/// const query and precedes the gate; only materializing dirties.
[[nodiscard]] inline std::expected<std::optional<ExternalKey>, std::string>
sessionNextPage(ProjectionSession &session,
                const std::optional<ExternalKey> &scope) {
  WindowedLens *window = nullptr;
  std::optional<ExternalKey> stub;
  if (scope.has_value()) {
    auto *router = dynamic_cast<RouterLens *>(session.lensMut());
    if (router == nullptr) {
      return std::unexpected("no router at the root");
    }
    const auto found = router->nextStubAt(*scope);
    if (!found.has_value()) {
      return std::unexpected(found.error());
    }
    stub = *found;
    if (stub.has_value()) {
      const auto win = router->windowedFor(*scope);
      if (!win.has_value()) {
        return std::unexpected(win.error());
      }
      window = *win;
    }
  } else {
    window = dynamic_cast<WindowedLens *>(session.lensMut());
    if (window == nullptr) {
      return std::unexpected("not a windowed lens");
    }
    stub = window->nextStub();
  }
  if (!stub.has_value()) {
    return std::optional<ExternalKey>{std::nullopt};
  }
  if (const auto gate = sessionDirty(session); !gate.has_value()) {
    return std::unexpected(gate.error());
  }
  if (const auto r = window->materializePage(*stub); !r.has_value()) {
    return std::unexpected("materialize failed: " + r.error());
  }
  (void)session.refresh();
  return stub;
}

/// Push every dirty cell's text through the lens's unproject half and
/// refresh() on success; Ok carries the applied count. Exempt from the
/// gate -- it is the dirty state exits through.
[[nodiscard]] inline std::expected<int, std::string>
sessionCommitAll(ProjectionSession &session) {
  return session.commit();
}

/// Arena-local edit on one projected cell; no refresh -- commit() is
/// what crosses back through the lens (§4's edit rule). Exempt from
/// the gate: a second edit must land.
[[nodiscard]] inline std::expected<void, std::string>
sessionEdit(ProjectionSession &session, const ExternalKey &key,
            const std::string_view text) {
  return session.editContent(key, text);
}

/// Drop one dirty edit without committing; refresh() restates the
/// source's content only when the key WAS dirty. Exempt from the
/// gate -- with commit it is how a wedged session exits dirty state.
[[nodiscard]] inline std::expected<void, std::string>
sessionRevert(ProjectionSession &session, const ExternalKey &key) {
  return session.revertKey(key);
}

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_SESSION_OPS_HPP
