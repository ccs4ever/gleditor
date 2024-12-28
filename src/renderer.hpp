#ifndef GLEDITOR_RENDERER_H
#define GLEDITOR_RENDERER_H

#include <cstdint>

#include "log.hpp"
#include "state.hpp"

struct RenderItem {
  enum class Type : std::uint8_t {
    NewDoc,
  };
  Type type;

  RenderItem(Type type) : type(type) {}
};
struct RenderItemNewDoc : public RenderItem {
  RenderItemNewDoc() : RenderItem(RenderItem::Type::NewDoc) {}
};

struct Renderer : public Loggable {
  void operator()(AppState &state);
};

#endif // GLEDITOR_RENDERER_H
