/**
 * @file edl_transform.hpp
 * @brief Composable Edit Decision List (EDL) transformation monoid.
 *
 * An EdlTransform represents a piecewise coordinate mapping from an input
 * concatext [0, inputLength) to an output concatext [0, outputLength).
 *
 * Slices are either:
 * - Source: mapped from the input concatext [sourceOffset, sourceOffset +
 * length).
 * - Primedia: newly introduced content span in the permascroll.
 * - Break: a zero-length page break marker.
 *
 * Transforms form a monoid under composition (T2 ∘ T1), enabling O(log K)
 * timeline scrubbing, fast diffing, and incremental version rebuilding.
 */
#ifndef XANADU_ENFILADE_EDL_TRANSFORM_HPP
#define XANADU_ENFILADE_EDL_TRANSFORM_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/version.hpp"

namespace xanadu::enfilade {

enum class SliceKind : std::uint8_t {
  Source, ///< References input concatext [sourceOffset, sourceOffset + length)
  Primedia, ///< References permascroll content span [scroll, start, length)
  Break     ///< Page break marker (length == 0)
};

struct EdlSlice {
  SliceKind kind{SliceKind::Source};
  std::uint32_t length{0};       ///< Length in concatext (0 for Break)
  std::uint32_t sourceOffset{0}; ///< Offset in input (when kind == Source)
  PrimediaSpan span{}; ///< Content span (when kind == Primedia or Break)

  [[nodiscard]] bool isBreak() const noexcept {
    return kind == SliceKind::Break;
  }
  [[nodiscard]] bool isSource() const noexcept {
    return kind == SliceKind::Source;
  }
  [[nodiscard]] bool isPrimedia() const noexcept {
    return kind == SliceKind::Primedia;
  }

  [[nodiscard]] EdlSlice subSlice(std::uint32_t offsetInSlice,
                                  std::uint32_t count) const;

  bool operator==(const EdlSlice &) const = default;
};

class EdlTransform {
public:
  EdlTransform() = default;
  explicit EdlTransform(std::uint32_t inputLength);

  /// Identity transform for an input concatext of length @p inputLength.
  [[nodiscard]] static EdlTransform identity(std::uint32_t inputLength);

  /// Construct a transform for a single operation applied to an input concatext
  /// of length @p inputLength.
  [[nodiscard]] static EdlTransform
  fromOp(const CompactOpNode &node, std::uint32_t inputLength,
         const std::vector<PrimediaSpan> &resolvedTranscludeSpans = {});

  /// Closed associative composition: returns (later ∘ earlier).
  /// earlier maps input -> intermediate.
  /// later maps intermediate -> output.
  /// Result maps input -> output.
  [[nodiscard]] static EdlTransform compose(const EdlTransform &earlier,
                                            const EdlTransform &later);

  // -- Mutation operations (matching Version's EDL operations) --
  void insert(std::uint32_t at, const PrimediaSpan &span);
  void insertSpans(std::uint32_t at, const std::vector<PrimediaSpan> &spans);
  void insertBreak(std::uint32_t at);
  std::vector<EdlSlice> remove(std::uint32_t at, std::uint32_t length);
  void rearrange(std::uint32_t at, std::uint32_t length, std::uint32_t to);
  void insertSlices(std::uint32_t at, const std::vector<EdlSlice> &slices);

  // -- Application and Materialization --
  [[nodiscard]] Version applyToVersion(const Version &base) const;
  [[nodiscard]] Version materializeState0() const;

  [[nodiscard]] std::uint32_t inputLength() const noexcept {
    return inputLength_;
  }
  [[nodiscard]] std::uint32_t outputLength() const noexcept {
    return outputLength_;
  }
  [[nodiscard]] const std::vector<EdlSlice> &slices() const noexcept {
    return slices_;
  }
  [[nodiscard]] bool empty() const noexcept { return slices_.empty(); }

  // WidthMonoid concept compliance:
  [[nodiscard]] bool isEmpty() const noexcept {
    return 0 == inputLength_ && 0 == outputLength_ && slices_.empty();
  }
  [[nodiscard]] EdlTransform combine(const EdlTransform &other) const {
    if (isEmpty()) {
      return other;
    }
    if (other.isEmpty()) {
      return *this;
    }
    return compose(*this, other);
  }

  bool operator==(const EdlTransform &) const = default;

private:
  std::size_t splitAt(std::uint32_t offset);
  void joinFollowing(std::size_t index);

  std::uint32_t inputLength_{0};
  std::uint32_t outputLength_{0};
  std::vector<EdlSlice> slices_;
};

} // namespace xanadu::enfilade

#endif // XANADU_ENFILADE_EDL_TRANSFORM_HPP
