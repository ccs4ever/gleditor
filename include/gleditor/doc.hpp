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
#include <pangomm/layout.h>
#include <string>
#include <vector>

#include <gleditor/render/types.hpp>

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
  /// Glyph rows actually written, which is what gets drawn. One instance is
  /// emitted per row.
  std::uint32_t instanceCount{};
  Glib::RefPtr<Pango::Layout> layout;
  /// Byte offset of this page's text within the whole document, so a picking
  /// result can name a position in the document rather than in the page.
  std::uint32_t textOffset{};
  /// Every cluster on the page, in text order.
  std::vector<ClusterBox> clusters;

public:
  Page(std::shared_ptr<Doc> aDoc, RenderState &state, glm::mat4 &model,
       Glib::RefPtr<Pango::Layout> aLayout, std::uint32_t aTextOffset);
  /// @param docTransform projection * view * document model.
  void draw(RenderState &state, const glm::mat4 &docTransform) const;

  [[nodiscard]] std::uint32_t baseOffset() const { return textOffset; }
  [[nodiscard]] const std::vector<ClusterBox> &clusterBoxes() const {
    return clusters;
  }
  ~Page() override = default;
};

class Doc : public Drawable, public std::enable_shared_from_this<Doc> {
private:
  std::vector<Page> pages;
  std::string docFile;
  Glib::ustring text;
  RendererRef renderer;
  /// Vertex storage shared by every page of this document.
  std::unique_ptr<BufferPool> pool;
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
    static constexpr unsigned int layerWidthHeight(const unsigned char layer,
                                                   const unsigned int width,
                                                   const unsigned int height) {
      assert(layer <= 10);
      assert(width < 16384);
      assert(height < 16384);
      return layer << 28 | width << 14 | height;
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
  /// @param viewProjection projection * view; the document's own model matrix
  ///        is applied on top of it here.
  void draw(RenderState &state, const glm::mat4 &viewProjection) const;
  void newPage(RenderState &state, Glib::RefPtr<Pango::Layout> &layout,
               std::uint32_t textOffset);
  [[nodiscard]] size_t numPages() const { return pages.size(); }

  friend class Page;
};

#endif // GLEDITOR_DOC_H
