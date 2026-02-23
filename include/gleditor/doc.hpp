#ifndef GLEDITOR_DOC_H
#define GLEDITOR_DOC_H

#include <array>
#include <cassert>
#include <gleditor/drawable.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/vao_supports.hpp>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <glm/ext/matrix_float4x4.hpp>
#include <memory>
#include <pangomm/layout.h>
#include <string>
#include <vector>

class Doc;
struct GLState;

class Page : public Drawable {
private:
  std::shared_ptr<Doc> doc;
  VAOSupports::Handle pageBackingHandle{};
  std::vector<VAOSupports::Handle> glyphs;
  unsigned int tex{};
  Glib::RefPtr<Pango::Layout> layout;

public:
  Page(std::shared_ptr<Doc> doc, GLState &state, glm::mat4 &model,
       Glib::RefPtr<Pango::Layout> layout);
  void draw(const GLState &state, const glm::mat4 &docModel) const;
  ~Page() override = default;
};

// NOLINTEND

class Doc : public Drawable,
            public VAOSupports,
            public std::enable_shared_from_this<Doc> {
private:
  int maxQuads = 10000;
  std::vector<Page> pages;
  std::string docFile;
  Glib::ustring text;
  // token to keep anything other than Doc::create from using our constructor
  struct Private {
    explicit Private() = default;
  };

public:
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

  static std::shared_ptr<Doc> create(const RendererRef &renderer,
                                     const glm::mat4 &model) {
    return std::make_shared<Doc>(renderer, model, Private());
  }
  static std::shared_ptr<Doc> create(const RendererRef &renderer,
                                     const glm::mat4 &model,
                                     std::string &fileName) {
    return std::make_shared<Doc>(renderer, model, fileName, Private());
  }
  std::shared_ptr<Doc> getPtr() { return shared_from_this(); }
  Doc(const RendererRef& renderer, const glm::mat4& model, Private);
  Doc(const RendererRef& renderer, const glm::mat4& model, const std::string &fileName, Private);
  ~Doc() override = default;
  void makePages(GLState &glState);
  void draw(const GLState &state) const;
  void newPage(GLState &state, Glib::RefPtr<Pango::Layout> &layout);
  size_t numPages() const { return pages.size(); }

  friend class Page;
};

#endif // GLEDITOR_DOC_H
