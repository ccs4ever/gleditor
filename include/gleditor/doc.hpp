#ifndef GLEDITOR_DOC_H
#define GLEDITOR_DOC_H

#include <array>
#include <cassert>
#include <cstdint>
#include <gleditor/buffer_pool.hpp>
#include <gleditor/drawable.hpp>
#include <gleditor/renderer.hpp>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <glm/ext/matrix_float4x4.hpp>
#include <memory>
#include <optional>
#include <pangomm/layout.h>
#include <string>
#include <vector>

#include <gleditor/draw_budget.hpp>
#include <gleditor/render/types.hpp>

class Caret;
class Doc;
struct RenderState;

namespace render {
class RenderDevice;
}

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
  Glib::RefPtr<Pango::Layout> layout;
  /// Byte offset of this page's text within the whole document, so a picking
  /// result can name a position in the document rather than in the page.
  std::uint32_t textOffset{};
  /// Every cluster on the page, in text order.
  std::vector<ClusterBox> clusters;
  /// This page's position in its document, carried in the picking tag.
  std::uint32_t pageIndex{};
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
               const glm::mat4 &docTransform, const DrawBudget &budget,
               DrawStats &stats) const;

  [[nodiscard]] std::uint32_t baseOffset() const { return textOffset; }
  [[nodiscard]] const std::vector<ClusterBox> &clusterBoxes() const {
    return clusters;
  }
  [[nodiscard]] const Glib::RefPtr<Pango::Layout> &layoutRef() const {
    return layout;
  }
  [[nodiscard]] const BufferPool::Allocation &allocation() const {
    return pageBacking;
  }
  /// Move this page's text offset without touching its shaping or its rows,
  /// which an edit earlier in the document leaves byte-identical.
  void shiftBaseOffset(const std::uint32_t bytes) { textOffset += bytes; }
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
  std::string docFile;
  Glib::ustring text;
  RendererRef renderer;
  /// Vertex storage shared by every page of this document.
  std::unique_ptr<BufferPool> pool;
  /// Position among the open documents; see setDocIndex().
  std::uint32_t docIndex{};
  /// Outcome of the most recent reflow, for reporting and for tests.
  ReflowScope reflowScope{ReflowScope::Document};
  std::size_t reflowPages{};

  /// Build a page layout for text starting at @p offset, with the page
  /// geometry makePages() uses. Safe to call off the render thread.
  [[nodiscard]] Glib::RefPtr<Pango::Layout>
  layoutFrom(std::uint32_t offset) const;
  /// Bytes of document text a finished page layout consumes.
  [[nodiscard]] static std::uint32_t
  consumedBytes(const Glib::RefPtr<Pango::Layout> &layout);
  /// Byte offsets at which each line of a layout starts.
  [[nodiscard]] static std::vector<int>
  lineStarts(const Glib::RefPtr<Pango::Layout> &layout);
  /// Rebuild the pages an edit disturbed. Render thread only.
  void reflowFrom(RenderState &state, std::size_t firstPage, std::uint32_t at,
                  std::uint32_t inserted, const std::vector<int> &oldStarts,
                  std::uint32_t oldConsumed);
  // token to keep anything other than Doc::create from using our constructor
  struct Private {
    explicit Private() = default;
  };

public:
  /**
   * @brief One glyph instance.
   *
   * Field order and offsets must stay in step with Doc::vertexLayout() and
   * with the attribute locations in assets/shaders/glyph.vert.glsl.
   */
  struct VBORow {
    std::array<float, 3> pos;
    unsigned int fg;
    unsigned int bg;
    std::array<float, 2> texcoord;
    std::array<float, 2> texBox;
    unsigned int layer;
    std::array<unsigned int, 2> tag;

    static unsigned int color3(const unsigned char red,
                               const unsigned char green,
                               const unsigned char blue) {
      return static_cast<unsigned int>(red << 24) | green << 16 | blue << 8 |
             255;
    }
    static unsigned int color(unsigned char rgb) {
      return color3(rgb, rgb, rgb);
    }
    /**
     * @brief Pack the atlas layer and the quad's size into one word.
     *
     * Six bits of layer, thirteen each of width and height. The layer field is
     * what bounds how far the glyph atlas can grow in layers -- the hardware
     * allows far more -- and thirteen bits leaves the largest quad drawn, a
     * page background of around fifteen hundred layout pixels, five times the
     * room it needs. Must stay in step with unpackLayerWH() in
     * assets/shaders/glyph.vert.glsl and lwh() in doc.cpp.
     */
    static constexpr unsigned int maxAtlasLayers = 64;
    static constexpr unsigned int maxQuadExtent  = 8191;
    static constexpr unsigned int layerWidthHeight(const unsigned char layer,
                                                   const unsigned int width,
                                                   const unsigned int height) {
      assert(layer < maxAtlasLayers);
      assert(width <= maxQuadExtent);
      assert(height <= maxQuadExtent);
      return layer << 26 | width << 13 | height;
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

  static std::shared_ptr<Doc> create(const RendererRef &renderer,
                                     render::RenderDevice *device,
                                     const glm::mat4 &model) {
    return std::make_shared<Doc>(renderer, device, model, Private());
  }
  static std::shared_ptr<Doc> create(const RendererRef &renderer,
                                     render::RenderDevice *device,
                                     const glm::mat4 &model,
                                     std::string &fileName) {
    return std::make_shared<Doc>(renderer, device, model, fileName, Private());
  }
  std::shared_ptr<Doc> getPtr() { return shared_from_this(); }
  Doc(const RendererRef &renderer, render::RenderDevice *device,
      const glm::mat4 &model, Private);
  Doc(const RendererRef &renderer, render::RenderDevice *device,
      const glm::mat4 &model, const std::string &fileName, Private);
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

  friend class Page;
};

#endif // GLEDITOR_DOC_H
