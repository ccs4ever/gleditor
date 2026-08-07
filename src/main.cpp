#include <array>                    // for array
#include <atomic>                   // for __atomic_base
#include <clocale>                  // for setlocale, LC_ALL
#include <exception>                // for exception
#include <functional>               // for reference_wrapper, ref
#include <gleditor/paths.hpp>       // for assetPath
#include <gleditor/renderer.hpp>    // for RenderItemNewDoc, Renderer
#include <gleditor/sdl_wrap.hpp>    // for AutoSDLSurface, AutoSDL
#include <glm/detail/type_vec3.hpp> // for vec
#include <glm/fwd.hpp>              // for vec3
#include <glm/gtx/string_cast.hpp>
#include <iostream>       // for basic_ostream, char_traits
#include <locale>         // for locale
#include <memory>         // for __shared_ptr_access, shared...
#include <stdexcept>      // for runtime_error
#include <mutex>          // for lock_guard
#include <pangomm/init.h> // for init
#include <string>         // for operator<<, basic_string
#include <string_view>    // for string_view
#include <thread>         // for jthread
#include <utility>        // for pair

#include "config.h"                   // for GLEDITOR_VERSION, TOSTRING
#include <argparse/argparse.hpp>      // for ArgumentParser, Argument
#include <gleditor/sdl_compat.hpp>    // for SDL, whichever major version
#include <gleditor/render/device.hpp> // for backendWindowFlags, Backend
#include <gleditor/render/types.hpp>  // for Backend
#include <gleditor/state.hpp>         // for AppState, AppStateRef
#include <glibmm/init.h>              // for init

void handleMouseMove(const SDL_Event &evt, const AppStateRef &state) {
  // Kept in window coordinates, top-down, which is what
  // RenderDevice::requestPickingTag takes. OpenGL's bottom-up framebuffer
  // convention is the GL backend's business, not the application's.
  state->mouseX = static_cast<int>(evt.motion.x);
  state->mouseY = static_cast<int>(evt.motion.y);
}

void handleKeyPress(const SDL_Event &evt, const AppStateRef &state,
                    const RendererRef &renderer) {
  std::lock_guard locker(state->view);
  const float speed =
      1; // state->view.speed * state->frameTimeDelta.load().count();
  /*if (0 == speed) {
    speed = 1;
  }*/
  std::cout << "camera pos before: " << glm::to_string(state->view.pos)
            << " speed: " << speed << "\n";
  switch (sdl::keyScancode(evt)) {
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
    break;
  }
  case SDL_SCANCODE_D: {
    if (0 != (sdl::keyModifiers(evt) & SDL_KMOD_SHIFT)) {
      state->view.pos += speed * state->view.front;
    } else {
      state->view.pos -= speed * state->view.front;
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
    if (0 != (sdl::keyModifiers(evt) & SDL_KMOD_SHIFT)) {
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

/// Split a `--toast` argument into its severity and its text. An unprefixed
/// argument is a warning, which is what a message worth showing but not worth
/// stopping for amounts to.
std::pair<render::DiagnosticSeverity, std::string>
parseToast(const std::string &argument) {
  using render::DiagnosticSeverity;
  static const std::array<std::pair<std::string_view, DiagnosticSeverity>, 3>
      prefixes = {std::pair{"info:", DiagnosticSeverity::Info},
                  std::pair{"warning:", DiagnosticSeverity::Warning},
                  std::pair{"error:", DiagnosticSeverity::Error}};

  for (const auto &[prefix, severity] : prefixes) {
    if (argument.starts_with(prefix)) {
      return {severity, argument.substr(prefix.size())};
    }
  }
  return {DiagnosticSeverity::Warning, argument};
}

/**
 * @brief Parse the command line and build the renderer it asks for.
 * @param[out] backend Receives the selected graphics backend.
 */
RendererRef handleArgs(const AppStateRef &state, render::Backend &backend,
                       const int argc, const char *const *const argv) {

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
  parser.add_argument("--fov")
      .default_value(std::string{"5"})
      .help("initial vertical field of view in degrees; widening it is how a "
            "headless run sees many pages at once, and small ones");
  parser.add_argument("--no-cull")
      .help("draw every page of every document, including those entirely "
            "outside the view")
      .flag();
  parser.add_argument("--coarse-below")
      .default_value(std::string{"0.15"})
      .help("draw a page as one solid bar per line once one layout pixel of it "
            "covers fewer than this many screen pixels; 0 always draws glyphs");
  parser.add_argument("--benchmark")
      .default_value(std::string{"0"})
      .help("draw N frames once the document has settled, report how long "
            "they took, and quit");
  parser.add_argument("--pick")
      .append()
      .help("read the picking tag at X,Y once the document has settled and "
            "print it; may be given more than once");
  parser.add_argument("--screenshot")
      .default_value(std::string{})
      .help("write the first rendered frame to this path as a PPM");
  parser.add_argument("--backend")
      .default_value(std::string{"opengl"})
      .help("rendering backend: opengl, opengles or vulkan");
  parser.add_argument("--click")
      .append()
      .help("click at X,Y once the document has settled, moving the caret "
            "there, and print where it landed; may be given more than once");
  parser.add_argument("--select")
      .help("select the document byte range START,END once the document has "
            "settled, as a drag would");
  parser.add_argument("--type")
      .default_value(std::string{})
      .help("text to insert at the caret once it has been placed");
  parser.add_argument("--toast")
      .append()
      .help("show a notification once the first frame is drawn, as "
            "[info:|warning:|error:]TEXT; may be given more than once");
  parser.add_argument("--strict-diagnostics")
      .help("treat a driver error as fatal instead of showing it as a "
            "notification")
      .flag();
  parser.add_argument("--print-asset-dir")
      .help("report the directory the shaders and icon were found in, then "
            "quit; the one check a package can make without a GPU")
      .flag();
  parser.add_argument("files").help("input files").remaining();

  try {

    parser.parse_args(argc, argv);

    backend = render::backendFromName(parser.get<std::string>("--backend"));
    if (!render::backendCompiledIn(backend)) {
      throw std::runtime_error(
          "The " + render::backendName(backend) +
          " backend was not compiled into this binary. Rebuild with "
          "GLEDITOR_ENABLE_VULKAN=1 to enable Vulkan.");
    }
    // Which SDL this binary was built against is not something the command
    // line can change, and a bug report is much easier to read with it stated.
    // Suppressed for --print-asset-dir, whose whole output is one path a
    // script reads: a query prints its answer and nothing else.
    if (parser["--print-asset-dir"] == false) {
      std::cout << "backend: " << render::backendName(backend) << ", SDL"
                << sdl::majorVersion << "\n";
    }

    RendererRef renderer = Renderer::create(state, backend);

    state->defaultFontName = parser.get("--font");
    // "files" is a remaining-argument list, which argparse leaves unset rather
    // than empty when no file is named.
    if (parser.present<std::vector<std::string>>("files")) {
      for (const auto &files = parser.get<std::vector<std::string>>("files");
           const auto &file : files) {
        std::cout << "file: " << file << "\n";
        renderer->push(RenderItemOpenDoc(file));
      }
    }
    state->profiling         = parser["--profile"] == true;
    state->printAssetDir     = parser["--print-asset-dir"] == true;
    state->benchmarkFrames =
        std::stoul(parser.get<std::string>("--benchmark"));
    state->cullPages   = parser["--no-cull"] == false;
    {
      const std::lock_guard locker(state->view);
      state->view.fov = std::stof(parser.get<std::string>("--fov"));
    }
    state->coarseBelow = std::stof(parser.get<std::string>("--coarse-below"));
    state->screenshotPath    = parser.get<std::string>("--screenshot");
    state->strictDiagnostics = parser["--strict-diagnostics"] == true;

    if (parser.present<std::vector<std::string>>("--toast")) {
      for (const auto &toast : parser.get<std::vector<std::string>>("--toast")) {
        state->requestedToasts.emplace_back(parseToast(toast));
      }
    }

    if (parser.present<std::vector<std::string>>("--click")) {
      for (const auto &click :
           parser.get<std::vector<std::string>>("--click")) {
        const auto comma = click.find(',');
        if (std::string::npos == comma) {
          throw std::runtime_error("--click expects X,Y, got: " + click);
        }
        state->requestedClicks.emplace_back(std::stoi(click.substr(0, comma)),
                                            std::stoi(click.substr(comma + 1)));
      }
    }
    state->typedText = parser.get<std::string>("--type");

    if (const auto span = parser.present<std::string>("--select")) {
      const auto comma = span->find(',');
      if (std::string::npos == comma) {
        throw std::runtime_error("--select expects START,END, got: " + *span);
      }
      state->requestedSelection = {
          static_cast<std::uint32_t>(std::stoul(span->substr(0, comma))),
          static_cast<std::uint32_t>(std::stoul(span->substr(comma + 1)))};
    }

    if (parser.present<std::vector<std::string>>("--pick")) {
      for (const auto &pick :
           parser.get<std::vector<std::string>>("--pick")) {
        const auto comma = pick.find(',');
        if (std::string::npos == comma) {
          throw std::runtime_error("--pick expects X,Y, got: " + pick);
        }
        state->requestedPicks.emplace_back(std::stoi(pick.substr(0, comma)),
                                           std::stoi(pick.substr(comma + 1)));
      }
    }

    return renderer;

  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    std::cerr << parser;
    return {nullptr};
  }
}

int main(const int argc, char **argv) {

  // "" signals that LC_ALL should be set from the environment
  std::setlocale(LC_ALL, "");           // for C and C++ where synced with stdio
  std::locale::global(std::locale("")); // for C++
  std::cerr.imbue(std::locale());
  std::cin.imbue(std::locale());
  std::cout.imbue(std::locale());

  const auto state = std::make_shared<AppState>();

  RendererRef rend;
  auto backend = render::Backend::OpenGL;

  if (nullptr == (rend = handleArgs(state, backend, argc, argv))) {
    return 1;
  }

  try {

    Glib::init();
    Pango::init();

    AutoSDL sdlScoped(SDL_INIT_VIDEO);

    // Answered here rather than before SDL starts, because the search asks SDL
    // where the executable is and SDL only knows once it has been initialised.
    // Before any window is created, though: a package installed on a machine
    // with no usable GL driver can still be asked whether it found its files,
    // and that is the question packaging gets wrong.
    if (state->printAssetDir) {
      std::cout << gleditor::assetDir() << "\n";
      return 0;
    }

    // Found next to the installed data files rather than in the working
    // directory, so that a packaged copy still has its icon.
    const AutoSDLSurface icon(gleditor::assetPath("logo.png").c_str());

    // The window has to be created with the flags and, for the GL family, the
    // context attributes the chosen backend needs; both are decided before the
    // device itself is constructed on the render thread.
    render::configureBackendWindowAttributes(backend);

    AutoSDLWindow window("GL Editor", state->view.screenWidth,
                         state->view.screenHeight,
                         render::backendWindowFlags(backend) |
                             SDL_WINDOW_RESIZABLE |
                             SDL_WINDOW_HIGH_PIXEL_DENSITY,
                         icon.surface);

    // Composed text rather than raw key events: this is what gives dead keys,
    // input methods and anything else the platform composes before it becomes
    // a character.
    sdl::startTextInput(window.window);

    std::jthread renderer(std::ref(*rend), std::ref(window));

    // Milliseconds to block in SDL_WaitEventTimeout. Waiting rather than
    // spinning on SDL_PollEvent keeps this thread off the CPU while idle; the
    // timeout still lets the loop notice `alive` being cleared by the renderer.
    constexpr int eventWaitMs = 100;

    while (state->alive) {
      SDL_Event evt;
      if (!SDL_WaitEventTimeout(&evt, eventWaitMs)) {
        continue;
      }
      do {
        switch (evt.type) {
        case SDL_EVENT_QUIT: {
          state->alive = false;
          break;
        }
        case SDL_EVENT_KEY_DOWN: {
          handleKeyPress(evt, state, rend);
          break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
          handleMouseMove(evt, state);
          // Motion with the left button held is a drag, which extends the
          // selection rather than moving the caret on its own.
          if (0 != (evt.motion.state & SDL_BUTTON_LMASK)) {
            state->dragX       = static_cast<int>(evt.motion.x);
            state->dragY       = static_cast<int>(evt.motion.y);
            state->dragPending = true;
          }
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
          // The render thread answers this: where a click lands in the text is
          // a question only the picking attachment can answer, and that read
          // is asynchronous.
          state->clickX       = static_cast<int>(evt.button.x);
          state->clickY       = static_cast<int>(evt.button.y);
          state->clickPending = true;
          break;
        }
        case SDL_EVENT_TEXT_INPUT: {
          const std::lock_guard locker(state->typedMutex);
          state->typedText += evt.text.text;
          break;
        }
        default: {
          // SDL2 reports every window change as one event type with a
          // sub-type, SDL3 as distinct types, so this is asked rather than
          // matched on.
          int width  = 0;
          int height = 0;
          if (!sdl::windowSizeChanged(evt, width, height)) {
            break;
          }
          std::cout << "window size changed(w/h): " << width << "/" << height
                    << "\n";
          {
            // the render thread reads these under the same lock while building
            // its projection matrix
            std::lock_guard locker(state->view);
            state->view.screenWidth  = width;
            state->view.screenHeight = height;
          }
          rend->push(RenderItemResize(width, height));
          break;
        }
        }
      } while (state->alive && SDL_PollEvent(&evt));
    }
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  // The render thread reports its own failures and cannot return a status
  // through std::jthread, so the flag it sets decides the exit code.
  return state->renderFailed ? 1 : 0;
}

// vi: set sw=2 sts=2 ts=2 et:
