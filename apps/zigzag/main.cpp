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
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/state.hpp>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/zzcore.hpp"
#include "zigzag_visualizer.hpp"

#ifdef __ANDROID__
#include <SDL3/SDL_main.h>
#endif

using gleditor::Mod;
using zigzag::DimVector;
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

struct LoadedDocument {
  std::optional<zigzag::ZzStructureDocument> doc;
  std::string sourcePath;
  std::string description;
};

LoadedDocument loadDocument(const std::string &slicePath,
                            const std::string &xuduPath) {
  if (!xuduPath.empty() && fs::exists(xuduPath)) {
    try {
      xanadu::Store store;
      store.load(xuduPath);
      auto versions = store.allVersions();
      if (versions.empty()) {
        versions.push_back(xanadu::MicroversionId::parse("1"));
      }
      auto doc = zigzag::projectStoreToZigzag(store, versions);
      return {.doc        = std::move(doc),
              .sourcePath = xuduPath,
              .description =
                  "Loaded Xudu Store into ZigZag Hypermesh from: " + xuduPath +
                  " (" + std::to_string(versions.size()) + " versions)"};
    } catch (const std::exception &err) {
      std::cerr << "Warning: could not load Xudu store: " << err.what() << "\n";
    }
  }

  if (!slicePath.empty() && fs::exists(slicePath)) {
    try {
      xanadu::Store store;
      store.load(slicePath);
      auto versions = store.allVersions();
      if (versions.empty()) {
        versions.push_back(xanadu::MicroversionId::parse("1"));
      }
      auto doc = zigzag::projectStoreToZigzag(store, versions);
      return {.doc        = std::move(doc),
              .sourcePath = slicePath,
              .description =
                  "Loaded sovereign Store into ZigZag Hypermesh from: " +
                  slicePath + " (" + std::to_string(versions.size()) +
                  " versions)"};
    } catch (const std::exception &err) {
      std::cerr << "Warning: could not load Store: " << err.what() << "\n";
    }
  }

  return {.doc = std::nullopt, .sourcePath = {}, .description = {}};
}

std::unique_ptr<xanadu::Store> loadOrCreateKeymapStore() {
  const auto dir = xanadu::systemDocDirectory(xanadu::SystemDocKind::Keymap);
  std::filesystem::create_directories(dir);

  auto sysStore = std::make_unique<xanadu::Store>();
  sysStore->setSystem(true);

  bool opened = false;
  if (std::filesystem::exists(dir / "ops.nodes") ||
      std::filesystem::exists(dir / "store.tables")) {
    try {
      sysStore->load(dir.string());
      opened = true;
    } catch (...) { // NOLINT(bugprone-empty-catch)
      // Recreate if unreadable
    }
  }
  if (opened) {
    if (sysStore->currentVersions().empty() && !sysStore->latest().isZero()) {
      sysStore->repointCurrentVersion(sysStore->latest());
    }
  } else {
    xanadu::initializeSystemStore(*sysStore, xanadu::SystemDocKind::Keymap);
    sysStore->save(dir.string());
  }
  return sysStore;
}

void bindCommands(gleditor::Application &app, const AppStateRef &state,
                  const std::shared_ptr<zigzag::ZigzagVisualizer> &viz) {
  app.commands().registerAction(std::string(xanadu::settings::kKeymapQuit),
                                "close the visualizer",
                                [state] { state->alive = false; });

  // Multi-View Modes (Cell Content View vs. Topology View)
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapViewModeContent1),
      "switch to Cell Content View (full content & XYZ alignment)", [viz] {
        viz->setViewMode(zigzag::ZigzagVisualizer::ViewMode::CellContent);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapViewModeContentV),
      "switch to Cell Content View (full content & XYZ alignment)", [viz] {
        viz->setViewMode(zigzag::ZigzagVisualizer::ViewMode::CellContent);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapViewModeTopology),
      "switch to Topology View (fixed-size cells & lattice geometry)", [viz] {
        viz->setViewMode(zigzag::ZigzagVisualizer::ViewMode::Topology);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapViewModeTopologyT),
      "switch to Topology View (fixed-size cells & lattice geometry)", [viz] {
        viz->setViewMode(zigzag::ZigzagVisualizer::ViewMode::Topology);
      });

  // Dimension Bundle switching
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapBundleExecution),
      "switch to Execution dimension bundle (d.spin, d.step, d.branch)", [viz] {
        viz->setDimensionBundle(
            zigzag::ZigzagVisualizer::DimensionBundle::Execution);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapBundleScope),
      "switch to Scope dimension bundle (d.lexical, d.dynamic, d.env)", [viz] {
        viz->setDimensionBundle(
            zigzag::ZigzagVisualizer::DimensionBundle::Scope);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapBundleContract),
      "switch to Contract dimension bundle (d.require, d.ensure, d.invariant)",
      [viz] {
        viz->setDimensionBundle(
            zigzag::ZigzagVisualizer::DimensionBundle::Contract);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapBundleLogic),
      "switch to Logic dimension bundle (d.clause, d.predicate, d.var)", [viz] {
        viz->setDimensionBundle(
            zigzag::ZigzagVisualizer::DimensionBundle::Logic);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapBundleStdlib),
      "switch to Stdlib dimension bundle (d.stdlib, d.symbol, d.version)",
      [viz] {
        viz->setDimensionBundle(
            zigzag::ZigzagVisualizer::DimensionBundle::Stdlib);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapBundleCycle),
      "cycle active dimension bundle forward",
      [viz] { viz->cycleDimensionBundle(true); });

  // Vortex Opcode & Library Palette HUD
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapTogglePalette),
      "toggle Vortex opcode and library palette HUD",
      [viz] { viz->togglePalette(); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapVqlTranslateAttach),
      "translate VQL filter text and attach to active chain", [viz] {
        if (viz->isPaletteVisible()) {
          viz->paletteTranslateVQL();
          viz->setPaletteVisible(false);
        }
      });

  // Interactive VQL Command Omnibar
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapToggleCommandBar),
      "toggle interactive VQL Command Omnibar",
      [viz] { viz->toggleCommandBar(); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapOpenCommandBarSlash),
      "open VQL Command Omnibar with '/' navigation prefix", [viz] {
        viz->setCommandBarVisible(true);
        if (viz->commandBarText().empty()) {
          viz->commandBarInputChar('/');
        }
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapOpenCommandBarColon),
      "open VQL Command Omnibar with ':' command prefix", [viz] {
        viz->setCommandBarVisible(true);
        if (viz->commandBarText().empty()) {
          viz->commandBarInputChar(':');
        }
      });

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapConfirmAction),
      "execute Command Omnibar or clone selected palette symbol", [viz] {
        if (viz->isCommandBarVisible()) {
          viz->executeCommandBar();
        } else if (viz->isPaletteVisible()) {
          viz->paletteCloneSelectedToFocus();
          viz->setPaletteVisible(false);
        }
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapDismissOverlay),
      "dismiss Command Omnibar or palette HUD", [viz] {
        if (viz->isCommandBarVisible()) {
          viz->setCommandBarVisible(false);
        } else if (viz->isPaletteVisible()) {
          viz->setPaletteVisible(false);
        }
      });

  // Navigation along active dimensions
  app.commands().registerAction(std::string(xanadu::settings::kKeymapStepXPos),
                                "step focus positive along X dimension",
                                [viz] { viz->dispatchAction("step-x-pos"); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapStepXNeg),
                                "step focus negative along X dimension",
                                [viz] { viz->dispatchAction("step-x-neg"); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapStepYPos),
                                "step focus positive along Y dimension", [viz] {
                                  if (viz->isPaletteVisible()) {
                                    viz->palettePrev();
                                  } else {
                                    viz->dispatchAction("step-y-pos");
                                  }
                                });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapStepYNeg),
                                "step focus negative along Y dimension", [viz] {
                                  if (viz->isPaletteVisible()) {
                                    viz->paletteNext();
                                  } else {
                                    viz->dispatchAction("step-y-neg");
                                  }
                                });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapStepZPos),
                                "step focus positive along Z dimension",
                                [viz] { viz->dispatchAction("step-z-pos"); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapStepZNeg),
                                "step focus negative along Z dimension",
                                [viz] { viz->dispatchAction("step-z-neg"); });

  // Dimension swapping and cycling
  app.commands().registerAction(std::string(xanadu::settings::kKeymapSwapXY),
                                "swap X and Y dimension bindings",
                                [viz] { viz->dispatchAction("swap-xy"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapCycleDimsForward),
      "cycle active dimension bindings forward",
      [viz] { viz->dispatchAction("cycle-dims-forward"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapCycleDimsBackward),
      "cycle active dimension bindings backward",
      [viz] { viz->dispatchAction("cycle-dims-backward"); });

  // Navigation jumping
  app.commands().registerAction(std::string(xanadu::settings::kKeymapJumpHome),
                                "jump focus to home cell",
                                [viz] { viz->dispatchAction("jump-home"); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapHopHead),
                                "hop to head of current rank along X dimension",
                                [viz] { viz->dispatchAction("hop-head"); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapHopTail),
                                "hop to tail of current rank along X dimension",
                                [viz] { viz->dispatchAction("hop-tail"); });

  // Duplication
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapDuplicateFocusCell),
      "duplicate focused cell along d.clone",
      [viz] { viz->dispatchAction("duplicate-focus-cell"); });

  // Xudu convergence operations
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapRasterizePrint),
      "print 2D raster reading text to stdout", [viz] {
        const auto res = viz->rasterize();
        std::cout << "\n=== ZigZag 2D Raster Reading Stream ===\n"
                  << res.text << "\n=======================================\n";
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapExportLinkPackage),
      "export current slice as Xudu LinkPackage", [viz] {
        xanadu::MutableKeys keys{};
        const auto pkg = viz->exportAsLinkPackage(keys);
        std::cout << "Exported Xudu LinkPackage: " << pkg.describe() << " ("
                  << pkg.links.size() << " links)\n";
      });

  // Interactive In-App Cell & Dimension Editing
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapInsertCellXPos),
      "insert connected cell positive along active X dimension",
      [viz] { viz->dispatchAction("insert-cell-x-pos"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapInsertCellXNeg),
      "insert connected cell negative along active X dimension",
      [viz] { viz->dispatchAction("insert-cell-x-neg"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapInsertCellYPos),
      "insert connected cell positive along active Y dimension",
      [viz] { viz->dispatchAction("insert-cell-y-pos"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapInsertCellYNeg),
      "insert connected cell negative along active Y dimension",
      [viz] { viz->dispatchAction("insert-cell-y-neg"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapUnlinkXPos),
      "unlink focused cell along positive X dimension",
      [viz] { viz->dispatchAction("unlink-x-pos"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapUnlinkXNeg),
      "unlink focused cell along negative X dimension",
      [viz] { viz->dispatchAction("unlink-x-neg"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapDeleteFocusCell),
      "delete currently focused cell",
      [viz] { viz->dispatchAction("delete-focus-cell"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapDeleteFocusCellBksp),
      "delete currently focused cell",
      [viz] { viz->dispatchAction("delete-focus-cell"); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapSaveStore),
                                "save current slice to sovereign store", [viz] {
                                  if (viz->saveStore("")) {
                                    std::cout
                                        << "Successfully saved ZigZag store.\n";
                                  }
                                });
}

} // namespace

// Catches std::exception and reports it; anything else (a real bug, not a
// user-facing failure) is deliberately left to terminate with a backtrace
// rather than be swallowed into a generic error message.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(const int argc, char **argv) {
#ifdef __ANDROID__
  gleditor::androidBootstrap();
#endif
  gleditor::initLocale();

  const auto state = std::make_shared<AppState>();
  // zigzag draws its own cells through its own FrameContributor, not Doc pages
  // -- RenderState::docs stays empty for the whole run by design,
  // so the default (waiting for it to be non-empty and loaded) would make
  // --profile/--screenshot wait on a condition that can never become true.
  state->usesDocPages = false;
  const bool detailed = wantsEveryOption(argc, argv);

  argparse::ArgumentParser parser("zigzag", TOSTRING(GLEDITOR_VERSION));
  gleditor::addCommonArguments(parser, detailed);
  parser.add_argument("--xudu")
      .default_value(std::string{})
      .help("load a Xudu store path or document");
  parser.add_argument("slice").help("Store to load").remaining();

  if (detailed) {
    parser.add_group("Batch and export options");
  }

  auto &rasterArg =
      parser.add_argument("--raster")
          .flag()
          .help("print 1D/2D raster text of the slice to stdout and exit");
  if (!detailed) {
    rasterArg.hidden();
  }

  if (detailed) {
    std::cout << parser << "\n";
    return 0;
  }

  render::Backend backend = render::Backend::OpenGL;
  RendererRef renderer;
  std::string slicePath;

  try {
    parser.parse_args(argc, argv);

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

    const auto xuduPath = parser.get<std::string>("--xudu");

    if (rasterMode) {
      auto loaded = loadDocument(slicePath, xuduPath);
      zigzag::ZzStructureDocument doc =
          loaded.doc ? std::move(*loaded.doc) : zigzag::ZzStructureDocument{};
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
    auto viz =
        std::make_shared<zigzag::ZigzagVisualizer>(state->defaultFontName);

    const auto xuduPath = parser.get<std::string>("--xudu");
    auto loaded         = loadDocument(slicePath, xuduPath);
    if (loaded.doc) {
      viz->adoptDocument(std::move(*loaded.doc), loaded.sourcePath);
      if (!loaded.description.empty()) {
        std::cout << loaded.description << "\n";
      }
    } else {
      std::cout << "Using built-in sample ZigZag structure\n";
    }

    renderer->addFrameContributor(viz.get());
    renderer->addPickObserver(viz.get());
    state->accessibility->addSource(viz.get());
    state->modal = viz.get();

    gleditor::Application app(state, renderer, backend,
                              "Project Xanadu ZigZag Visualizer");
    app.setTextInputEnabled(false); // Keystrokes map to navigation commands
    bindCommands(app, state, viz);

    auto keymapStore = loadOrCreateKeymapStore();
    if (keymapStore && keymapStore->opCount() > 0) {
      const auto kmCfg = xanadu::KeymapConfig::fromStore(*keymapStore);
      for (const auto &[act, comboStr] : kmCfg.bindings) {
        if (const auto combo = gleditor::parseKeyCombo(comboStr)) {
          app.commands().rebind(act, combo->first, combo->second);
        }
      }
    }
    return app.run();
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    return 1;
  }
}
