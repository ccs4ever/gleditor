/**
 * @file meta_lens.hpp
 * @brief MetaLens: a store's own structure as a projection of
 *        references.
 *
 * design/lens-candidates.md section 3.3 -- introspection in the
 * system's own language, powered by project-by-reference (U-L5): every
 * enumerated key binds to the source cell itself rather than an
 * ephemeral twin, so VQL extkey/2 and annotations resolve against the
 * real structure.
 *
 * Two scope rules from the candidates doc:
 * - the source is a **frozen fold** (Store::rebuildManifold at a fixed
 *   version), re-folded per refresh -- a live manifold appends ops
 *   mid-enumeration;
 * - the session arena must **overlay** the source manifold, or base
 *   cells can't resolve as refs.
 */
#ifndef COMMON_XANADU_ZIGZAG_META_LENS_HPP
#define COMMON_XANADU_ZIGZAG_META_LENS_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/zigzag/projection_lens.hpp"

namespace xanadu {
class SpanReader;
} // namespace xanadu

namespace zigzag {

class Manifold;

/// A frozen manifold's cells and dimension census, projected by ref.
class MetaLens final : public ProjectionLens {
public:
  /// @p label names the key space (meta:<label>:...); @p source the
  /// frozen fold to enumerate; @p reader the SpanReader that renders
  /// its cells' text (the store).
  MetaLens(std::string label, const Manifold &source,
           const xanadu::SpanReader &reader);

  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override;
  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)> emit)
      const override;
  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)> emit)
      const override;
  bool forEachRoot(gleditor::cpp26::function_ref<void(const ExternalKey &)>
                       emit) const override;

private:
  std::string connKey_;              ///< meta:<label>
  const Manifold *source_;           ///< frozen fold (outlives the lens)
  const xanadu::SpanReader *reader_; ///< span reader for the fold
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_META_LENS_HPP
