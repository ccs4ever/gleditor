#include <pangomm/init.h>                 // for init
#include <gleditor/renderer.hpp>          // for RenderItemNewDoc, Renderer
#include <gleditor/sdl_wrap.hpp>          // for AutoSDLSurface, AutoSDL
#include <glm/detail/type_vec3.hpp>       // for vec
#include <glm/fwd.hpp>                    // for vec3
#include <glm/gtx/string_cast.hpp>
#include <clocale>                        // for setlocale, LC_ALL
#include <exception>                      // for exception
#include <functional>                     // for reference_wrapper, ref
#include <iostream>                       // for basic_ostream, char_traits
#include <locale>                         // for locale
#include <memory>                         // for __shared_ptr_access, shared...
#include <mutex>                          // for lock_guard
#include <thread>                         // for jthread
#include <atomic>                         // for __atomic_base
#include <optional>                       // for optional
#include <string>                         // for operator<<, basic_string

#include <SDL3/SDL.h>                          // for SDL_INIT_VIDEO
#include <SDL3/SDL_events.h>                   // for SDL_Event, SDL_PollEvent
#include <SDL3/SDL_keycode.h>                  // for KMOD_SHIFT
#include <SDL3/SDL_scancode.h>                 // for SDL_SCANCODE_C, SDL_SCANCODE_D
#include <SDL3/SDL_video.h>                    // for SDL_WINDOWPOS_UNDEFINED
#include <SDL3/SDL_main.h>
#include <argparse/argparse.hpp>          // for ArgumentParser, Argument
#include "config.h"                       // for GLEDITOR_VERSION, TOSTRING
#include <SDL3_image/SDL_image.h>                    // for IMG_INIT_PNG
#include <gleditor/state.hpp>             // for AppState, AppStateRef
#include <glibmm/init.h>                  // for init
#ifdef GLEDITOR_ENABLE_VULKAN
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <gleditor/vulkan/renderer_vk.hpp>
#endif

void handleMouseMove(SDL_Event &evt, const AppStateRef &state) {
  state->mouseX = evt.motion.x;
  state->mouseY = state->view.screenHeight - evt.motion.y;
}

void handleKeyPress(SDL_Event &evt, const AppStateRef &state,
                    RendererRef &renderer) {
  std::lock_guard locker(state->view);
  float speed = 1; // state->view.speed * state->frameTimeDelta.load().count();
  if (0 == speed) {
    speed = 1;
  }
  std::cout << "camera pos before: " << glm::to_string(state->view.pos)
            << " speed: " << speed << "\n";
  switch (evt.key.scancode) {
  case SDL_SCANCODE_Q: {
    state->alive = false;
    break;
  }
  case SDL_SCANCODE_N: {
    renderer->push(RenderItemNewDoc());
    break;
  }
  case SDL_SCANCODE_R: {
    state->view.resetPos();
    break;
  }
  case SDL_SCANCODE_E: {
    state->view.pos -= glm::normalize(glm::cross(state->view.front,
                                                 glm::vec3(1.0F, 0.0F, 0.0F))) *
                       speed;
  } break;
  case SDL_SCANCODE_D: {
    if (0 != (evt.key.mod & SDL_KMOD_SHIFT)) {
      state->view.pos += (speed * state->view.front);
    } else {
      state->view.pos -= (speed * state->view.front);
    }
    break;
  }
  case SDL_SCANCODE_C: {
    state->view.pos += glm::normalize(glm::cross(state->view.front,
                                                 glm::vec3(1.0F, 0.0F, 0.0F))) *
                       speed;
    break;
  }
  case SDL_SCANCODE_S: {
    state->view.pos -=
        glm::normalize(glm::cross(state->view.front, state->view.upward)) *
        speed;
    break;
  }
  case SDL_SCANCODE_F: {
    state->view.pos +=
        glm::normalize(glm::cross(state->view.front, state->view.upward)) *
        speed;
    break;
  }
  case SDL_SCANCODE_G: {
    auto fov = state->view.fov;
    if (0 != (evt.key.mod & SDL_KMOD_SHIFT)) {
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
    state->view.fov = fov;
    break;
  }
  default: {
    break;
  }
  }
  std::cout << "camera pos after: " << glm::to_string(state->view.pos) << "\n";
}

RendererRef handleArgs(const AppStateRef &state, int argc,
               char **argv) {

#ifndef GLEDITOR_VERSION
#error GLEDITOR_VERSION must be defined
#endif

  argparse::ArgumentParser parser("gleditor", TOSTRING(GLEDITOR_VERSION));
  parser.add_argument("--font")
      .default_value("Monospace 16")
      .help("default font to use for display");
  parser.add_argument("--profile")
      .help("perform initial setup, then quit")
      .flag();
#ifdef GLEDITOR_ENABLE_VULKAN
  parser.add_argument("--vulkan")
      .help("use the vulkan renderer")
      .flag();
#endif
  parser.add_argument("files").help("input files").remaining();

  try {

    parser.parse_args(argc, argv);

    RendererRef renderer =
#ifdef GLEDITOR_ENABLE_VULKAN
      parser["--vulkan"] == true ? RendererVK::create(state) :
#endif
      Renderer::create(state);

    state->defaultFontName = parser.get("--font");
    auto files = parser.get<std::vector<std::string>>("files");
    for (const auto& file : files) {
      std::cout << "file: " << file << "\n";
      renderer->push(RenderItemOpenDoc(file));
    }
    state->profiling = parser["--profile"] == true;

    return renderer;

  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    std::cerr << parser;
    return RendererRef(nullptr);
  }
}

int main(int argc, char **argv) {

  // "" signals that LC_ALL should be set from the environment
  std::setlocale(LC_ALL, "");           // for C and C++ where synced with stdio
  std::locale::global(std::locale("")); // for C++
  std::cerr.imbue(std::locale());
  std::cin.imbue(std::locale());
  std::cout.imbue(std::locale());

  auto state = std::make_shared<AppState>();

  RendererRef rend;

  if (nullptr == (rend = handleArgs(state, argc, argv))) {
    return 1;
  }

  try {

    Glib::init();
    Pango::init();

    AutoSDL sdlScoped(SDL_INIT_VIDEO);

    AutoSDLSurface icon("logo.png");

    AutoSDLWindow window("GL Editor", SDL_WINDOWPOS_UNDEFINED,
                         SDL_WINDOWPOS_UNDEFINED, state->view.screenWidth,
                         state->view.screenHeight,
                         SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY,
                         icon.surface);

    std::jthread renderer(std::ref(*rend), std::ref(window));

    while (state->alive) {
      SDL_Event evt;
      while (state->alive && SDL_PollEvent(&evt)) {
        switch (evt.type) {
        case SDL_EVENT_QUIT: {
          state->alive = false;
        }
        case SDL_EVENT_KEY_DOWN: {
          handleKeyPress(evt, state, rend);
          break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
          handleMouseMove(evt, state);
          break;
        }
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MAXIMIZED: {
          const auto width  = evt.window.data1;
          const auto height = evt.window.data2;
          std::cout << "window size changed(w/h): " << width << "/" << height << "\n";
          state->view.screenWidth  = width;
          state->view.screenHeight = height;
          rend->push(RenderItemResize(width, height));
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
