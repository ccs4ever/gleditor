#ifndef GLEDITOR_DOC_H
#define GLEDITOR_DOC_H

#include "drawable.hpp"
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

public:
  Page(std::shared_ptr<Doc> doc, glm::mat4 &model);
  void draw(const GLState &state, const glm::mat4 &docModel) const;
  ~Page() override = default;
};

class Doc : public Drawable,
            public VAOSupports,
            public std::enable_shared_from_this<Doc> {
private:
  // NOLINTBEGIN (modernize-avoid-c-arrays)
  struct VBORow {
    float pos[3];
    float fg[3];
    float bg[3];
    float texcoord[2];
    float layer;
  };
  // NOLINTEND
  int maxQuads = 10000;
  std::vector<Page> pages;
  // token to keep anything other than Doc::create from using our constructor
  struct Private {
    explicit Private() = default;
  };

public:
  Doc(glm::mat4 model, Private);
  ~Doc() override = default;
  static std::shared_ptr<Doc> create(glm::mat4 model) {
    return std::make_shared<Doc>(model, Private());
  }
  std::shared_ptr<Doc> getPtr() { return shared_from_this(); }
  void draw(const GLState &state) const;
  void newPage(GLState& state);

  friend class Page;
};

#endif // GLEDITOR_DOC_H
