/**
 * @file main.cpp
 * @brief The Project Xanadu ZigZag visualizer and navigator application.
 */
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "config.h" // for GLEDITOR_VERSION, TOSTRING
#include <argparse/argparse.hpp>

#include <gleditor/android_bootstrap.hpp>
#include <gleditor/app.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/state.hpp>

#include "core/zzcore.hpp"
#include "core/zzstructure_loader.hpp"
#include "zigzag_visualizer.hpp"

#ifdef __ANDROID__
#include <SDL3/SDL_main.h>
#endif

using gleditor::Mod;
namespace fs = std::filesystem;

namespace {

bool wantsEveryOption(const int argc, const char *const *const argv) {
  for (int i = 1; i < argc; i++) {
    if (nullptr != argv[i] && std::string_view{"--help-all"} == argv[i]) {
      return true;
    }
  }
  return false;
}

std::string resolveHomeSlicePath() {
  return zigzag::zzcore::resolveXdgPath(std::getenv("XDG_CONFIG_HOME"),
                                        std::getenv("HOME"), ".config",
                                        "zigzag/home_slice.yaml");
}

void bindCommands(gleditor::Application &app, const AppStateRef &state,
                  const std::shared_ptr<zigzag::ZigzagVisualizer> &viz) {
  app.bindDefaultViewCommands();

  app.commands().bind(SDL_SCANCODE_Q, "quit", "close the visualizer",
                      [state] { state->alive = false; });

  // Multi-View Modes (Cell Content View vs. Topology View)
  app.commands().bind(
      SDL_SCANCODE_1, "view-mode-content",
      "switch to Cell Content View (full content & XYZ alignment)", [viz] {
        viz->setViewMode(zigzag::ZigzagVisualizer::ViewMode::CellContent);
      });
  app.commands().bind(
      SDL_SCANCODE_V, "view-mode-content-v",
      "switch to Cell Content View (full content & XYZ alignment)", [viz] {
        viz->setViewMode(zigzag::ZigzagVisualizer::ViewMode::CellContent);
      });
  app.commands().bind(
      SDL_SCANCODE_2, "view-mode-topology",
      "switch to Topology View (fixed-size cells & lattice geometry)", [viz] {
        viz->setViewMode(zigzag::ZigzagVisualizer::ViewMode::Topology);
      });
  app.commands().bind(
      SDL_SCANCODE_T, "view-mode-topology-t",
      "switch to Topology View (fixed-size cells & lattice geometry)", [viz] {
        viz->setViewMode(zigzag::ZigzagVisualizer::ViewMode::Topology);
      });

  // Navigation along active dimensions
  app.commands().bind(
      SDL_SCANCODE_RIGHT, "step-x-pos", "step focus positive along X dimension",
      [viz] { viz->navigateFocus(viz->currentView().x_dimension, true); });
  app.commands().bind(
      SDL_SCANCODE_LEFT, "step-x-neg", "step focus negative along X dimension",
      [viz] { viz->navigateFocus(viz->currentView().x_dimension, false); });
  app.commands().bind(
      SDL_SCANCODE_UP, "step-y-pos", "step focus positive along Y dimension",
      [viz] { viz->navigateFocus(viz->currentView().y_dimension, true); });
  app.commands().bind(
      SDL_SCANCODE_DOWN, "step-y-neg", "step focus negative along Y dimension",
      [viz] { viz->navigateFocus(viz->currentView().y_dimension, false); });
  app.commands().bind(
      SDL_SCANCODE_PAGEUP, "step-z-pos",
      "step focus positive along Z dimension",
      [viz] { viz->navigateFocus(viz->currentView().z_dimension, true); });
  app.commands().bind(
      SDL_SCANCODE_PAGEDOWN, "step-z-neg",
      "step focus negative along Z dimension",
      [viz] { viz->navigateFocus(viz->currentView().z_dimension, false); });

  // Dimension swapping and cycling
  app.commands().bind(SDL_SCANCODE_SPACE, "swap-xy",
                      "swap X and Y dimension bindings",
                      [viz] { viz->swapDimensions(0, 1); });
  app.commands().bind(SDL_SCANCODE_TAB, "cycle-dims-forward",
                      "cycle active dimension bindings forward",
                      [viz] { viz->cycleDimensions(true); });
  app.commands().bind(SDL_SCANCODE_TAB, Mod::Shift, "cycle-dims-backward",
                      "cycle active dimension bindings backward",
                      [viz] { viz->cycleDimensions(false); });

  // Preflets & History
  app.commands().bind(SDL_SCANCODE_RETURN, "follow-preflet",
                      "follow outbound Preflet at focus",
                      [viz] { viz->followPrefletAtFocus(); });
  app.commands().bind(SDL_SCANCODE_KP_ENTER, "follow-preflet-kp",
                      "follow outbound Preflet at focus",
                      [viz] { viz->followPrefletAtFocus(); });
  app.commands().bind(SDL_SCANCODE_BACKSPACE, "slice-back",
                      "return to previous Slice",
                      [viz] { viz->returnToPreviousSlice(); });
  app.commands().bind(SDL_SCANCODE_ESCAPE, "cancel-fetch",
                      "cancel in-flight Preflet download",
                      [viz] { viz->cancelPrefletFetch(); });

  // Xudu convergence operations
  app.commands().bind(SDL_SCANCODE_R, "rasterize-print",
                      "print 2D raster reading text to stdout", [viz] {
                        const auto res = viz->rasterize();
                        std::cout
                            << "\n=== ZigZag 2D Raster Reading Stream ===\n"
                            << res.text
                            << "\n=======================================\n";
                      });
  app.commands().bind(SDL_SCANCODE_S, Mod::Ctrl, "export-link-package",
                      "export current slice as Xudu LinkPackage", [viz] {
                        xudu::MutableKeys keys{};
                        const auto pkg = viz->exportAsLinkPackage(keys);
                        std::cout
                            << "Exported Xudu LinkPackage: " << pkg.describe()
                            << " (" << pkg.links.size() << " links)\n";
                      });

  // Interactive In-App Cell & Dimension Editing
  app.commands().bind(SDL_SCANCODE_N, "insert-cell-x-pos",
                      "insert connected cell positive along active X dimension",
                      [viz] {
                        viz->insertConnectedCell(
                            "New Cell", viz->currentView().x_dimension, true);
                      });
  app.commands().bind(SDL_SCANCODE_N, Mod::Shift, "insert-cell-x-neg",
                      "insert connected cell negative along active X dimension",
                      [viz] {
                        viz->insertConnectedCell(
                            "New Cell", viz->currentView().x_dimension, false);
                      });
  app.commands().bind(SDL_SCANCODE_D, "insert-cell-y-pos",
                      "insert connected cell positive along active Y dimension",
                      [viz] {
                        viz->insertConnectedCell(
                            "New Cell", viz->currentView().y_dimension, true);
                      });
  app.commands().bind(SDL_SCANCODE_D, Mod::Shift, "insert-cell-y-neg",
                      "insert connected cell negative along active Y dimension",
                      [viz] {
                        viz->insertConnectedCell(
                            "New Cell", viz->currentView().y_dimension, false);
                      });
  app.commands().bind(
      SDL_SCANCODE_U, "unlink-x-pos",
      "unlink focused cell along positive X dimension",
      [viz] { viz->unlinkFocusAlong(viz->currentView().x_dimension, true); });
  app.commands().bind(
      SDL_SCANCODE_U, Mod::Shift, "unlink-x-neg",
      "unlink focused cell along negative X dimension",
      [viz] { viz->unlinkFocusAlong(viz->currentView().x_dimension, false); });
  app.commands().bind(SDL_SCANCODE_S, Mod::Ctrl | Mod::Shift, "save-slice",
                      "save current slice to disk YAML", [viz] {
                        if (viz->saveStructureYaml("")) {
                          std::cout
                              << "Successfully saved ZigZag slice YAML.\n";
                        }
                      });
}

} // namespace

int main(const int argc, char **argv) {
#ifdef __ANDROID__
  gleditor::androidBootstrap();
#endif
  gleditor::initLocale();

  const auto state    = std::make_shared<AppState>();
  const bool detailed = wantsEveryOption(argc, argv);

  argparse::ArgumentParser parser("zigzag", TOSTRING(GLEDITOR_VERSION));
  gleditor::addCommonArguments(parser, detailed);
  parser.add_argument("--no-fetch")
      .flag()
      .help("disable BitTorrent Preflet fetching");
  parser.add_argument("--raster")
      .flag()
      .help("print 1D/2D raster text of the slice to stdout and exit");
  parser.add_argument("--xudu")
      .default_value(std::string{})
      .help("load a Xudu store path or document");
  parser.add_argument("slice").help("Slice YAML file to load").remaining();

  if (detailed) {
    std::cout << parser << "\n";
    return 0;
  }

  render::Backend backend = render::Backend::OpenGL;
  RendererRef renderer;
  bool fetchingEnabled = true;
  std::string slicePath;

  try {
    parser.parse_args(argc, argv);

    if (parser["--no-fetch"] == true) {
      fetchingEnabled = false;
    }

    bool rasterMode = false;
    if (parser["--raster"] == true || parser.is_used("--raster")) {
      rasterMode = true;
    }

    if (parser.present<std::vector<std::string>>("slice")) {
      const auto slices = parser.get<std::vector<std::string>>("slice");
      for (const auto &s : slices) {
        if (s == "--raster") {
          rasterMode = true;
        } else if (slicePath.empty() && !s.starts_with("-")) {
          slicePath = s;
        }
      }
    }

    if (rasterMode) {
      const std::string xuduPath = parser.get<std::string>("--xudu");
      zigzag::ZzStructureDocument doc;
      bool loaded = false;

      if (!xuduPath.empty() && fs::exists(xuduPath)) {
        xudu::Store store;
        store.load(xuduPath);
        auto versions = store.allVersions();
        if (versions.empty()) {
          versions.push_back(xudu::MicroversionId::parse("1"));
        }
        doc    = zigzag::projectStoreToZigzag(store, versions);
        loaded = true;
      }

      if (!loaded) {
        std::vector<std::string> candidates;
        if (!slicePath.empty()) {
          candidates.push_back(slicePath);
        } else {
          const std::string homeSlice = resolveHomeSlicePath();
          if (!homeSlice.empty()) {
            candidates.push_back(homeSlice);
          }
          candidates.push_back(
              gleditor::assetPath("zigzag/zigzag_structure.yaml"));
          candidates.push_back("assets/zigzag/zigzag_structure.yaml");
          candidates.push_back("zigzag_structure.yaml");
        }

        for (const auto &candidate : candidates) {
          if (fs::exists(candidate)) {
            if (auto loadedDoc = zigzag::loadZzStructure(candidate)) {
              doc    = std::move(*loadedDoc);
              loaded = true;
              break;
            }
          }
        }
      }

      const auto res = zigzag::rasterizeZzStructure(doc);
      std::cout << res.text << "\n";
      return 0;
    }

    backend  = gleditor::applyCommonArguments(parser, state, argc, argv);
    renderer = Renderer::create(state, backend);
  } catch (const std::exception &err) {
    std::cerr << err.what() << "\n" << parser;
    return 1;
  }

  try {
    auto viz = std::make_shared<zigzag::ZigzagVisualizer>(
        state->defaultFontName, fetchingEnabled);

    std::string xuduPath = parser.get<std::string>("--xudu");
    bool loaded          = false;

    if (!xuduPath.empty() && fs::exists(xuduPath)) {
      try {
        xudu::Store store;
        store.load(xuduPath);
        auto versions = store.allVersions();
        if (versions.empty()) {
          versions.push_back(xudu::MicroversionId::parse("1"));
        }
        viz->adoptXuduStore(store, versions);
        loaded = true;
        std::cout << "Loaded Xudu Store into ZigZag Hypermesh from: "
                  << xuduPath << " (" << versions.size() << " versions)\n";
      } catch (const std::exception &err) {
        std::cerr << "Warning: could not load Xudu store: " << err.what()
                  << "\n";
      }
    }

    if (!loaded) {
      // Resolve Slice file cascade: CLI -> Home Slice -> Bundled -> Fallback
      std::vector<std::string> candidates;
      if (!slicePath.empty()) {
        candidates.push_back(slicePath);
      } else {
        const std::string homeSlice = resolveHomeSlicePath();
        if (!homeSlice.empty()) {
          candidates.push_back(homeSlice);
        }
        candidates.push_back(
            gleditor::assetPath("zigzag/zigzag_structure.yaml"));
        candidates.push_back("assets/zigzag/zigzag_structure.yaml");
        candidates.push_back("zigzag_structure.yaml");
      }

      for (const auto &candidate : candidates) {
        if (fs::exists(candidate)) {
          if (auto doc = zigzag::loadZzStructure(candidate)) {
            viz->adoptDocument(std::move(*doc), candidate);
            loaded = true;
            std::cout << "Loaded ZigZag Slice from: " << candidate << "\n";
            break;
          }
        }
      }
    }

    if (!loaded) {
      std::cout << "Using built-in sample ZigZag structure\n";
    }

    if (parser["--raster"] == true) {
      const auto res = viz->rasterize();
      std::cout << res.text << "\n";
      return 0;
    }

    renderer->addFrameContributor(viz.get());
    renderer->addPickObserver(viz.get());
    state->accessibility->addSource(viz.get());

    gleditor::Application app(state, renderer, backend,
                              "Project Xanadu ZigZag Visualizer");
    app.setTextInputEnabled(false); // Keystrokes map to navigation commands
    bindCommands(app, state, viz);
    return app.run();
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    return 1;
  }
}
