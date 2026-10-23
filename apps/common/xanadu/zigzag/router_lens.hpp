/**
 * @file router_lens.hpp
 * @brief RouterLens: a filesystem projection that mounts delegate lenses
 *        under individual cells -- cross-lens composition, one hop in
 *        and one hop out.
 *
 * UnionLens delegates by SCHEME: every child contributes to one flat
 * view. RouterLens delegates by CELL: the base is a FilesystemLens, and
 * opening a focused fs cell mounts the lens its path classifies for --
 * a directory holding .git -> GitLens, a *.log file -> LogLens, a *.json
 * file -> ConfigLens -- so the delegated projection hangs UNDER the file
 * cell in the same arena and the same key space (the delegate's keys
 * carry their own git:/log:/cfg: scheme and never collide with fs:).
 *
 * Mount semantics: the mount cell gains a synthetic d.contains edge to
 * each delegate-declared root, emitted after the base's own edges so it
 * wins the cell's one posward d.contains slot. When the mount cell had
 * filesystem children (a git worktree's files), the delegate roots are
 * spliced IN FRONT of the old child-run head on d.seq -- the run reads
 * [delegate roots..., fs children...], everything stays reachable, and
 * negward traversal climbs back out through the same cells. Unmounting
 * drops the delegate: the base re-asserts the plain child run, the
 * session's tombstone pass restores the mount cell's d.contains, and
 * the delegate's absent keys stale-mark per the projection rule.
 *
 * The mounted lens keeps its own evented life -- its pollChanged()/
 * waitableFd() feed the router's, and the router forwards the WindowedLens
 * verbs to the mount that owns the key, so a mounted GitLens still
 * refreshes when HEAD moves and lens.next-page still pages its log.
 */
#ifndef COMMON_XANADU_ZIGZAG_ROUTER_LENS_HPP
#define COMMON_XANADU_ZIGZAG_ROUTER_LENS_HPP

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/zigzag/filesystem_lens.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

/// A filesystem projection with per-cell delegate mounts.
class RouterLens final : public ProjectionLens, public WindowedLens {
public:
  /// Projects @p root's directory tree exactly as FilesystemLens does;
  /// mounts are layered on afterwards by open().
  explicit RouterLens(std::filesystem::path root);

  // -- the gestures --------------------------------------------------------

  /// The open gesture: classify the fs cell @p cellKey names and mount
  /// the matching delegate lens under it. Returns the delegate's root
  /// key (the cell the reader lands under), or the refusal. Idempotent:
  /// re-opening a mounted cell answers its existing mount's root.
  [[nodiscard]] std::expected<ExternalKey, std::string>
  open(const ExternalKey &cellKey);

  /// The back gesture's target: the mount cell for @p key -- the cell
  /// itself when it is mounted, the mount that owns @p key when it is
  /// a delegate cell (a git:/log:/cfg: key), or the deepest mount whose
  /// path is an ancestor of an fs cell inside the mounted tree.
  /// nullopt when no mount is at or above @p key.
  [[nodiscard]] std::optional<ExternalKey>
  mountBoundary(const ExternalKey &key) const;

  /// Drop the mount at @p mountKey. The delegate's cells stale-mark on
  /// the next refresh(); nothing else is touched.
  [[nodiscard]] std::expected<void, std::string>
  unmount(const ExternalKey &mountKey);

  [[nodiscard]] bool isMounted(const ExternalKey &key) const;
  [[nodiscard]] std::size_t mountCount() const noexcept {
    return mounts_.size();
  }

  // -- ProjectionLens -------------------------------------------------------

  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override;
  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)> emit)
      const override;
  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)> emit)
      const override;
  /// Only the filesystem root: mounted roots hang under their cells and
  /// never join roots().
  bool forEachRoot(gleditor::cpp26::function_ref<void(const ExternalKey &)>
                       emit) const override;
  [[nodiscard]] std::vector<std::pair<std::string, std::string>>
  staleAttributes(const ExternalKey &key) const override;
  /// base's (never) OR every mount's -- a moved delegate source refreshes.
  [[nodiscard]] bool pollChanged() const override;
  /// The first mount's waitable descriptor, else -1.
  [[nodiscard]] std::optional<int> waitableFd() const noexcept override;
  /// Routed by ownership: a delegate's cell edits through its own lens.
  [[nodiscard]] std::expected<ExternalKey, std::string>
  applyContentChange(const ExternalKey &node,
                     std::string_view newContent) override;

  // -- WindowedLens (forwarded to the owning mount) --------------------------

  [[nodiscard]] std::expected<void, std::string>
  materializePage(const ExternalKey &windowKey) override;
  void discardMaterializedAfter(const ExternalKey &boundaryKey) override;
  /// The first mounted windowed lens's deferred page, if any.
  [[nodiscard]] std::optional<ExternalKey> nextStub() const override;

  /// Classify @p path and construct its delegate (browsing it so a bad
  /// mount refuses at open() time, not at the next refresh). Stateless
  /// -- the file picker's "open this path" uses it for the same answer.
  [[nodiscard]] static std::expected<std::unique_ptr<ProjectionLens>,
                                     std::string>
  delegateFor(const std::filesystem::path &path);

private:
  /// One mounted delegate under its fs cell.
  struct Mount {
    ExternalKey mountKey;                 ///< the fs cell the lens hangs under
    std::filesystem::path path;           ///< mountKey's on-disk path
    std::unique_ptr<ProjectionLens> lens; ///< the delegate
    std::vector<ExternalKey> roots;       ///< the delegate's declared roots

    /// Does this mount's delegate emit @p key? Delegate keys equal their
    /// root or extend it with ':' segments (git:/log:/cfg: all do).
    [[nodiscard]] bool owns(const ExternalKey &key) const;
  };

  FilesystemLens base_;
  std::vector<Mount> mounts_;
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_ROUTER_LENS_HPP
