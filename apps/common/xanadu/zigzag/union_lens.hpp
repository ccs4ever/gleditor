/**
 * @file union_lens.hpp
 * @brief UnionLens: several lenses hosted in one session (U-L1).
 *
 * design/lens-candidates.md's remaining gap: one ProjectionLens per
 * session can't host a mixed view. A composite lens that delegates
 * every verb to its children in order gives the documented union rule
 * -- one refresh() enumerates all children in ONE epoch (atomic
 * refresh), and a key two children emit is one cell (the session's
 * identity map dedupes; the child's attrs win first-come).
 *
 * Deliberate same-key emission across children is the unification
 * mechanism (a MetaLens census key binding a cell a SqlLens projects);
 * accidental collisions share the same rule.
 */
#ifndef COMMON_XANADU_ZIGZAG_UNION_LENS_HPP
#define COMMON_XANADU_ZIGZAG_UNION_LENS_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

/// A ProjectionLens composed of children, in priority order.
class UnionLens final : public ProjectionLens {
public:
  void add(std::unique_ptr<ProjectionLens> lens) {
    children_.push_back(std::move(lens));
  }

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
  [[nodiscard]] bool pollChanged() const override;
  /// First child that takes the edit wins; a refusal tries the next
  /// (a lens that doesn't own the key refuses by contract).
  [[nodiscard]] std::expected<ExternalKey, std::string>
  applyContentChange(const ExternalKey &node,
                     std::string_view newContent) override;

  /// A child by index -- callers driving child-only verbs (browse(),
  /// materializePage()) reach the concrete type this way.
  [[nodiscard]] ProjectionLens *child(std::size_t i) const {
    return children_.at(i).get();
  }
  [[nodiscard]] std::size_t size() const noexcept { return children_.size(); }

private:
  std::vector<std::unique_ptr<ProjectionLens>> children_;
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_UNION_LENS_HPP
