#ifndef GLEDITOR_DOC_H
#define GLEDITOR_DOC_H

#include <array>
#include <cassert>
#include <choreograph/Choreograph.h>
#include <cstdint>
#include <gleditor/buffer_pool.hpp>
#include <gleditor/drawable.hpp>
#include <gleditor/renderer.hpp>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <glm/ext/matrix_float4x4.hpp>
#include <deque>
#include <memory>
#include <optional>
#include <pangomm/layout.h>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/draw_budget.hpp>
#include <gleditor/render/types.hpp>

class Caret;
class Doc;
struct RenderState;

namespace render {
class RenderDevice;
}

namespace gleditor {
class DocumentObserver;
class TextSource;
} // namespace gleditor

/**
 * @brief One shaped cluster of the page's text.
 *
 * A cluster is Pango's unit of indivisible shaping: a ligature such as "ffi",
 * a base letter with its combining marks, or an emoji sequence is one cluster
 * drawn as one quad, while covering several characters of the text. Hit
 * testing has to know both, which is why the byte range and the character
 * count are kept rather than assuming one quad is one character.
 */
struct ClusterBox {
  /// Byte offset of the cluster within the page's own text.
  std::uint32_t byteStart{};
  /// Length of the cluster in bytes.
  std::uint32_t byteLength{};
  /// Characters the cluster covers. Greater than one for a ligature, which is
  /// what makes a click inside the quad ambiguous without interpolation.
  std::uint32_t charCount{};
};

class Page : public Drawable {
private:
  std::shared_ptr<Doc> doc;
  BufferPool::Allocation pageBacking{};
  /**
   * @brief Rows of the full-detail draw: the page background then one per
   *        glyph.
   *
   * The allocation holds the coarse draw's rows straight after these, so both
   * draws are a contiguous run and either can be aimed at with nothing but a
   * byte offset and a count.
   */
  std::uint32_t detailInstances{};
  /// Rows of the coarse draw: the page background again, then one solid bar
  /// per line of text. Zero when the page has no lines worth drawing.
  std::uint32_t coarseInstances{};
  /// Page size in layout pixels, which is the space the vertex positions are
  /// in. Kept for the frustum test.
  float pageWidth{};
  float pageHeight{};
  /**
   * @brief This page's shaping, kept only while something needs it.
   *
   * A layout is what Pango shaped the page's text into, and it is large: on a
   * megabyte of text the layouts of every page came to ninety megabytes,
   * more than the vertex buffer they produced. Once a page's quads are in that
   * buffer the layout has done its job, and a reader who never puts a cursor
   * in the document never needs it again.
   *
   * So it is released when the page is built and shaped again on demand -- by
   * the same call on the same text, so it comes back identical. Only placing a
   * caret and reflowing an edit need one, and both are things a person does to
   * one page at a time.
   */
  mutable Glib::RefPtr<Pango::Layout> layout;
  /// Byte offset of this page's text within the whole document, so a picking
  /// result can name a position in the document rather than in the page.
  std::uint32_t textOffset{};
  /// Every cluster on the page, in text order.
  std::vector<ClusterBox> clusters;
  /// This page's position in its document, carried in the picking tag.
  std::uint32_t pageIndex{};
  /// The picking identity every quad of this page shares -- its document and
  /// page, with no kind, since that is the part each quad supplies. Passed to
  /// the draw rather than written into a million instances.
  std::uint32_t identity{};
  /// Bytes of document text this page lays out.
  std::uint32_t textBytes{};
  /// Offset applied to layout coordinates so the page is centred on its own
  /// origin; kept so caret geometry lands in the same space as the glyphs.
  float originX{};
  float originY{};

public:
  Page(std::shared_ptr<Doc> aDoc, RenderState &state, glm::mat4 &model,
       Glib::RefPtr<Pango::Layout> aLayout, std::uint32_t aTextOffset,
       std::uint32_t aPageIndex);
  /**
   * @brief Append this page's draw to @p batches, or decide it needs none.
   * @param docTransform projection * view * document model.
   *
   * Collected rather than issued so that the whole frame's page draws reach the
   * device in one call, which is what a backend needs in order to record them
   * on more than one thread. Two decisions are made here rather than by the
   * device, because both need to know what the page is rather than what the
   * draw is: a page entirely outside the view contributes nothing and is
   * skipped, and a page too small on screen for its glyphs to be legible is
   * drawn as one solid bar per line instead.
   */
  void collect(std::vector<render::GlyphBatch> &batches,
               const glm::mat4 &docTransform, float opacity,
               const DrawBudget &budget, DrawStats &stats) const;

  [[nodiscard]] std::uint32_t baseOffset() const { return textOffset; }
  [[nodiscard]] const std::vector<ClusterBox> &clusterBoxes() const {
    return clusters;
  }
  /**
   * @brief This page's shaping, produced again if it is not being kept.
   *
   * Returned by value rather than by reference: the document keeps only a few
   * of these at a time, and asking for one may be what pushes another out.
   */
  [[nodiscard]] Glib::RefPtr<Pango::Layout> ensureLayout() const;
  /// Let go of the shaping. Whatever needs it next asks for it again.
  void dropLayout() const { layout.reset(); }
  [[nodiscard]] const BufferPool::Allocation &allocation() const {
    return pageBacking;
  }
  /// Move this page's text offset without touching its shaping or its rows,
  /// which an edit earlier in the document leaves byte-identical. Negative for
  /// an edit that removed text.
  void shiftBaseOffset(const std::int32_t bytes) {
    textOffset = static_cast<std::uint32_t>(static_cast<std::int64_t>(textOffset) +
                                            bytes);
  }
  /// Bytes of document text this page lays out.
  [[nodiscard]] std::uint32_t textLength() const { return textBytes; }
  /// True when a document-global byte offset falls within this page's text.
  [[nodiscard]] bool contains(std::uint32_t globalOffset) const;

  /**
   * @brief Where a caret sits for a document-global byte offset.
   *
   * Answered by Pango rather than derived from the cluster table: it is the
   * same call an editor would make to place a cursor, and it already knows
   * about right-to-left runs and about offsets that fall inside a cluster.
   *
   * @param[out] posX,posY Centre of the caret in this page's pixel space.
   * @param[out] height Caret height in the same space.
   * @return false when the offset is not on this page.
   */
  [[nodiscard]] bool caretGeometry(std::uint32_t globalOffset, float &posX,
                                   float &posY, float &height) const;

  /// This page's own text, as a view into the document's. The offsets the
  /// cluster table carries are relative to its start, and asking the document
  /// rather than the layout is what lets the layout be let go of.
  [[nodiscard]] std::string_view pageText() const;

  /**
   * @brief Resolve a picked cluster and fractional position to a byte offset.
   *
   * A cluster covering several characters -- a ligature -- is subdivided
   * evenly by the fraction, which is how Pango itself turns an x coordinate
   * into an index, so a click lands on the boundary a user would expect.
   *
   * @return The document-global byte offset, or nullopt for an unknown
   *         cluster.
   */
  [[nodiscard]] std::optional<std::uint32_t>
  offsetForCluster(std::uint32_t clusterIndex, float fraction) const;

  /**
   * @brief The highlight span covering a byte range on this page.
   *
   * The edges are quantised to character boundaries within the clusters they
   * fall in: a selection ending between the "f" and the "i" of an "fi"
   * ligature yields a fraction of one half of that one quad, not a whole
   * cluster and not nothing. Single-character clusters come out as 0 or 1,
   * which is the same rule with nothing to divide.
   *
   * @return nullopt when the range does not reach this page.
   */
  [[nodiscard]] std::optional<render::HighlightRange>
  highlightFor(std::uint32_t selStart, std::uint32_t selEnd,
               std::uint32_t colour) const;
  ~Page() override = default;
};

/**
 * @brief How much had to be laid out again after an edit.
 *
 * Reported so that the fast paths are observable rather than merely claimed:
 * a change that quietly stopped taking them would still look correct.
 */
enum class ReflowScope : std::uint8_t {
  Line,     ///< The edit stayed within one line: no line break moved.
  Page,     ///< Line breaks moved, but the page still ends where it did.
  Document, ///< The page spilled, so pagination changed after it.
};

const char *reflowScopeName(ReflowScope scope);

class Doc : public Drawable, public std::enable_shared_from_this<Doc> {
private:
  std::vector<Page> pages;
  /**
   * @brief Pages whose shaping is currently being kept, oldest first.
   *
   * A page gives up its layout as soon as its quads exist, and shapes again
   * when a caret or an edit needs it. Without a bound the pages a person
   * visits would accumulate layouts until the document held them all again,
   * which is the cost this exists to avoid; with one, a long session keeps as
   * many as a person can be working on at once.
   */
  mutable std::deque<std::uint32_t> liveLayouts;
  /// Kept because a reflow shapes the edited page and the pages after it, and
  /// a caret sits in one page while an edit rebuilds another.
  static constexpr std::size_t maxLiveLayouts = 4;
  /// What this document is called: a path for one opened from disk, whatever
  /// the source said otherwise.
  std::string docName;
  Glib::ustring text;
  /// Told about every edit. Bare pointers, not owned; see DocumentObserver.
  std::vector<gleditor::DocumentObserver *> observers;
  RendererRef renderer;
  /// Vertex storage shared by every page of this document.
  std::unique_ptr<BufferPool> pool;
  /// Position among the open documents; see setDocIndex().
  std::uint32_t docIndex{};
  /// Outcome of the most recent reflow, for reporting and for tests.
  ReflowScope reflowScope{ReflowScope::Document};
  std::size_t reflowPages{};
  /**
   * @brief Where the document actually is, as opposed to where it belongs.
   *
   * Animated, so during an arrival it lags the resting place the constructor
   * was given. Everything that needs the document's transform reads
   * modelMatrix(), which is built from this rather than from the base class's
   * matrix -- that one records the destination and does not move.
   */
  ch::Output<glm::vec3> position;
  /// Alpha the whole document is drawn at. Zero until it has arrived.
  ch::Output<float> opacity{1.0F};
  /// Whether a departure has been started, so the owner knows this document is
  /// on its way out rather than merely transparent for a moment.
  bool closing{};

  /// Build a page layout for text starting at @p offset, with the page
  /// geometry makePages() uses. Safe to call off the render thread.
  [[nodiscard]] Glib::RefPtr<Pango::Layout>
  layoutFrom(std::uint32_t offset) const;

  /// Record that page @p pageIndex has shaped itself again, and let go of the
  /// least recently shaped page once more than a few are being kept.
  void keepLayoutOf(std::uint32_t pageIndex) const;
public:
  /**
   * @brief Bytes of document text a finished page layout consumes.
   *
   * Public alongside fillPage() below, and for the same reason: between them
   * these two decide where one page ends and the next begins, they are pure
   * functions of a layout, and the property that matters -- that bounding the
   * text a layout is given does not move that boundary -- can only be checked
   * by asking both.
   */
  [[nodiscard]] static std::uint32_t
  consumedBytes(const Glib::RefPtr<Pango::Layout> &layout);

  /**
   * @brief Give @p layout the least text that still fills the page.
   *
   * A page cannot show more than its height allows, so text past that point
   * changes nothing about what the page holds -- but Pango does not know it is
   * unwanted, and breaking lines through it is what asking a layout anything
   * pays for. Handed a whole document, that made building pages cost time
   * proportional to the document once per page.
   *
   * How much a page holds depends on the font and the geometry, so it cannot
   * be a constant. The slice starts small and grows until the layout
   * ellipsizes, which is Pango saying it ran out of room -- at which point
   * more text provably cannot change what the page shows. A slice that reaches
   * the end of the text is likewise complete. So the page ends up holding
   * exactly what it would have held given everything.
   *
   * Each slice ends on a character boundary, since cutting through one would
   * hand Pango invalid UTF-8 that was never in the document.
   *
   * Public because it is worth testing directly: handed a document with no
   * line breaks in it, Pango stops wrapping past a certain length and lays the
   * whole thing out as one enormously wide line, which this avoids.
   *
   * @param from Start of the remaining text, which need not be NUL-terminated
   *        at the point the page ends.
   * @param remaining Bytes available from @p from.
   */
  static void fillPage(const Glib::RefPtr<Pango::Layout> &layout,
                       const char *from, std::size_t remaining);

private:
  /// Byte offsets at which each line of a layout starts.
  [[nodiscard]] static std::vector<int>
  lineStarts(const Glib::RefPtr<Pango::Layout> &layout);
  /**
   * @brief Rebuild the pages an edit disturbed. Render thread only.
   * @param delta Bytes the document grew by, negative for a removal. Every
   *        offset after the edit moves by exactly this, which is what lets the
   *        pages past the damage keep their shaping and only renumber.
   */
  void reflowFrom(RenderState &state, std::size_t firstPage, std::uint32_t at,
                  std::int32_t delta, const std::vector<int> &oldStarts,
                  std::uint32_t oldConsumed);
  /// Shared tail of insert() and erase(): find the damaged page, record what
  /// its lines looked like, and schedule the reflow.
  void scheduleReflow(RenderState &state, std::uint32_t at, std::int32_t delta);
  // token to keep anything other than Doc::create from using our constructor
  struct Private {
    explicit Private() = default;
  };

public:
  /**
   * @brief One quad, which is one glyph or one solid rectangle.
   *
   * Twenty-four bytes, and every one of them is packed: a megabyte of text is
   * a little over a million of these, so the record's size is the largest
   * single cost the renderer has. Four bytes per instance is fifty megabytes
   * across a document, which is why fields that could be derived are derived
   * and fields that need a byte are given bits.
   *
   * What is *not* here is as important as what is. There is no per-vertex data
   * at all -- the quad's four corners come from the vertex index -- and no
   * texture extent, because a glyph's box in the atlas is the same rectangle
   * as its box on the page and is read out of @ref quad. Nor is there a
   * picking identity: the document and page a quad belongs to are the same for
   * every instance of a draw, so they arrive in render::DrawUniforms and only
   * the kind, which does vary within a page's draw, is carried here.
   *
   * Field order and offsets must stay in step with Doc::vertexLayout() and
   * with the attribute locations in assets/shaders/glyph.vert.glsl.
   */
  struct VBORow {
    /// Centre of the quad. Two components: depth is one of a few fixed steps
    /// and rides in @ref foreground rather than spending a float.
    std::array<float, 2> pos;
    /// Ink colour, r:8 g:8 b:8, and flags:8. See @ref ink.
    unsigned int foreground;
    /// Atlas origin of the glyph in texels, x:16 y:16. See @ref atlasAt.
    unsigned int atlas;
    /// width:12 height:12 layer:6 kind:2. See @ref box.
    unsigned int quad;
    /// Paper colour as RGB565:16, then cluster:16. See @ref paperAt.
    unsigned int paper;

    /**
     * @brief Quad size the packing can name, in layout pixels.
     *
     * Twelve bits each. The largest quad drawn is a page background of around
     * sixteen hundred layout pixels, so this leaves it more than twice the
     * room it needs, and the two bits saved against the old thirteen are what
     * the kind field is written in.
     */
    static constexpr unsigned int maxQuadExtent = 4095;
    /// Atlas layers the packing can name; the hardware allows far more.
    static constexpr unsigned int maxAtlasLayers = 64;
    /**
     * @brief Clusters one page may hold, which the cluster field bounds.
     *
     * Sixteen bits. A page of the default font holds around forty-five
     * hundred, so this is fourteen times a real page; a page that would run
     * past it stops early and the next one carries on, which costs a short
     * page at absurd font sizes rather than a mis-resolved click.
     */
    static constexpr unsigned int maxClustersPerPage = 65535;

    /**
     * @brief Depth of a quad, as a step rather than a coordinate.
     *
     * Only a few depths are ever used -- paper, the text on it, and the caret
     * over that -- and they exist to break ties in the depth test rather than
     * to be seen, so two bits say which one. Must match the same constant in
     * assets/shaders/glyph.vert.glsl.
     */
    static constexpr float depthStep    = 0.1F;
    static constexpr unsigned int onPaper = 0; ///< The page itself.
    static constexpr unsigned int onText  = 1; ///< Glyphs and bars.
    static constexpr unsigned int onTop   = 2; ///< The caret.
    static constexpr unsigned int maxDepthStep = 3;

    /// Flag bits of @ref foreground's low byte.
    static constexpr unsigned int depthMask = 0x3;
    /// A quad with no glyph on it: filled with @ref foreground and never
    /// sampled from the atlas, which is both what a background is and one
    /// texture fetch per fragment cheaper than pretending otherwise.
    static constexpr unsigned int solidFlag = 0x4;

    /**
     * @brief Pack an ink colour and its flags.
     *
     * The alpha the colour word used to carry was never read -- a draw's
     * opacity is a uniform, because it applies to everything the draw covers
     * -- so the byte it occupied holds the flags instead.
     */
    static constexpr unsigned int ink(const unsigned int rgb,
                                      const unsigned int depth,
                                      const bool solid) {
      assert(depth <= maxDepthStep);
      return (rgb & 0xFFFFFF00U) | (depth & depthMask) |
             (solid ? solidFlag : 0U);
    }
    /// A solid quad of @p rgb, sitting at @p depth.
    static constexpr unsigned int fill(const unsigned int rgb,
                                       const unsigned int depth) {
      return ink(rgb, depth, true);
    }

    static constexpr unsigned int color3(const unsigned char red,
                                         const unsigned char green,
                                         const unsigned char blue) {
      return static_cast<unsigned int>(red) << 24 |
             static_cast<unsigned int>(green) << 16 |
             static_cast<unsigned int>(blue) << 8;
    }
    static constexpr unsigned int color(const unsigned char rgb) {
      return color3(rgb, rgb, rgb);
    }

    /**
     * @brief Narrow a colour to the sixteen bits the paper field holds.
     *
     * Five bits of red, six of green, five of blue. Paper is a flat fill
     * behind text -- white for a page, a panel colour for a notification --
     * and a flat fill has no gradient to band, so the two units of error this
     * can introduce have nowhere to show. Ink keeps all eight bits, which is
     * what matters: the bars that stand in for lines at a distance are shaded
     * by how much ink each line carries, and they are drawn as ink.
     */
    static constexpr unsigned int rgb565(const unsigned int rgb) {
      // Rounded to the nearest representable value rather than truncated,
      // which halves the error and, more to the point, keeps white white: a
      // page whose paper came back a shade under would be a grey page.
      const auto narrow = [](const unsigned int channel,
                             const unsigned int most) {
        return ((channel * most) + 127) / 255;
      };
      return narrow((rgb >> 24) & 0xFFU, 31) << 11 |
             narrow((rgb >> 16) & 0xFFU, 63) << 5 |
             narrow((rgb >> 8) & 0xFFU, 31);
    }

    /// Pack the paper colour and the cluster this quad stands for.
    static constexpr unsigned int paperAt(const unsigned int rgb,
                                          const unsigned int cluster) {
      assert(cluster <= maxClustersPerPage);
      return rgb565(rgb) << 16 | (cluster & 0xFFFFU);
    }

    /// Pack a glyph's origin in the atlas, in texels.
    static constexpr unsigned int atlasAt(const unsigned int texelX,
                                          const unsigned int texelY) {
      assert(texelX <= 0xFFFFU);
      assert(texelY <= 0xFFFFU);
      return (texelX & 0xFFFFU) << 16 | (texelY & 0xFFFFU);
    }

    /**
     * @brief Pack the quad's size, its atlas layer and its picking kind.
     *
     * The layer field is what bounds how far the glyph atlas may grow in
     * layers. Must stay in step with unpackQuad() in
     * assets/shaders/glyph.vert.glsl.
     */
    static constexpr unsigned int box(const unsigned char layer,
                                      const unsigned int width,
                                      const unsigned int height,
                                      const unsigned int kind) {
      assert(layer < maxAtlasLayers);
      assert(width <= maxQuadExtent);
      assert(height <= maxQuadExtent);
      assert(kind < 4);
      return width << 20 | height << 8 | static_cast<unsigned int>(layer) << 2 |
             kind;
    }
  };

  /// Per-instance vertex layout describing VBORow to the device.
  static render::VertexLayout vertexLayout();

  /**
   * @brief Scale from layout pixels to world units.
   *
   * Pages are laid out in the pixel space Pango works in and shrunk here, so
   * that a glyph's quad and the pen advance that positions it are expressed in
   * the same unit. They previously were not -- quads carried raw pixel sizes
   * while the pen advanced by a seventeenth of one -- which drew every glyph
   * overlapping its neighbours.
   */
  static constexpr float pixelsToWorld = 1.0F / 18.0F;

  /// An empty document.
  static std::shared_ptr<Doc> create(const RendererRef &renderer,
                                     render::RenderDevice *device,
                                     const glm::mat4 &model) {
    return std::make_shared<Doc>(renderer, device, model, Private());
  }
  /**
   * @brief A document holding whatever @p source supplies.
   *
   * The source is consulted once, here, on whichever thread is building the
   * document -- a background loader thread for anything opened from a command
   * line. It is not kept afterwards: a document is its text once it exists,
   * and re-reading a source that has since changed would not be a reload but a
   * silent one.
   */
  static std::shared_ptr<Doc> create(const RendererRef &renderer,
                                     render::RenderDevice *device,
                                     const glm::mat4 &model,
                                     const gleditor::TextSource &source) {
    return std::make_shared<Doc>(renderer, device, model, source, Private());
  }
  std::shared_ptr<Doc> getPtr() { return shared_from_this(); }
  Doc(const RendererRef &renderer, render::RenderDevice *device,
      const glm::mat4 &model, Private);
  Doc(const RendererRef &renderer, render::RenderDevice *device,
      const glm::mat4 &model, const gleditor::TextSource &source, Private);
  ~Doc() override = default;
  void makePages(RenderState &state);
  /// Append every visible page's draw to @p batches.
  /// @param viewProjection projection * view; the document's own model matrix
  ///        is applied on top of it here.
  void collect(std::vector<render::GlyphBatch> &batches,
               const glm::mat4 &viewProjection, const DrawBudget &budget,
               DrawStats &stats) const;
  void newPage(RenderState &state, Glib::RefPtr<Pango::Layout> &layout,
               std::uint32_t textOffset);

  /**
   * @brief Insert UTF-8 text at a document-global byte offset.
   *
   * The text is spliced immediately, so the document is authoritative at once;
   * the layout that follows is scheduled off the render thread, because
   * shaping a page is far too slow to do between a keystroke and the next
   * frame.
   */
  void insert(RenderState &state, std::uint32_t offset, const std::string &utf8,
              Caret *caret);

  /**
   * @brief Remove @p bytes of text from a document-global byte offset.
   *
   * The range is clamped to the document and snapped outwards to character
   * boundaries, so a caller working in bytes cannot leave a partial UTF-8
   * sequence behind. Like insert(), the text is spliced at once and the pages
   * are laid out again afterwards.
   *
   * @return What was actually removed, which is empty when the range was.
   */
  std::string erase(RenderState &state, std::uint32_t offset,
                    std::uint32_t bytes, Caret *caret);

  /**
   * @brief Start or stop being told about this document's edits.
   *
   * The observer is not owned and must outlive the document or remove itself
   * first. Adding one twice adds it once.
   */
  void addObserver(gleditor::DocumentObserver *observer);
  void removeObserver(gleditor::DocumentObserver *observer);

  /// What this document is called. A path, for one opened from a file.
  [[nodiscard]] const std::string &name() const { return docName; }

  /// Scope of the most recent reflow, and how many pages it rebuilt.
  [[nodiscard]] ReflowScope lastReflowScope() const { return reflowScope; }
  [[nodiscard]] std::size_t lastReflowPages() const { return reflowPages; }
  [[nodiscard]] const Glib::ustring &contents() const { return text; }
  /// Position among the renderer's open documents, carried in the picking tag
  /// so a result names which document was clicked.
  void setDocIndex(const std::uint32_t index) { docIndex = index; }
  [[nodiscard]] std::uint32_t documentIndex() const { return docIndex; }
  [[nodiscard]] const Page *page(const std::size_t index) const {
    return index < pages.size() ? &pages[index] : nullptr;
  }

  /**
   * @brief Turn a picking result into a byte offset in this document's text.
   *
   * A glyph resolves through its cluster and the fractional position across
   * the quad. A page background has no character under it, so it resolves to
   * the start of that page -- the click was beside the text rather than on it.
   */
  [[nodiscard]] std::optional<std::uint32_t>
  offsetForPick(const render::PickingTag &tag) const;

  /// Draw @p caret on whichever page holds its offset, if it belongs to this
  /// document.
  void drawCaret(RenderState &state, const glm::mat4 &viewProjection,
                 Caret &caret) const;

  /// Append the highlight spans covering a byte range, one per page it
  /// touches.
  void highlightsFor(std::uint32_t selStart, std::uint32_t selEnd,
                     std::uint32_t colour,
                     std::vector<render::HighlightRange> &out) const;
  [[nodiscard]] size_t numPages() const { return pages.size(); }

  /**
   * @brief Ease this document into place and fade it in.
   *
   * Called when the document is opened, on the render thread: @p timeline is
   * stepped by the render loop and Choreograph does not lock. The document
   * starts in front of where it belongs and becomes opaque as it settles back,
   * so opening one reads as an arrival rather than a document appearing
   * already in position.
   */
  void animateArrival(ch::Timeline &timeline);

  /**
   * @brief Fade this document out and drift it towards the viewer.
   *
   * The reverse of the arrival, and deliberately not an immediate removal: the
   * caller keeps the document alive until hasFadedOut() is true, so the fade
   * is something the user sees rather than a frame in which a document was
   * there and then was not.
   */
  void animateDeparture(ch::Timeline &timeline);

  /**
   * @brief Ease this document to a new resting place.
   *
   * Used when a document closes and the ones after it move up. Retargets the
   * animation from wherever the document currently is, so a move that
   * interrupts another move does not jump.
   */
  void animateMoveTo(ch::Timeline &timeline, const glm::vec3 &target);

  /// True once a departure has been started.
  [[nodiscard]] bool isClosing() const { return closing; }
  /// True when a departing document has finished fading and can be dropped.
  [[nodiscard]] bool hasFadedOut() const { return closing && opacity() <= 0.0F; }

  /// Transform placing this document in the world, including any arrival still
  /// in progress. Prefer this to getModel(), which is the resting place.
  [[nodiscard]] glm::mat4 modelMatrix() const;
  /// Alpha this document currently draws at.
  [[nodiscard]] float currentOpacity() const { return opacity(); }

  friend class Page;
};

#endif // GLEDITOR_DOC_H
