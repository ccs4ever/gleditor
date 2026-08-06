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

class Page : public Drawable {
private:
  std::shared_ptr<Doc> doc;
  BufferPool::Allocation pageBacking{};
  /// Glyph rows actually written, which is what gets drawn. One instance is
  /// emitted per row.
  std::uint32_t instanceCount{};
  Glib::RefPtr<Pango::Layout> layout;

public:
  Page(std::shared_ptr<Doc> aDoc, RenderState &state, glm::mat4 &model,
       Glib::RefPtr<Pango::Layout> aLayout);
  void draw(RenderState &state, const glm::mat4 &docModel) const;
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
  void draw(RenderState &state) const;
  void newPage(RenderState &state, Glib::RefPtr<Pango::Layout> &layout);
  [[nodiscard]] size_t numPages() const { return pages.size(); }

  friend class Page;
};

#endif // GLEDITOR_DOC_H
