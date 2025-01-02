#ifndef GLEDITOR_STATE_H
#define GLEDITOR_STATE_H

#include "doc.hpp"
#include "tqueue.hpp"
#include <atomic>
#include <glm/ext/matrix_float4x4.hpp>
#include <mutex>

struct RenderItem;

struct AppState {
  /// Shared state between main and renderer threads
  std::atomic_bool alive{true};
  std::atomic<float> frameTimeDelta{0.0};
  TQueue<RenderItem> renderQueue;
  struct ViewPerspective : public std::mutex {
    int x{};
    int y{};
    int screenWidth  = 640;
    int screenHeight = 480;
    float fov        = 90.0;
    glm::vec3 pos    = glm::vec3(0.0F, 0.0F, 2.0F);
    glm::vec3 front  = glm::vec3(0.0F, 0.0F, 0.0F);
    glm::vec3 upward = glm::vec3(0.0F, 1.0F, 0.0F);
    float speed      = 2.5;
  } view;
  ///
};

#endif // GLEDITOR_STATE_H
