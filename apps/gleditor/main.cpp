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
#include <gleditor/doc.hpp>
#include <gleditor/doc_switcher.hpp>
#include <gleditor/floating_toolbar_3d.hpp>
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

/// Animate documents along a 3D spatial orbital arc centered on the active
/// document
void updateDocumentSpatialPositions(
    RenderState &rState, const std::uint32_t activeIndex,
    const RendererRef &renderer,
    const std::shared_ptr<gleditor::DocumentSwitcher> &switcher,
    const std::shared_ptr<gleditor::FloatingToolbar3D> &toolbar) {
  if (rState.docs.empty()) {
    return;
  }
  switcher->setActiveDocIndex(activeIndex);
  toolbar->setActiveDocIndex(activeIndex);

  auto *const caret = renderer->editCaret();
  if (caret) {
    caret->placeAt(activeIndex, 0);
  }

  auto *const tl = renderer->animTimeline();
  for (std::size_t i = 0; i < rState.docs.size(); ++i) {
    auto &doc = rState.docs[i];
    if (!doc || doc->isClosing()) {
      continue;
    }
    const auto offset = static_cast<int>(i) - static_cast<int>(activeIndex);
    if (offset == 0) {
      doc->setRestingOpacity(1.0F);
      if (tl) {
        doc->animateMoveTo(*tl, glm::vec3(0.0F, 0.0F, 0.0F),
                           gleditor::anim::docArrival);
      }
    } else {
      const float spacingX = 70.0F;
      const float depthZ   = -45.0F * static_cast<float>(std::abs(offset));
      const float posX     = static_cast<float>(offset) * spacingX;
      doc->setRestingOpacity(gleditor::anim::backgroundOpacity);
      if (tl) {
        doc->animateMoveTo(*tl, glm::vec3(posX, 0.0F, depthZ),
                           gleditor::anim::docArrival);
      }
    }
  }
}

/// Bind the keys this program answers to. The camera controls come from the
/// library, since they are about the view rather than about editing.
void bindCommands(gleditor::Application &app, const AppStateRef &state,
                  const RendererRef &renderer,
                  const std::shared_ptr<gleditor::DocumentSwitcher> &switcher,
                  const std::shared_ptr<gleditor::FloatingToolbar3D> &toolbar) {
  app.bindDefaultViewCommands();
  app.commands().bind(SDL_SCANCODE_Q, "quit", "close the editor",
                      [state] { state->alive = false; });
  app.commands().bind(SDL_SCANCODE_N, Mod::Ctrl, "new",
                      "open an empty document",
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

  // Formatting keybindings
  app.commands().bind(SDL_SCANCODE_B, Mod::Ctrl, "bold",
                      "toggle bold formatting", [] {});
  app.commands().bind(SDL_SCANCODE_I, Mod::Ctrl, "italic",
                      "toggle italic formatting", [] {});
  app.commands().bind(SDL_SCANCODE_U, Mod::Ctrl, "underline",
                      "toggle underline formatting", [] {});
  app.commands().bind(
      SDL_SCANCODE_F10, "3d-overview", "toggle 3D spatial overview tray",
      [toolbar] { toolbar->setVisible(!toolbar->isVisible()); });

  // Document switching keyboard navigation
  app.commands().bind(SDL_SCANCODE_TAB, Mod::Ctrl, "next-doc",
                      "switch to next document", [renderer, switcher, toolbar] {
                        renderer->runWithState([renderer, switcher,
                                                toolbar](RenderState &rState) {
                          if (rState.docs.empty()) {
                            return;
                          }
                          const auto cur = switcher->activeDocIndex();
                          const auto next =
                              (cur + 1U) %
                              static_cast<std::uint32_t>(rState.docs.size());
                          updateDocumentSpatialPositions(rState, next, renderer,
                                                         switcher, toolbar);
                        });
                      });

  app.commands().bind(
      SDL_SCANCODE_TAB, Mod::Ctrl | Mod::Shift, "prev-doc",
      "switch to previous document", [renderer, switcher, toolbar] {
        renderer->runWithState(
            [renderer, switcher, toolbar](RenderState &rState) {
              if (rState.docs.empty()) {
                return;
              }
              const auto total = static_cast<std::uint32_t>(rState.docs.size());
              const auto cur   = switcher->activeDocIndex();
              const auto prev  = (cur + total - 1U) % total;
              updateDocumentSpatialPositions(rState, prev, renderer, switcher,
                                             toolbar);
            });
      });

  for (int i = 1; i <= 9; ++i) {
    const auto scancode = static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (i - 1));
    const auto targetIndex = static_cast<std::uint32_t>(i - 1);
    app.commands().bind(
        scancode, Mod::Ctrl, "doc-" + std::to_string(i),
        "switch to document " + std::to_string(i),
        [renderer, switcher, toolbar, targetIndex] {
          renderer->runWithState(
              [renderer, switcher, toolbar, targetIndex](RenderState &rState) {
                if (targetIndex < rState.docs.size()) {
                  updateDocumentSpatialPositions(rState, targetIndex, renderer,
                                                 switcher, toolbar);
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
    auto floatingToolbar =
        std::make_shared<gleditor::FloatingToolbar3D>(state->defaultFontName);

    docSwitcher->setCloseHandler([&renderer](const std::uint32_t docIndex) {
      renderer->push(RenderItemCloseDoc(docIndex));
    });
    docSwitcher->setSelectHandler([&renderer, &docSwitcher, &floatingToolbar](
                                      const std::uint32_t docIndex) {
      renderer->runWithState([&renderer, &docSwitcher, &floatingToolbar,
                              docIndex](RenderState &rState) {
        updateDocumentSpatialPositions(rState, docIndex, renderer, docSwitcher,
                                       floatingToolbar);
      });
    });

    floatingToolbar->setActionHandler(
        [&renderer,
         &floatingToolbar](const gleditor::FloatingToolbar3D::ButtonId btn,
                           const std::uint32_t activeDocIndex) {
          using ButtonId = gleditor::FloatingToolbar3D::ButtonId;
          switch (btn) {
          case ButtonId::NewDoc:
            renderer->push(RenderItemNewDoc());
            break;
          case ButtonId::SaveDoc:
            renderer->push(RenderItemSaveDoc(activeDocIndex));
            break;
          case ButtonId::CloseDoc:
            renderer->push(RenderItemCloseDoc(activeDocIndex));
            break;
          case ButtonId::OverviewTray:
            floatingToolbar->setVisible(!floatingToolbar->isVisible());
            break;
          case ButtonId::Bold:
          case ButtonId::Italic:
          case ButtonId::Underline:
          case ButtonId::Strikethrough:
          case ButtonId::Heading1:
          case ButtonId::Heading2:
          case ButtonId::Heading3:
          case ButtonId::FontDec:
          case ButtonId::FontInc:
          case ButtonId::AlignLeft:
          case ButtonId::AlignCenter:
          case ButtonId::AlignRight:
          case ButtonId::ListBullet:
          case ButtonId::ListNumbered:
          case ButtonId::CodeBlock:
          case ButtonId::QuoteBlock:
          case ButtonId::OpenFile:
            break;
          }
        });

    renderer->addFrameContributor(docSwitcher.get());
    renderer->addPickObserver(docSwitcher.get());
    state->accessibility->addSource(docSwitcher.get());

    renderer->addFrameContributor(floatingToolbar.get());
    renderer->addPickObserver(floatingToolbar.get());
    state->accessibility->addSource(floatingToolbar.get());

    gleditor::Application app(state, renderer, backend, "GL Editor");
    bindCommands(app, state, renderer, docSwitcher, floatingToolbar);
    return app.run();
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    return 1;
  }
}
