

#include "GL/glew.h"
#include "SDL.h"
#include "SDL_events.h"
#include <glm/ext.hpp>
#include <glm/geometric.hpp>
#include <glm/gtx/string_cast.hpp>
#include <iostream>
#include <mutex>
#include <pangomm/init.h>
#include <thread>

#include "SDL_image.h"
#include "SDLwrap.hpp"
#include "renderer.hpp"

void handleMouseMove(SDL_Event &evt, AppState &state) {
  state.mouseX = evt.motion.x;
  state.mouseY = evt.motion.y;
}

void handleKeyPress(SDL_Event &evt, AppState &state) {
  std::lock_guard locker(state.view);
  const auto speed = state.view.speed;
  std::cout << "camera pos before: " << glm::to_string(state.view.pos)
            << " speed: " << speed << "\n";
  switch (evt.key.keysym.scancode) {
  case SDL_SCANCODE_Q: {
    state.alive = false;
    break;
  }
  case SDL_SCANCODE_N: {
    state.renderQueue.push(RenderItemNewDoc());
    break;
  }
  case SDL_SCANCODE_R: {
    state.view.pos    = glm::vec3(0.0F, 0.0F, 3.0F);
    state.view.front  = glm::vec3(0.0F, 0.0F, -1.0F);
    state.view.upward = glm::vec3(0.0F, 1.0F, 0.0F);
    break;
  }
  case SDL_SCANCODE_E: {
    state.view.pos -= glm::normalize(glm::cross(state.view.front,
                                                glm::vec3(1.0F, 0.0F, 0.0F))) *
                      speed;
  } break;
  case SDL_SCANCODE_D: {
    if (0 != (evt.key.keysym.mod & KMOD_SHIFT)) {
      state.view.pos += (speed * state.view.front);
    } else {
      state.view.pos -= (speed * state.view.front);
    }
    break;
  }
  case SDL_SCANCODE_C: {
    state.view.pos += glm::normalize(glm::cross(state.view.front,
                                                glm::vec3(1.0F, 0.0F, 0.0F))) *
                      speed;
    break;
  }
  case SDL_SCANCODE_S: {
    state.view.pos -=
        glm::normalize(glm::cross(state.view.front, state.view.upward)) * speed;
    break;
  }
  case SDL_SCANCODE_F: {
    state.view.pos +=
        glm::normalize(glm::cross(state.view.front, state.view.upward)) * speed;
    break;
  }
  default: {
    break;
  }
  }
  std::cout << "camera pos after: " << glm::to_string(state.view.pos) << "\n";
}

int main(int argc, char **argv) {

  try {

    AppState state;

    Pango::init();

    AutoSDL sdlScoped(SDL_INIT_VIDEO);
    AutoSDLImg sdlImgScoped(IMG_INIT_PNG);

    AutoSDLSurface icon("logo.png");

    AutoSDLWindow window(
        "GL Editor", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480,
        SDL_WINDOW_OPENGL | SDL_WINDOW_MAXIMIZED, icon.surface);

    Renderer rend;

    std::jthread renderer(rend, std::ref(state), std::ref(window));

    while (state.alive) {
      SDL_Event evt;
      while (state.alive && bool(SDL_PollEvent(&evt))) {
        switch (evt.type) {
        case SDL_QUIT: {
          state.alive = false;
        }
        case SDL_KEYDOWN: {
          handleKeyPress(evt, state);
          break;
        }
        case SDL_MOUSEMOTION: {
          handleMouseMove(evt, state);
          break;
        }
        default:
          break;
        }
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}

// vi: set sw=2 sts=2 ts=2 et:
