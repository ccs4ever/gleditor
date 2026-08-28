/**
 * @file main.cpp
 * @brief The plain editor: open files, look at them, type into them.
 *
 * Everything here is either an option this program accepts or a key it binds.
 * The window, the render thread, the event loop and the options every program
 * on this library takes belong to the library -- see gleditor/app.hpp -- which
 * is what keeps this file to the part that is actually about editing.
 */
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "config.h" // for GLEDITOR_VERSION, TOSTRING
#include <argparse/argparse.hpp>

#include <gleditor/android_bootstrap.hpp>
#include <gleditor/app.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc_switcher.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/state.hpp>

// On every other platform this program's own main() is the process entry
// point. On Android it is not: SDL_main.h renames it and SDL's Java Activity
// calls the renamed function from JNI once a window surface exists, which is
// what lets the rest of this file stay unaware it is being started that way.
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>
#endif

using gleditor::Mod;

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
                  const RendererRef &renderer,
                  const std::shared_ptr<gleditor::DocumentSwitcher> &switcher) {
  app.bindDefaultViewCommands();
  app.commands().bind(SDL_SCANCODE_Q, "quit", "close the editor",
                      [state] { state->alive = false; });
  app.commands().bind(SDL_SCANCODE_N, "new", "open an empty document",
                      [renderer] { renderer->push(RenderItemNewDoc()); });
  app.commands().bind(SDL_SCANCODE_W, Mod::Ctrl, "close",
                      "close the active document", [renderer, switcher] {
                        renderer->push(
                            RenderItemCloseDoc(switcher->activeDocIndex()));
                      });
  app.commands().bind(
      SDL_SCANCODE_S, Mod::Ctrl, "save",
      "write the active document back to disk", [renderer, switcher] {
        renderer->push(RenderItemSaveDoc(switcher->activeDocIndex()));
      });

  // Document switching keyboard navigation
  app.commands().bind(
      SDL_SCANCODE_TAB, Mod::Ctrl, "next-doc", "switch to next document",
      [renderer, switcher] {
        renderer->runWithState([renderer, switcher](RenderState &rState) {
          if (rState.docs.empty()) {
            return;
          }
          const auto cur = switcher->activeDocIndex();
          const auto next =
              (cur + 1U) % static_cast<std::uint32_t>(rState.docs.size());
          switcher->setActiveDocIndex(next);
          auto *const caret = renderer->editCaret();
          if (caret) {
            caret->placeAt(next, 0);
          }
        });
      });

  app.commands().bind(
      SDL_SCANCODE_TAB, Mod::Ctrl | Mod::Shift, "prev-doc",
      "switch to previous document", [renderer, switcher] {
        renderer->runWithState([renderer, switcher](RenderState &rState) {
          if (rState.docs.empty()) {
            return;
          }
          const auto total = static_cast<std::uint32_t>(rState.docs.size());
          const auto cur   = switcher->activeDocIndex();
          const auto prev  = (cur + total - 1U) % total;
          switcher->setActiveDocIndex(prev);
          auto *const caret = renderer->editCaret();
          if (caret) {
            caret->placeAt(prev, 0);
          }
        });
      });

  for (int i = 1; i <= 9; ++i) {
    const auto scancode = static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (i - 1));
    const auto targetIndex = static_cast<std::uint32_t>(i - 1);
    app.commands().bind(
        scancode, Mod::Ctrl, "doc-" + std::to_string(i),
        "switch to document " + std::to_string(i),
        [renderer, switcher, targetIndex] {
          renderer->runWithState(
              [renderer, switcher, targetIndex](RenderState &rState) {
                if (targetIndex < rState.docs.size()) {
                  switcher->setActiveDocIndex(targetIndex);
                  auto *const caret = renderer->editCaret();
                  if (caret) {
                    caret->placeAt(targetIndex, 0);
                  }
                }
              });
        });
  }
}

} // namespace

int main(const int argc, char **argv) {
#ifdef __ANDROID__
  gleditor::androidBootstrap();
#endif
  gleditor::initLocale();

  const auto state    = std::make_shared<AppState>();
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
    backend  = gleditor::applyCommonArguments(parser, state, argc, argv);
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
    } else if (const char *openFile = std::getenv("GLEDITOR_OPEN_FILE");
               nullptr != openFile) {
      // Android has no command line: this is androidBootstrap() (called
      // above, before argument parsing) reporting whatever file the app was
      // launched or shared to open, the same way GLEDITOR_BACKEND already
      // stands in for --backend there.
      std::cout << "file: " << openFile << "\n";
      renderer->push(RenderItemOpenDoc(std::string(openFile)));
    }
  } catch (const std::exception &err) {
    std::cerr << err.what() << "\n" << parser;
    return 1;
  }

  try {
    auto docSwitcher =
        std::make_shared<gleditor::DocumentSwitcher>(state->defaultFontName);
    docSwitcher->setCloseHandler([&renderer](const std::uint32_t docIndex) {
      renderer->push(RenderItemCloseDoc(docIndex));
    });
    docSwitcher->setSelectHandler([&renderer](const std::uint32_t docIndex) {
      renderer->runWithState([&renderer, docIndex](RenderState &rState) {
        if (docIndex < rState.docs.size() && rState.docs[docIndex]) {
          auto *const caret = renderer->editCaret();
          if (caret) {
            caret->placeAt(docIndex, 0);
          }
        }
      });
    });

    renderer->addFrameContributor(docSwitcher.get());
    renderer->addPickObserver(docSwitcher.get());
    state->accessibility->addSource(docSwitcher.get());

    gleditor::Application app(state, renderer, backend, "GL Editor");
    bindCommands(app, state, renderer, docSwitcher);
    return app.run();
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    return 1;
  }
}
