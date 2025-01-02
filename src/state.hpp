#ifndef GLEDITOR_STATE_H
#define GLEDITOR_STATE_H

#include "doc.hpp"
#include "tqueue.hpp"
#include <atomic>
#include <glm/ext/matrix_float4x4.hpp>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct RenderItem;

struct AppState {
  std::atomic_bool alive{true};
  // renderer needs to be able to update this every frame
  std::atomic<float> frameTimeDelta{0.0};
  std::vector<std::shared_ptr<Doc>> docs;
  TQueue<RenderItem> renderQueue;
  struct mouse {
    int x{};
    int y{};
  } mouse;
  struct camera {
    glm::mat4x4 view;
    glm::vec3 pos    = glm::vec3(0.0F, 0.0F, 2.0F);
    glm::vec3 front  = glm::vec3(0.0F, 0.0F, 0.0F);
    glm::vec3 upward = glm::vec3(0.0F, 1.0F, 0.0F);
    float speed      = 2.5;
  } camera;
  glm::mat4x4 projection;
  struct Loc {
    int loc;
    std::string type;
    int size = 1;
    operator int() const { return loc; }
  };
  struct Program {
    unsigned int id;
    std::unordered_map<std::string, Loc> locs;
    Loc &operator[](const std::string &loc) { return locs[loc]; }
    const Loc &operator[](const std::string &loc) const { return locs.at(loc); }
  };
  std::unordered_map<std::string, Program> programs;
  int screenWidth  = 640;
  int screenHeight = 480;
  float fov        = 90.0;
};

#endif // GLEDITOR_STATE_H
