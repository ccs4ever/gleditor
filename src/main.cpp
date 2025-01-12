

#include "GL/glew.h"
#include "SDL.h"
#include "SDL_events.h"
#include "argparse/argparse.hpp"
#include "config.h"
#include <exception>
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

void handleWindowChange(SDL_Event &evt, AppState &state, SDL_Window *win) {
  switch (evt.window.event) {
  case SDL_WINDOWEVENT_RESIZED:
  case SDL_WINDOWEVENT_SIZE_CHANGED:
  case SDL_WINDOWEVENT_MAXIMIZED: {
    const auto width  = evt.window.data1;
    const auto height = evt.window.data2;
    std::cout << "window size changed(w/h): " << width << "/" << height << "\n";
    state.view.screenWidth  = width;
    state.view.screenHeight = height;
    state.renderQueue.push(RenderItemResize(width, height));
    break;
  }
  default:
    break;
  }
}

void handleMouseMove(SDL_Event &evt, AppState &state) {
  state.mouseX = evt.motion.x;
  state.mouseY = evt.motion.y;
}

void handleKeyPress(SDL_Event &evt, AppState &state) {
  std::lock_guard locker(state.view);
  const float speed = state.view.speed * state.frameTimeDelta.load().count();
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
    state.view.resetPos();
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
  case SDL_SCANCODE_G: {
    auto fov = state.view.fov;
    if (0 != (evt.key.keysym.mod & KMOD_SHIFT)) {
      fov -= 1;
      if (fov < 1) {
        fov = 1;
      }
    } else {
      fov += 1;
      if (fov > 360) {
        fov = 360;
      }
    }
    state.view.fov = fov;
    break;
  }
  default: {
    break;
  }
  }
  std::cout << "camera pos after: " << glm::to_string(state.view.pos) << "\n";
}

int handleArgs(AppState &state, int argc, char **argv) {

#ifndef GLEDITOR_VERSION
#error GLEDITOR_VERSION must be defined
#endif

  argparse::ArgumentParser parser("gleditor", TOSTRING(GLEDITOR_VERSION));
  parser.add_argument("--font")
      .default_value("Monospace 16")
      .help("default font to use for display");
  parser.add_argument("-f", "--file").help("path to document to open");

  try {

    parser.parse_args(argc, argv);

    state.defaultFontName = parser.get("--font");
    if (parser.present("--file")) {
      state.renderQueue.push(RenderItemOpenDoc(parser.get("--file")));
    }

  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    std::cerr << parser;
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {

  AppState state;

  if (0 != handleArgs(state, argc, argv)) {
    return 1;
  }

  try {

    Pango::init();

    AutoSDL sdlScoped(SDL_INIT_VIDEO);
    AutoSDLImg sdlImgScoped(IMG_INIT_PNG);

    AutoSDLSurface icon("logo.png");

    AutoSDLWindow window("GL Editor", SDL_WINDOWPOS_UNDEFINED,
                         SDL_WINDOWPOS_UNDEFINED, state.view.screenWidth,
                         state.view.screenHeight, SDL_WINDOW_OPENGL,
                         icon.surface);

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
        case SDL_WINDOWEVENT:
          handleWindowChange(evt, state, window.window);
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
