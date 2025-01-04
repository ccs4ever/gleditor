#ifndef GLEDITOR_RENDERER_H
#define GLEDITOR_RENDERER_H

#include <cstdint>

#include "log.hpp"
#include "state.hpp"

struct AutoSDLWindow;

struct RenderItem {
  enum class Type : std::uint8_t {
    NewDoc, Resize,
  };
  Type type;

  RenderItem(Type type) : type(type) {}
};
struct RenderItemNewDoc : public RenderItem {
  RenderItemNewDoc() : RenderItem(RenderItem::Type::NewDoc) {}
};

struct RenderItemResize : public RenderItem {
	int width, height;
  RenderItemResize(int width, int height) : RenderItem(RenderItem::Type::Resize), width(width), height(height) {}
};

struct Renderer : public Loggable {
  void operator()(AppState &state, AutoSDLWindow& window);
};

#endif // GLEDITOR_RENDERER_H
