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
  Page(std::shared_ptr<Doc> doc, AppState &appState, GLState &state,
       glm::mat4 &model, Glib::RefPtr<Pango::Layout>& layout);
  void draw(const GLState &state, const glm::mat4 &docModel);
  ~Page() override = default;
};

class Doc : public Drawable,
            public VAOSupports,
            public std::enable_shared_from_this<Doc> {
private:
  // NOLINTBEGIN (modernize-avoid-c-arrays)
  struct VBORow {
    std::array<float, 3> pos;
    unsigned int fg;
    unsigned int bg;
    std::array<float, 2> texcoord;
    std::array<float, 2> texBox;
    unsigned int layer;
    static unsigned int color3(unsigned char red, unsigned char green,
                               unsigned char blue) {
      return (unsigned int)(red << 24) | green << 16 | blue << 8 | 255;
    }
    static unsigned int color(unsigned char rgb) {
      return color3(rgb, rgb, rgb);
    }
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
  int maxQuads = 10000;
  std::vector<Page> pages;
  std::string docFile;
  Glib::ustring text;
  // token to keep anything other than Doc::create from using our constructor
  struct Private {
    explicit Private() = default;
  };

public:
  Doc(glm::mat4 model, AppState& appState, Private);
  Doc(glm::mat4 model, AppState& appState, GLState &glState, std::string &fileName, Private);
  ~Doc() override = default;
  void makePages(AppState &appState, GLState &glState);
  static std::shared_ptr<Doc> create(glm::mat4 model, AppState& appState) {
    return std::make_shared<Doc>(model, appState, Private());
  }
  static std::shared_ptr<Doc> create(glm::mat4 model, AppState& appState, GLState &glState, std::string &fileName) {
    auto ret = std::make_shared<Doc>(model, appState, glState, fileName, Private());
    ret->makePages(appState, glState);
    return ret;
  }
  std::shared_ptr<Doc> getPtr() { return shared_from_this(); }
  void draw(const GLState &state);
  void newPage(AppState &appState, GLState &state, Glib::RefPtr<Pango::Layout>& layout);

  friend class Page;
};

#endif // GLEDITOR_DOC_H
