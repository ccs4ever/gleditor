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
#include "core/zzcore.hpp"
#include "zigzag_commands.hpp"
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
  zigzag::registerZigzagCommands(app.commands(), viz);

  // The store is this program's, so saving and exporting it are too.
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
  const auto save = [viz] {
    if (viz->saveStore("")) {
      std::cout << "Successfully saved ZigZag store.\n";
    }
  };
  app.commands().registerAction(std::string(xanadu::settings::kKeymapSaveStore),
                                "save current slice to sovereign store", save);
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapZigzagSaveStore),
      "save current slice to sovereign store", save);

  // Every key reaches ZigZag here: there is no document to share them with.
  for (const auto &command : std::vector(app.commands().all())) {
    app.commands().setScope(command.name,
                            std::string(xanadu::keymapScope(command.name)));
  }
  app.commands().setScopeResolver(
      [] { return std::string(xanadu::kKeyScopeZigzag); });
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
    // Text input on for the omnibar and for naming cells, which take it as a
    // modal while they are open; there is no document for anything else to
    // be typed into.
    state->documentTakesText = [] { return false; };
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
    auto [hints, unused] = zigzag::zigzagKeyHints(app.commands(), {});
    viz->setKeyHints(std::move(hints), {});
    return app.run();
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    return 1;
  }
}
