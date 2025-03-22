#ifndef GLEDITOR_DOC_H
#define GLEDITOR_DOC_H

#include "drawable.hpp"
#include "glibmm/refptr.h"
#include "glibmm/ustring.h"
#include "pangomm/layout.h"
#include "state.hpp"
#include "vao_supports.hpp"
#include <memory>
#include <vector>

class Doc;
struct GLState;

class Page : public Drawable {
private:
  std::shared_ptr<Doc> doc;
  VAOSupports::Handle pageBackingHandle;
  std::vector<VAOSupports::Handle> glyphs;
  unsigned int tex{};
  Glib::RefPtr<Pango::Layout> layout;

public:
  Page(std::shared_ptr<Doc> doc, GLState &state, glm::mat4 &model,
       Glib::RefPtr<Pango::Layout> layout);
  void draw(const GLState &state, const glm::mat4 &docModel);
  ~Page() override = default;
};

struct DocVBORow {
  std::array<float, 3> pos;
  unsigned int fg;
  unsigned int bg;
  std::array<float, 2> texcoord;
  std::array<float, 2> texBox;
  unsigned int layer;
  std::array<unsigned int, 2> tag;
  static unsigned int color3(unsigned char red, unsigned char green,
                             unsigned char blue) {
    return (unsigned int)(red << 24) | green << 16 | blue << 8 | 255;
  }
  static unsigned int color(unsigned char rgb) { return color3(rgb, rgb, rgb); }
  static constexpr unsigned int layerWidthHeight(unsigned char layer,
                                                 unsigned int width,
                                                 unsigned int height) {
    assert(layer <= 10);
    assert(width < 16384);
    assert(height < 16384);
    return layer << 28 | width << 14 | height;
  }
};
// NOLINTEND

class Doc : public Drawable,
            public VAOSupports<DocVBORow>,
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
  static std::shared_ptr<Doc> create(RendererRef renderer, glm::mat4 model) {
    return std::make_shared<Doc>(renderer, model, Private());
  }
  static std::shared_ptr<Doc> create(RendererRef renderer, glm::mat4 model,
                                     std::string &fileName) {
    return std::make_shared<Doc>(renderer, model, fileName, Private());
  }
  std::shared_ptr<Doc> getPtr() { return shared_from_this(); }
  Doc(RendererRef renderer, glm::mat4 model, Private);
  Doc(RendererRef renderer, glm::mat4 model, std::string &fileName, Private);
  ~Doc() override = default;
  void makePages(GLState &glState);
  void draw(const GLState &state);
  void newPage(GLState &state, Glib::RefPtr<Pango::Layout> &layout);
  size_t numPages() { return pages.size(); }

  friend class Page;
};

#endif // GLEDITOR_DOC_H
