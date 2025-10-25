#ifndef GLEDITOR_DRAWABLE_H
#define GLEDITOR_DRAWABLE_H

#include <glm/ext/matrix_float4x4.hpp>

class Drawable {
protected:
  glm::mat4 model;

public:
  Drawable(glm::mat4 model) : model(model) {}
  virtual ~Drawable() = default;
  [[nodiscard]] glm::mat4 getModel() const { return model; }
};

#endif // GLEDITOR_DRAWABLE_H
