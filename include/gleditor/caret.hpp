/**
 * @file caret.hpp
 * @brief The editing caret: where text will be inserted.
 *
 * The caret is a position in a document's text, held as a byte offset from the
 * start of that document rather than as a page and a coordinate. Pages are a
 * presentation of the text and reflow underneath it; an offset survives that,
 * a page-and-coordinate does not.
 *
 * Drawing goes through the same glyph pipeline everything else uses: one quad
 * whose foreground and background are the same colour, so the fragment stage's
 * blend between them ignores whatever the atlas holds. That is the same trick
 * the notification panel uses, and it is why a solid rectangle needs no second
 * shader and no blend state.
 */
#ifndef GLEDITOR_CARET_H
#define GLEDITOR_CARET_H

#include <cstdint>
#include <memory>

#include <glm/ext/matrix_float4x4.hpp>

#include <gleditor/buffer_pool.hpp>
#include <gleditor/render/types.hpp>

struct RenderState;

namespace render {
class RenderDevice;
}

/**
 * @class Caret
 * @brief Insertion point in one open document.
 */
class Caret {
public:
  /**
   * @brief Caret width in a page's layout pixels.
   *
   * Pages are drawn scaled down, so a one- or two-pixel caret in layout space
   * lands on well under one screen pixel and rasterises to nothing. This is
   * wide enough to survive that scale while still reading as a caret rather
   * than a block.
   */
  static constexpr float widthPixels = 6.0F;

  /**
   * @param aDevice Device the caret's vertex storage lives on. Not owned; must
   *        outlive the caret.
   */
  explicit Caret(render::RenderDevice *aDevice);
  ~Caret();

  /**
   * @brief Build the pipeline the caret draws with.
   *
   * Depth testing is off. The caret sits in the same plane as the text it
   * marks, close enough that which of the two wins is decided by rounding
   * rather than by intent -- the glyph backgrounds punched white holes through
   * it. An overlay does not belong in that contest: submitted after the
   * documents, it is simply on top.
   */
  void createPipeline(const render::PipelineDesc &documentDesc);

  Caret(const Caret &)            = delete;
  Caret &operator=(const Caret &) = delete;
  Caret(Caret &&)                 = delete;
  Caret &operator=(Caret &&)      = delete;

  /// Place the caret at a byte offset in a document, making it visible. This
  /// drops any selection: a plain click replaces one rather than extending it.
  void placeAt(std::uint32_t aDocIndex, std::uint32_t aByteOffset);

  /**
   * @brief Start a selection at the caret's position.
   *
   * The anchor is where a drag began and the caret is where it has reached, so
   * dragging backwards over the anchor selects in the other direction without
   * any special case.
   */
  void anchorSelection();

  /// Move the caret, keeping the anchor, which extends the selection.
  void extendTo(std::uint32_t aByteOffset);

  /// Forget the selection but keep the caret where it is.
  void clearSelection() { anchored = false; }

  [[nodiscard]] bool hasSelection() const {
    return anchored && anchor != byteOffset_;
  }
  /// Selected range as [start, end), ordered however the drag went.
  [[nodiscard]] std::uint32_t selectionStart() const {
    return anchor < byteOffset_ ? anchor : byteOffset_;
  }
  [[nodiscard]] std::uint32_t selectionEnd() const {
    return anchor < byteOffset_ ? byteOffset_ : anchor;
  }
  /// Hide the caret and drop any selection; the editor goes back to navigating
  /// rather than typing. This is what clicking on empty space does.
  void clear() {
    visible  = false;
    anchored = false;
  }

  [[nodiscard]] bool active() const { return visible; }
  [[nodiscard]] std::uint32_t documentIndex() const { return docIndex; }
  [[nodiscard]] std::uint32_t byteOffset() const { return byteOffset_; }

  /**
   * @brief Shift the caret to follow an edit made elsewhere in the text.
   *
   * An insertion before the caret moves the text under it; the caret has to
   * move with it or it would end up pointing at a different character than the
   * one the user left it on.
   */
  void shiftForInsertion(std::uint32_t at, std::uint32_t bytes);

  /**
   * @brief Shift the caret to follow a removal made elsewhere in the text.
   *
   * The mirror of shiftForInsertion(). A position after the removed range
   * moves back by its length; one inside it lands on the join, which is where
   * the text it pointed at used to begin. A selection that the removal
   * collapsed to nothing is dropped, since a zero-width selection is not one.
   */
  void shiftForErasure(std::uint32_t at, std::uint32_t bytes);

  /**
   * @brief Write the caret quad for a rectangle in a page's pixel space.
   *
   * Separate from draw() because the geometry only changes when the caret
   * moves or the page reflows, while drawing happens every frame.
   */
  void setGeometry(float posX, float posY, float height);

  /// Draw the caret. @p pageTransform is projection * view * page model, so
  /// the caret scales and moves with the page it sits on.
  void draw(RenderState &state, const glm::mat4 &pageTransform) const;

private:
  render::RenderDevice *device;
  std::unique_ptr<BufferPool> pool;
  BufferPool::Allocation backing{};
  render::PipelineHandle pipeline{};
  bool visible{};
  bool hasGeometry{};
  std::uint32_t docIndex{};
  std::uint32_t byteOffset_{};
  /// Where a selection began. The caret itself is the other end.
  std::uint32_t anchor{};
  bool anchored{};
};

#endif // GLEDITOR_CARET_H
