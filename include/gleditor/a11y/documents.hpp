/**
 * @file documents.hpp
 * @brief Saying what the open documents are, and where the caret is in them.
 *
 * This is the part of the accessibility tree that matters most and is least
 * like a widget. A form field has a name and a value and that is the whole of
 * it; a document is text somebody is reading and editing, and an assistive
 * technology navigating it wants to move by character, by word and by line,
 * ask what is selected, and be told when the caret moves.
 *
 * AccessKit's answer to that is text runs: a node per line holding the line's
 * text, the byte length of each of its characters, and the character index
 * each of its words begins at. The document node above them carries the
 * selection, as a pair of positions naming a run and an offset into it. That
 * is what this builds -- see a11y::runBreaks() for what counts as a line and
 * why a very long one is cut.
 *
 * It costs a copy of every open document's text whenever the text changes,
 * which is why Doc::editGeneration() exists: without it the only way to know
 * whether the description was stale would be to compare the description
 * against the document, every frame.
 */
#ifndef GLEDITOR_A11Y_DOCUMENTS_H
#define GLEDITOR_A11Y_DOCUMENTS_H

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>

#include <gleditor/a11y/tree.hpp>

class Caret;
struct RenderState;

namespace gleditor::a11y {

/**
 * @class DocumentsSource
 * @brief The open documents, as an assistive technology sees them.
 *
 * Owned by the renderer, which is what has the documents. observe() and
 * describe() are both called on the render thread, in that order, once a
 * frame; performAction() is called on the event thread, so what it does is
 * queued rather than done.
 */
class DocumentsSource : public Source {
public:
  /**
   * @brief Look at what is on screen now.
   *
   * Cheap when nothing has changed: the documents are asked for their edit
   * generation rather than their text, and the text is only copied when one of
   * them has moved.
   *
   * @param viewProjection The frame's camera, for turning a page's corners
   *        into a rectangle in window pixels.
   * @param width,height The drawing area, which is the space those pixels are
   *        in.
   */
  void observe(const RenderState &state, const Caret *caret,
               const glm::mat4 &viewProjection, int width, int height);

  void describe(Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override;
  bool performAction(std::uint64_t nodeId, Action action,
                     std::string_view value) override;

  /**
   * @brief Something an assistive technology asked for, waiting to be done.
   *
   * The requests arrive on the event thread and the documents belong to the
   * render thread, so they are queued rather than carried out. The renderer
   * drains this inside its own loop.
   */
  struct Wanted {
    enum class Kind : std::uint8_t {
      /// Put the caret in this document, at this byte offset.
      PlaceCaret,
    };
    Kind kind{Kind::PlaceCaret};
    std::uint32_t document{};
    std::uint32_t byteOffset{};
  };

  /**
   * @brief Take everything asked for since the last call. Render thread.
   *
   * The requests are resolved here rather than where they arrived, because
   * turning "the fourth line of the second document" into a byte offset means
   * reading the description -- and the description belongs to this thread.
   */
  [[nodiscard]] std::vector<Wanted> takeWanted();

private:
  /// One open document, as of the last observe().
  struct Described {
    std::uint32_t index{};
    std::string name;
    std::string text;
    /// Where each run of @ref text starts, and finally its size. See
    /// a11y::runBreaks().
    std::vector<std::size_t> breaks;
    /// Where the document is on screen, or nothing when no page of it could be
    /// projected -- which is what an empty document, or one entirely behind
    /// the camera, comes to.
    std::optional<Rect> bounds;
    /// Whether the caret is in this document, and where.
    bool hasCaret{};
    std::uint32_t caretByte{};
    std::uint32_t selectionStart{};
    std::uint32_t selectionEnd{};
    bool hasSelection{};
  };

  /// The run holding a byte offset, and how many characters into it the offset
  /// falls. The last run when the offset is past the end, which is where a
  /// caret at the end of the document belongs.
  [[nodiscard]] static TextPoint pointAt(const Described &doc,
                                         std::uint64_t firstRunId,
                                         std::uint32_t byteOffset);

  std::vector<Described> docs;
  std::uint64_t revision{1};
  /// What the last revision was computed from, so an unchanged frame is one
  /// comparison and no copying.
  std::uint64_t lastSignature{};
  bool everObserved{};

  /// A node an assistive technology asked something of, as its numbering here:
  /// which document, and which of its runs -- or none, for the document node
  /// itself.
  struct Asked {
    std::uint32_t document{};
    std::optional<std::size_t> run;
  };
  /// Filled on the event thread, drained on the render thread.
  mutable std::mutex wantedGuard;
  std::vector<Asked> wanted;
};

} // namespace gleditor::a11y

#endif // GLEDITOR_A11Y_DOCUMENTS_H
