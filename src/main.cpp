

#include "GL/glew.h"
#include "SDL.h"
#include "SDL_events.h"
#include <iostream>
#include <mutex>
#include <pangomm/init.h>
#include <thread>

#include "SDL_image.h"
#include "SDLwrap.hpp"
#include "renderer.hpp"

void handleMouseMove(SDL_Event &evt, AppState &state) {
  std::lock_guard locker(state.view);
  state.view.x = evt.motion.x;
  state.view.y = evt.motion.y;
}

void handleKeyPress(SDL_Event &evt, AppState &state) {
  switch (evt.key.keysym.scancode) {
  case SDL_SCANCODE_Q: {
    state.alive = false;
    break;
  }
  case SDL_SCANCODE_N: {
    state.renderQueue.push(RenderItemNewDoc());
    break;
  }
  default: {
    break;
  }
  }
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
