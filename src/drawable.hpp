#ifndef GLEDITOR_DRAWABLE_H
#define GLEDITOR_DRAWABLE_H

#include <glm/mat4x4.hpp>

class Drawable {
protected:
  glm::mat4x4 model;
public:
  Drawable(glm::mat4 model) : model(model) {}
  virtual ~Drawable() = default;
};

#endif // GLEDITOR_DRAWABLE_H
