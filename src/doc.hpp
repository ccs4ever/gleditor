#ifndef GLEDITOR_DOC_H
#define GLEDITOR_DOC_H

#include "drawable.hpp"
#include "vao_supports.hpp"
#include <memory>
#include <vector>

class Doc;
struct AppState;

class Page : public Drawable {
private:
  std::shared_ptr<Doc> doc;
  VAOSupports::Handle pageBackingHandle;
  std::vector<VAOSupports::Handle> glyphs;

public:
  Page(std::shared_ptr<Doc> doc, glm::mat4 &model);
  void draw(const AppState &state, const glm::mat4 &docModel) const;
  ~Page() override = default;
};

class Doc : public Drawable,
            public VAOSupports,
            public std::enable_shared_from_this<Doc> {
private:
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
  void draw(const AppState &state) const;
  void newPage(AppState& state);

  friend class Page;
};

#endif // GLEDITOR_DOC_H
