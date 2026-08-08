/**
 * @file session.hpp
 * @brief What holds a xanadoc open on screen.
 *
 * The library draws documents and reports what happens to them. The store
 * keeps content, operations and links, and knows nothing about drawing. This
 * is the piece between them, and it is deliberately the only piece that knows
 * both: everything Xanadu-shaped lives on this side of the line, and the
 * library is reached only through the hooks it publishes -- a TextSource, a
 * DocumentObserver, a SpanDecorator, a FrameContributor.
 *
 * There is nothing in the library that had to learn what a transclusion is.
 */
#ifndef XUDU_SESSION_H
#define XUDU_SESSION_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gleditor/canvas.hpp>
#include <gleditor/document_observer.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/span_decorator.hpp>
#include <gleditor/text_source.hpp>

#include "core/microversion.hpp"
#include "core/store.hpp"

class Caret;
class Doc;

namespace xudu {

class Session;

/**
 * @brief The text of one microversion, rebuilt on demand.
 *
 * OSMIC's "versioning on demand" reaching the screen: what the library is
 * handed is not a stored document but the result of replaying the operations
 * the version's name spells out. Held by shared pointer through the render
 * queue, and consulted on a loader thread, which is why it takes a copy of the
 * text rather than a reference to the store.
 */
class VersionTextSource : public gleditor::TextSource {
public:
  VersionTextSource(std::string aText, MicroversionId aVersion)
      : contents(std::move(aText)), id(std::move(aVersion)) {}

  [[nodiscard]] std::string text() const override { return contents; }
  [[nodiscard]] std::string name() const override { return id.str(); }
  [[nodiscard]] const MicroversionId &version() const { return id; }

private:
  std::string contents;
  MicroversionId id;
};

/**
 * @brief One open document: which version it shows, and where it sits.
 *
 * The library indexes open documents by position, and the picking tags carry
 * that index, so this is kept in the same order.
 */
struct OpenView {
  MicroversionId version;
  /// The version rebuilt, kept so that decorating does not replay the whole
  /// history once per frame per document.
  Version pieces;
};

/**
 * @class Session
 * @brief A store, the views onto it, and the hooks that keep the two in step.
 */
class Session : public gleditor::DocumentObserver,
                public gleditor::SpanDecorator {
public:
  /// Colours the decorator paints with. Backgrounds behind text, so they are
  /// pale enough to read through.
  static constexpr std::uint32_t transclusionColour = 0xFFE9A8FFU;
  static constexpr std::uint32_t linkColour         = 0xB9E8C4FFU;

  explicit Session(std::string aStorePath);

  [[nodiscard]] Store &store() { return docStore; }
  [[nodiscard]] const Store &store() const { return docStore; }

  /// Where the store is kept, and writing it there.
  [[nodiscard]] const std::string &path() const { return storePath; }
  void save() const { docStore.save(storePath); }

  /// The version each open document shows, in the library's document order.
  [[nodiscard]] const std::vector<OpenView> &views() const { return open; }
  [[nodiscard]] MicroversionId versionOf(std::uint32_t docIndex) const;

  /// Note that a document showing @p version has been opened, or that one has
  /// gone. Called from the render thread as documents come and go.
  void viewOpened(const MicroversionId &version);
  void viewClosed(std::uint32_t docIndex);
  /// Point an already-open document at a different version.
  void viewMovedTo(std::uint32_t docIndex, const MicroversionId &version);

  /// A source for @p version, ready to hand to the render queue.
  [[nodiscard]] std::shared_ptr<VersionTextSource>
  sourceFor(const MicroversionId &version) const;

  // -- gleditor::DocumentObserver -------------------------------------------
  //
  // Every edit the library applies becomes a hyperop against the version that
  // document is showing, and the document moves to the state the op produced.
  // Typing is therefore how hypertime is extended; there is no separate
  // "commit".
  void textInserted(Doc &doc, std::uint32_t at,
                    const std::string &utf8) override;
  void textErased(Doc &doc, std::uint32_t at,
                  const std::string &removed) override;

  // -- gleditor::SpanDecorator ----------------------------------------------
  //
  // Shades the passages this document shares with another open one, and the
  // passages a link is attached to. Both are computed from primedia addresses,
  // so they are what the store knows rather than a text search.
  void decorate(const Doc &doc, std::vector<gleditor::SpanStyle> &out) override;

private:
  /// Rebuild the cached version for @p docIndex after an edit moved it.
  void refresh(std::uint32_t docIndex, const MicroversionId &version);

  Store docStore;
  std::string storePath;
  std::vector<OpenView> open;

public:
  /// Forget every open view. Used when the program replaces what is on screen
  /// wholesale, which is how travelling to another state is done.
  void clearViews() { open.clear(); }
};

/**
 * @brief The hypertime map: every state of the document, and how they connect.
 *
 * Drawn as a frame contributor over the documents, in window pixels. Nelson's
 * prototype had one too, and it is the part of OSMIC that cannot be explained
 * without a picture: a graph where nothing is ever lost, as against the single
 * line an undo stack offers.
 */
class HypertimeMap : public gleditor::FrameContributor {
public:
  HypertimeMap(std::string aFontName, const Session &aSession);

  /// The canvas is built here rather than in the constructor, because the
  /// device does not exist until the render thread has started.
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;

  /// Whether the map is shown at all.
  void setVisible(const bool shown) { visible = shown; }
  [[nodiscard]] bool isVisible() const { return visible; }
  void toggle() { visible = !visible; }

  /// Which state to mark as where the reader is.
  void setCurrent(MicroversionId id) { current = std::move(id); }

  void drawFrame(gleditor::FrameContext &ctx) override;

private:
  /// Pixel geometry of the map. A node is a labelled box; a generation is a
  /// column, so time runs left to right and branches stack downwards.
  static constexpr float nodeWidth   = 74.0F;
  static constexpr float nodeHeight  = 26.0F;
  static constexpr float columnGap   = 34.0F;
  static constexpr float rowGap      = 10.0F;
  static constexpr float mapMargin   = 16.0F;
  static constexpr float padding     = 10.0F;

  std::string fontName;
  std::unique_ptr<gleditor::Canvas> canvas;
  const Session &session;
  MicroversionId current;
  bool visible{};
  /// What the map showed when it was last built, so that a frame in which
  /// nothing changed does not rebuild and re-upload it.
  std::size_t builtForOps{static_cast<std::size_t>(-1)};
  MicroversionId builtForCurrent;
  bool builtForVisible{};
  int builtForHeight{};
};

} // namespace xudu

#endif // XUDU_SESSION_H
// vi: set sw=2 sts=2 ts=2 et:
