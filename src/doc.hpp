#ifndef GLEDITOR_DOC_H
#define GLEDITOR_DOC_H

#include "drawable.hpp"
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

public:
  Page(std::shared_ptr<Doc> doc, AppState& appState, GLState& state, glm::mat4 &model);
  void draw(const GLState &state, const glm::mat4 &docModel);
  ~Page() override = default;
};

class Doc : public Drawable,
            public VAOSupports,
            public std::enable_shared_from_this<Doc> {
private:
  // NOLINTBEGIN (modernize-avoid-c-arrays)
  struct VBORow {
    float pos[3];
    unsigned int fg;
    unsigned int bg;
    float texcoord[2];
    float texBox[2];
    unsigned int layer;
    static unsigned int color3(unsigned char red, unsigned char green, unsigned char blue) { return (unsigned int)(red << 24) | green << 16 | blue << 8 | 255; }
    static unsigned int color(unsigned char rgb) { return color3(rgb, rgb, rgb); }
    static constexpr unsigned int layerWidthHeight(unsigned char layer, unsigned int width, unsigned int height) { assert(width < 4096 && height < 4096); return layer << 24 | width << 12 | height; }
  };
  // NOLINTEND
  int maxQuads = 10000;
  std::vector<Page> pages;
  std::string docFile;
  // token to keep anything other than Doc::create from using our constructor
  struct Private {
    explicit Private() = default;
  };

public:
  Doc(glm::mat4 model, Private);
  Doc(glm::mat4 model, std::string& fileName, Private);
  ~Doc() override = default;
  static std::shared_ptr<Doc> create(glm::mat4 model) {
    return std::make_shared<Doc>(model, Private());
  }
  static std::shared_ptr<Doc> create(glm::mat4 model, std::string& fileName) {
    return std::make_shared<Doc>(model, fileName, Private());
  }
  std::shared_ptr<Doc> getPtr() { return shared_from_this(); }
  void draw(const GLState &state);
  void newPage(AppState& appState, GLState& state);

  friend class Page;
};

#endif // GLEDITOR_DOC_H
