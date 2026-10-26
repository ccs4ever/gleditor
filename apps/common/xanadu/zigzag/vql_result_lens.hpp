/**
 * @file vql_result_lens.hpp
 * @brief VqlResultLens: a VQL query's answer set as a slice.
 *
 * design/lens-candidates.md section 3.5 -- closes the loop: query the
 * docuverse, get a slice. Answer cells bind by project-by-reference
 * (U-L5): the key names the engine's answer cell itself, so the
 * projected result IS the result, not a copy. Answers that are store
 * cells keep durable identity; scratch-constructed answers are
 * arena-owned refs -- the honesty ladder's mechanism, at ord: strength
 * (position in this run's answer order).
 */
#ifndef COMMON_XANADU_ZIGZAG_VQL_RESULT_LENS_HPP
#define COMMON_XANADU_ZIGZAG_VQL_RESULT_LENS_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/zigzag/projection_lens.hpp"

namespace xanadu::vql {
class VQLEngine;
}

namespace zigzag {

/// One VQL evaluation's answers, projected by reference.
class VqlResultLens final : public ProjectionLens {
public:
  /// @p engine evaluates queries; its arena must be (or overlay) the
  /// session's, so answer refs resolve.
  explicit VqlResultLens(xanadu::vql::VQLEngine &engine);

  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override;
  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)> emit)
      const override;
  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)> emit)
      const override;
  bool forEachRoot(gleditor::cpp26::function_ref<void(const ExternalKey &)>
                       emit) const override;

  /// Evaluate @p vql and buffer the answers -- the browse entry. A
  /// later refresh() re-enumerates the buffered run; run() again to
  /// re-evaluate.
  [[nodiscard]] std::expected<void, std::string> run(std::string_view vql);

private:
  xanadu::vql::VQLEngine *engine_;
  std::string vql_;
  std::string connKey_;          ///< vq:<ordinal> query node
  std::vector<CellRef> answers_; ///< the last run's answers, in order
  std::size_t runs_{0};
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_VQL_RESULT_LENS_HPP
