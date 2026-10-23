/**
 * @file git_lens.hpp
 * @brief GitLens: a repository's refs and commit DAG as a windowed
 *        projection.
 *
 * design/lens-candidates.md section 3.1: git is already an append-only
 * DAG with content-addressed identity, so commits carry pk-strength
 * keys for free (git:<repo>:c:<sha>) and the log's only mutable half is
 * the refs rank, where refresh()'s union identity is exactly what makes
 * ref motion safe. The log is windowed -- page 0 is HEAD's newest
 * commits, older pages materialize from stubs.
 *
 * Backend is `git` plumbing (rev-list/for-each-ref via popen), not
 * libgit2 -- read-only round one needs no linking.
 */
#ifndef COMMON_XANADU_ZIGZAG_GIT_LENS_HPP
#define COMMON_XANADU_ZIGZAG_GIT_LENS_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

/// One commit as plumbing reports it.
struct GitCommit {
  std::string sha;
  std::string parents; ///< space-joined parent SHAs
  std::string author;
  std::string date; ///< ISO-8601
  std::string subject;
};

/// A git repository as a windowed enumeration projection.
class GitLens final : public ProjectionLens, public WindowedLens {
public:
  /// @p repoPath names a git worktree (or a bare dir); @p pageSize
  /// bounds each log window's commits.
  explicit GitLens(std::string repoPath, std::size_t pageSize = 64);

  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override;
  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)> emit)
      const override;
  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)> emit)
      const override;
  bool forEachRoot(gleditor::cpp26::function_ref<void(const ExternalKey &)>
                       emit) const override;
  [[nodiscard]] std::vector<std::pair<std::string, std::string>>
  staleAttributes(const ExternalKey &key) const override;

  /// Enumerate refs + the log shape and materialize page 0 (HEAD's
  /// newest commits). The browse entry, SqlLens-style.
  [[nodiscard]] std::expected<void, std::string> browse();

  [[nodiscard]] std::expected<void, std::string>
  materializePage(const ExternalKey &windowKey) override;
  void discardMaterializedAfter(const ExternalKey &boundaryKey) override;

private:
  struct LogPage {
    ExternalKey key;     ///< git:<repo>:p:<skip>
    std::size_t skip{0}; ///< rev-list offset
    std::size_t count{0};
    bool stub{true};
    std::vector<std::string> shas; ///< commit order in the window
  };
  struct GitRef {
    std::string name;   ///< refs/heads/main
    std::string target; ///< sha
  };

  /// `git -C repo <args>` -> stdout, or the error.
  [[nodiscard]] std::expected<std::string, std::string>
  run(std::string_view args) const;
  /// rev-list HEAD's commits [skip, skip+count) with their metadata.
  [[nodiscard]] std::expected<std::vector<GitCommit>, std::string>
  logPage(std::size_t skip, std::size_t count) const;

  std::string repo_;
  std::size_t pageSize_;
  std::string connKey_; ///< git:<repo>
  mutable std::vector<LogPage> pages_;
  mutable std::vector<GitRef> refs_;
  mutable std::vector<GitCommit> commits_; ///< materialized, keyed by sha
  mutable std::uint64_t total_{0};
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_GIT_LENS_HPP
