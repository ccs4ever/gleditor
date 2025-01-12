#ifndef GLEDITOR_RENDERER_H
#define GLEDITOR_RENDERER_H

#include <cstdint>
#include <utility>

#include "log.hpp"
#include "state.hpp"

struct AutoSDLWindow;

struct RenderItem {
  enum class Type : std::uint8_t {
    NewDoc,
    Resize,
    OpenDoc,
  };
  Type type;

  RenderItem(Type type) : type(type) {}
  virtual ~RenderItem() = default;
};
struct RenderItemNewDoc : public RenderItem {
  RenderItemNewDoc() : RenderItem(RenderItem::Type::NewDoc) {}
  ~RenderItemNewDoc() override = default;
};

struct RenderItemResize : public RenderItem {
  int width, height;
  RenderItemResize(int width, int height)
      : RenderItem(RenderItem::Type::Resize), width(width), height(height) {}
  ~RenderItemResize() override = default;
};

struct RenderItemOpenDoc : public RenderItem {
  std::string docFile;
  RenderItemOpenDoc(std::string  fileName)
      : RenderItem(RenderItem::Type::OpenDoc), docFile(std::move(fileName)) {}
  ~RenderItemOpenDoc() override = default;
};

struct Renderer : public Loggable {
  void operator()(AppState &state, AutoSDLWindow &window);
};

#endif // GLEDITOR_RENDERER_H
