/**
 * @file span_decorator.hpp
 * @brief Colouring ranges of a document that the library has no opinion about.
 *
 * The renderer already knows how to paint a background behind a byte range: it
 * is what a selection is. The machinery underneath -- quantising an edge to a
 * character boundary inside a ligature, splitting a range across the pages it
 * spans, packing the result into the uniform block the fragment stage reads --
 * is not specific to selections at all, but it was only ever reachable by
 * making one.
 *
 * A decorator is how a program says which other ranges matter and what colour
 * they should be. The library neither knows nor asks what the ranges mean.
 * Search hits, a diff, unsaved regions, whichever parts of a document came
 * from somewhere else -- all of them are a list of byte ranges and a colour,
 * and none of them needs the renderer to learn a new concept.
 */
#ifndef GLEDITOR_SPAN_DECORATOR_H
#define GLEDITOR_SPAN_DECORATOR_H

#include <cstdint>
#include <vector>

class Doc;

namespace gleditor {

/**
 * @brief One coloured range of a document's text.
 *
 * Offsets are document-global bytes, the same coordinates the caret and
 * picking use, and the range is half-open. A range whose end is not greater
 * than its start contributes nothing.
 */
struct SpanStyle {
  std::uint32_t start{};
  std::uint32_t end{};
  /// Packed RGBA8, most significant byte red -- the same encoding
  /// render::HighlightRange::colour takes.
  std::uint32_t colour{};
};

/**
 * @brief Asked, every frame, which ranges of a document to colour.
 *
 * Called on the render thread while the frame is being built. The document is
 * const because a decorator answering a question must not change the thing it
 * is being asked about.
 *
 * There is a hard limit on how many ranges reach the shader in one frame --
 * render::maxHighlightRanges, and a range crossing a page boundary costs one
 * per page it touches -- so a decorator over a large document should return
 * what is on screen rather than everything it knows. Ranges past the limit are
 * dropped, and the selection is added last so that it survives.
 */
class SpanDecorator {
public:
  SpanDecorator()          = default;
  virtual ~SpanDecorator() = default;

  SpanDecorator(const SpanDecorator &)            = delete;
  SpanDecorator &operator=(const SpanDecorator &) = delete;
  SpanDecorator(SpanDecorator &&)                 = delete;
  SpanDecorator &operator=(SpanDecorator &&)      = delete;

  /// Append the ranges to colour in @p doc. Called once per open document per
  /// frame; @p out is not cleared between documents.
  virtual void decorate(const Doc &doc, std::vector<SpanStyle> &out) = 0;
};

} // namespace gleditor

#endif // GLEDITOR_SPAN_DECORATOR_H
