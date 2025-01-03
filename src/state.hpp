#ifndef GLEDITOR_STATE_H
#define GLEDITOR_STATE_H

#include "tqueue.hpp"
#include <atomic>
#include <chrono>
#include <glm/ext/matrix_float4x4.hpp>
#include <mutex>

struct RenderItem;

struct AppState {
  /// Shared state between main and renderer threads
  std::atomic_bool alive{true};
  std::atomic<std::chrono::duration<double>> frameTimeDelta;
  TQueue<RenderItem> renderQueue;
  std::atomic_int mouseX;
  std::atomic_int mouseY;
  struct ViewPerspective : public std::mutex {
    int screenWidth  = 640;
    int screenHeight = 480;
    float fov        = 90.0;
    glm::vec3 pos    = glm::vec3(0.0F, 0.0F, 3.0F);
    glm::vec3 front  = glm::vec3(0.0F, 0.0F, -1.0F);
    glm::vec3 upward = glm::vec3(0.0F, 1.0F, 0.0F);
    float speed      = 2.5;
  } view;
  ///
};

#endif // GLEDITOR_STATE_H
