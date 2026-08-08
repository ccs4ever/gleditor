/**
 * @file main.cpp
 * @brief The plain editor: open files, look at them, type into them.
 *
 * Everything here is either an option this program accepts or a key it binds.
 * The window, the render thread, the event loop and the options every program
 * on this library takes belong to the library -- see gleditor/app.hpp -- which
 * is what keeps this file to the part that is actually about editing.
 */
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "config.h" // for GLEDITOR_VERSION, TOSTRING
#include <argparse/argparse.hpp>

#include <gleditor/app.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/state.hpp>

namespace {

/**
 * @brief Whether --help-all appears on the command line.
 *
 * Answered by looking rather than by parsing, because which parser to build is
 * the question it settles: a hidden argument stays hidden however it is asked
 * about, so the detailed listing has to be the one that was constructed.
 */
bool wantsEveryOption(const int argc, const char *const *const argv) {
  for (int i = 1; i < argc; i++) {
    if (nullptr != argv[i] && std::string_view{"--help-all"} == argv[i]) {
      return true;
    }
  }
  return false;
}

/// Bind the keys this program answers to. The camera controls come from the
/// library, since they are about the view rather than about editing.
void bindCommands(gleditor::Application &app, const AppStateRef &state,
                  const RendererRef &renderer) {
  app.bindDefaultViewCommands();
  app.commands().bind(SDL_SCANCODE_Q, "quit", "close the editor",
                      [state] { state->alive = false; });
  app.commands().bind(SDL_SCANCODE_N, "new", "open an empty document",
                      [renderer] { renderer->push(RenderItemNewDoc()); });
  app.commands().bind(SDL_SCANCODE_W, "close",
                      "close the most recently opened document",
                      // Which document that is, is something only the render
                      // thread knows.
                      [renderer] { renderer->push(RenderItemCloseDoc()); });
}

} // namespace

int main(const int argc, char **argv) {
  gleditor::initLocale();

  const auto state = std::make_shared<AppState>();
  const bool detailed = wantsEveryOption(argc, argv);

  argparse::ArgumentParser parser("gleditor", TOSTRING(GLEDITOR_VERSION));
  gleditor::addCommonArguments(parser, detailed);
  parser.add_argument("files").help("input files").remaining();

  // Answered before parse_args, because a request for help is the whole of
  // what that run was for. Exits rather than returns, which is what argparse's
  // own --help does.
  if (detailed) {
    std::cout << parser << "\n";
    return 0;
  }

  render::Backend backend = render::Backend::OpenGL;
  RendererRef renderer;
  try {
    parser.parse_args(argc, argv);
    backend  = gleditor::applyCommonArguments(parser, state);
    renderer = Renderer::create(state, backend);

    // Which SDL this binary was built against is not something the command line
    // can change, and a bug report is much easier to read with it stated.
    // Suppressed for --print-asset-dir, whose whole output is one path a script
    // reads: a query prints its answer and nothing else.
    if (parser["--print-asset-dir"] == false) {
      std::cout << "backend: " << render::backendName(backend) << ", SDL"
                << sdl::majorVersion << "\n";
    }

    // "files" is a remaining-argument list, which argparse leaves unset rather
    // than empty when no file is named.
    if (parser.present<std::vector<std::string>>("files")) {
      for (const auto &file : parser.get<std::vector<std::string>>("files")) {
        std::cout << "file: " << file << "\n";
        renderer->push(RenderItemOpenDoc(file));
      }
    }
  } catch (const std::exception &err) {
    std::cerr << err.what() << "\n" << parser;
    return 1;
  }

  try {
    gleditor::Application app(state, renderer, backend, "GL Editor");
    bindCommands(app, state, renderer);
    return app.run();
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    return 1;
  }
}

// vi: set sw=2 sts=2 ts=2 et:
