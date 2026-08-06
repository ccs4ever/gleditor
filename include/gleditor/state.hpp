#ifndef GLEDITOR_STATE_H
#define GLEDITOR_STATE_H

#include <atomic>
#include <chrono>
#include <glm/ext/vector_float3.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gleditor/render/diagnostics.hpp>

struct RenderItem;

struct AppState {
  /// Shared state between main and renderer threads
  // set before the render thread starts, no need to synchronize
  std::string defaultFontName;
  /// When set, the first fully drawn frame is written here as a PPM and the
  /// path is cleared. Used to compare backends pixel for pixel.
  std::string screenshotPath;
  /// Picking queries to run once the document has settled, each printed as it
  /// comes back. Used to compare what the backends report at given pixels.
  /// Written before the render thread starts and only read after.
  std::vector<std::pair<int, int>> requestedPicks;
  std::atomic_bool alive{true};
  /// Set when the render thread stops because of an error rather than because
  /// it was asked to. The process exit status follows it, so a renderer that
  /// dies is not reported as a successful run.
  std::atomic_bool renderFailed{false};
  /// Notifications to show as soon as the first frame can be drawn. Driver
  /// diagnostics are the reason the overlay exists, and they cannot be provoked
  /// on demand, so this is how the overlay is exercised and compared between
  /// backends. Written before the render thread starts and only read after.
  std::vector<std::pair<render::DiagnosticSeverity, std::string>>
      requestedToasts;
  bool profiling{};
  /// When set, a driver error ends the render thread instead of being shown as
  /// a notification. Automated runs want it: a frame rendered by a driver that
  /// was reporting errors proves nothing, however plausible it looks.
  bool strictDiagnostics{};
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
      pos    = glm::vec3(0.0F, 0.0F, 1000.0F);
      front  = glm::vec3(0.0F, 0.0F, -1.0F);
      upward = glm::vec3(0.0F, 1.0F, 0.0F);
    }
  } view;
  ///
};

using AppStateRef = std::shared_ptr<AppState>;

#endif // GLEDITOR_STATE_H
