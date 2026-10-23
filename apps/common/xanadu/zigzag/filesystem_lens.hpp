/**
 * @file filesystem_lens.hpp
 * @brief A ProjectionLens over a directory tree, with file metadata
 *        (design/anything-to-zigzag-lens.md §7). The second lens, built to
 *        prove the trait is not xanadoc-shaped.
 *
 * Projects a directory rooted at a path: each entry is a node whose content is
 * its name and whose attributes carry role (file/dir/symlink), MIME, size,
 * mtime and permissions; a directory->child edge is d.contains, sibling order
 * is d.seq, and a symlink->target edge is d.ref. The stable ExternalKey is the
 * (device, inode) pair so a rename within a device preserves identity (§5.2);
 * a no-inode path falls back to a path hash.
 *
 * Projection is read-only: nothing on disk is touched. The arena is a live view
 * of the directory.
 */
#ifndef ZIGZAG_FILESYSTEM_LENS_HPP
#define ZIGZAG_FILESYSTEM_LENS_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

class FilesystemLens final : public ProjectionLens {
public:
  explicit FilesystemLens(std::filesystem::path root);

  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override;

  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)> emit)
      const override;

  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)> emit)
      const override;

  // -- cell -> source lookups (the RouterLens's hops) -------------------------

  /// The on-disk path a projected key names. The fs:<dev>:<ino> key
  /// carries no path, so a router that mounts delegate lenses by file
  /// type needs the walk's own record. nullopt for a foreign key.
  [[nodiscard]] std::optional<std::filesystem::path>
  pathFor(const ExternalKey &key) const;

  /// @p parent's children in sibling order -- the child run a router
  /// splices a mounted delegate's roots into. Empty for a file or a
  /// childless dir.
  [[nodiscard]] std::vector<ExternalKey>
  childKeys(const ExternalKey &parent) const;

  /// The walked root's key; nullopt when the root was unreadable.
  [[nodiscard]] std::optional<ExternalKey> rootKey() const;

private:
  // One walked entry, captured once so forEachNode and forEachEdge agree.
  struct Entry {
    ExternalKey key;
    std::filesystem::path path; ///< kept for pathFor -- keys carry no path
    std::string name;
    std::string role; ///< "dir" | "file" | "symlink"
    std::string mime;
    std::int64_t size{0};
    std::int64_t mtime{0};
    std::int64_t perm{0};
    std::optional<ExternalKey> parent;    ///< containing dir's key
    std::optional<ExternalKey> symTarget; ///< d.ref target, if a symlink
    std::uint64_t order{0};               ///< sibling order within its parent
  };

  std::filesystem::path root_;
  std::vector<Entry> entries_; ///< sorted-DFS order; filled by the ctor

  void walk();
};

} // namespace zigzag

#endif // ZIGZAG_FILESYSTEM_LENS_HPP
