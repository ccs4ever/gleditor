#ifndef GLEDITOR_STATE_H
#define GLEDITOR_STATE_H

#include <atomic>
#include <chrono>
#include <gleditor/tqueue.hpp>
#include <glm/ext/matrix_float4x4.hpp>
#include <mutex>

struct RenderItem;

struct AppState {
  /// Shared state between main and renderer threads
  // set before the render thread starts, no need to synchronize
  std::string defaultFontName;
  std::atomic_bool alive{true};
  bool profiling{};
  std::atomic<std::chrono::duration<float>> frameTimeDelta;
  std::atomic_int mouseX;
  std::atomic_int mouseY;
  struct ViewPerspective : public std::mutex {
    int screenWidth  = 800;
    int screenHeight = 600;
    float fov        = 5.0;
    glm::vec3 pos;
    glm::vec3 front;
    glm::vec3 upward;
    float speed = 60.0;
    ViewPerspective() { resetPos(); }
    void resetPos() {
      pos    = glm::vec3(0.0F, 0.0F, 100.0F);
      front  = glm::vec3(0.0F, 0.0F, -1.0F);
      upward = glm::vec3(0.0F, 1.0F, 0.0F);
    }
  } view;
  ///
};

using AppStateRef = std::shared_ptr<AppState>;

#endif // GLEDITOR_STATE_H
