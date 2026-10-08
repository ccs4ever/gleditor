/**
 * @file xuzz_app.cpp
 * @brief Sovereign application orchestrator unifying Xanadoc and Zigzag.
 */
#include "xuzz_app.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "config.h" // for GLEDITOR_VERSION, TOSTRING
#include <argparse/argparse.hpp>
#include <gleditor/app.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc_switcher.hpp>
#include <gleditor/form.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/radial_menu.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/state.hpp>
#include <gleditor/ui/focus_manager.hpp>

#include "cli.hpp"
#include "view_coordinator.hpp"

#include "common/ui/quotation_builder_overlay.hpp"
#include "common/ui/store_object_manager.hpp"
#include "common/xanadu/config.hpp"
#include "common/xanadu/framing.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/provenance.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/torrent.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/zigzag/zz_xudu_projector.hpp"
#include "common/xanadu/zigzag/zzcore.hpp"

#include "common/ui/hypertime_graph.hpp"
#include "common/ui/slice/zigzag_commands.hpp"
#include "common/ui/slice/zigzag_visualizer.hpp"
#include "common/ui/view/slice_presentation.hpp"
#include "common/ui/xanadoc/batch_orchestrator.hpp"
#include "common/ui/xanadoc/beams.hpp"
#include "common/ui/xanadoc/bridge_coordinator.hpp"
#include "common/ui/xanadoc/kinetic_tether_overlay.hpp"
#include "common/ui/xanadoc/link_context.hpp"
#include "common/ui/xanadoc/link_panel_overlay.hpp"
#include "common/ui/xanadoc/overview_overlay.hpp"
#include "common/ui/xanadoc/pouch_drawer.hpp"
#include "common/ui/xanadoc/satelloid.hpp"
#include "common/ui/xanadoc/session.hpp"
#include "common/ui/xanadoc/swarm_telescope_overlay.hpp"
#include "common/ui/xanadoc/tenuous_tether.hpp"
#include "common/ui/xanadoc/views.hpp"
#include "common/ui/xanadoc/wireframe_hull.hpp"
#include "common/xanadu/link_navigation.hpp"
#include "common/xanadu/link_panel.hpp"
#include "common/xanadu/reading_place.hpp"
#include <gleditor/caret_motion.hpp>
#include <gleditor/logging.hpp>

namespace fs = std::filesystem;

namespace xuzz {

namespace {

constexpr float kBackgroundDepthZ = -500.0F;

void applyKeymap(
    gleditor::CommandTable &commands, const xanadu::Store &store,
    const std::shared_ptr<xanadu::view::SlicePresentation> &zigzagPresentation =
        nullptr,
    const std::shared_ptr<zigzag::vortex::VortexHost> &vHost = nullptr) {
  if (vHost) {
    vHost->loadMacrosFromStore(store);
    for (const auto &macroName : vHost->listMacros()) {
      commands.registerOrRebindAction(
          macroName, "User macro: " + macroName,
          [zigzagPresentation, vHost, macroName] {
            if (zigzagPresentation) {
              std::ignore = zigzagPresentation->dispatchAction(macroName);
            } else if (vHost) {
              std::ignore = vHost->dispatchAction(macroName);
            }
          });
    }
  }

  for (const auto &[act, comboStr] :
       xanadu::KeymapConfig::fromStore(store).bindings) {
    const auto combo = gleditor::parseKeyCombo(comboStr);
    if (!combo) {
      GLEDITOR_LOG_WARN("xuzz.keymap", "{}: \"{}\" is not a key combination",
                        act, comboStr);
      continue;
    }

    if (commands.rebind(act, combo->first, combo->second)) {
      GLEDITOR_LOG_DEBUG("xuzz.keymap", "{} bound to {}", act, comboStr);
      continue;
    }

    const auto canonical = xanadu::canonicalKeymapAction(act);
    if (canonical != act &&
        commands.rebind(canonical, combo->first, combo->second)) {
      GLEDITOR_LOG_DEBUG("xuzz.keymap", "{} ({}) bound to {}", canonical, act,
                         comboStr);
      continue;
    }

    const auto legacy = xanadu::legacyKeymapAction(act);
    if (legacy != act && commands.rebind(legacy, combo->first, combo->second)) {
      GLEDITOR_LOG_DEBUG("xuzz.keymap", "{} ({}) bound to {}", legacy, act,
                         comboStr);
      continue;
    }

    if (zigzagPresentation || vHost) {
      commands.registerOrRebindAction(
          act, "Custom keymap action: " + act,
          [zigzagPresentation, vHost, act] {
            if (zigzagPresentation) {
              std::ignore = zigzagPresentation->dispatchAction(act);
            } else if (vHost) {
              std::ignore = vHost->dispatchAction(act);
            }
          },
          combo->first, combo->second);
      // Registered after the start-up pass that scopes every built-in
      // command, so it must take its scope here: left unscoped, a pane's
      // action such as std:zigzag/save_store competes with the global one on
      // its chord, and the global one wins even in that pane.
      commands.setScope(act, std::string(xanadu::keymapScope(act)));
      GLEDITOR_LOG_DEBUG("xuzz.keymap", "dynamically registered {} bound to {}",
                         act, comboStr);
    } else {
      GLEDITOR_LOG_DEBUG("xuzz.keymap", "{}: not a command in this program",
                         act);
    }
  }

  for (const auto &[kept, shadowed] : commands.conflicts()) {
    GLEDITOR_LOG_WARN("xuzz.keymap",
                      "{} and {} are on the same key; only {} can run", kept,
                      shadowed, kept);
  }
}

class KeyboardPane : public gleditor::PickObserver {
public:
  explicit KeyboardPane(AppStateRef aState) : state(std::move(aState)) {}

  [[nodiscard]] bool inZigzag() const noexcept { return zigzag.load(); }

  [[nodiscard]] std::string scope() const {
    return std::string(zigzag.load() ? xanadu::kKeyScopeZigzag
                                     : xanadu::kKeyScopeDocument);
  }

  void setChangeHandler(std::function<void(bool)> handler) {
    changed = std::move(handler);
  }

  void enterZigzag() {
    if (!zigzag.exchange(true)) {
      {
        std::scoped_lock locker(state->view);
        documentCamera = state->view.pos;
      }
      if (changed) {
        changed(true);
      }
    }
  }

  void leaveZigzag(const bool restoreCamera) {
    if (!zigzag.exchange(false)) {
      return;
    }
    if (restoreCamera) {
      std::scoped_lock locker(state->view);
      state->view.pos = documentCamera;
    }
    if (changed) {
      changed(false);
    }
  }

  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState & /*state*/) override {
    if (render::tagKindGlyph == pick.tag.kind ||
        render::tagKindPage == pick.tag.kind) {
      leaveZigzag(false);
    } else if (render::tagKindOverlay == pick.tag.kind && pick.semanticTarget &&
               pick.semanticTarget->cellRef) {
      enterZigzag();
    }
    return false;
  }

private:
  AppStateRef state;
  std::atomic<bool> zigzag{false};
  glm::vec3 documentCamera{};
  std::function<void(bool)> changed;
};

} // namespace

XuzzApp::XuzzApp()  = default;
XuzzApp::~XuzzApp() = default;

int XuzzApp::executeCheckAuthorship(const std::string &where) {
  const fs::path given(where);
  const auto record =
      fs::is_directory(given) ? given / xanadu::provenanceFileName : given;
  const auto sig = fs::path(record.string() + ".asc");

  const auto slurp = [](const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
  };
  xanadu::SignedProvenance sealed{.tsv       = slurp(record),
                                  .signature = slurp(sig)};
  if (sealed.tsv.empty()) {
    std::cerr << "no authorship record at " << record << "\n";
    return 1;
  }

  const auto check = xanadu::verifyProvenance(sealed);
  std::cout << sealed.tsv;
  if (!check.signatureValid) {
    std::cout << "\nxuzz: this record is NOT vouched for -- " << check.detail
              << "\n";
    return 1;
  }
  std::cout << "\nxuzz: signed by " << check.signer << "\n"
            << "      key " << check.fingerprint << "\n"
            << "      "
            << (check.keyTrusted
                    ? "which is a key this keyring trusts"
                    : "which this keyring has no reason to trust -- the "
                      "signature is real, but that it is this person's key is "
                      "only their say-so")
            << "\n";

  if (const auto said = xanadu::parseProvenance(sealed.tsv); said) {
    const auto content = record.parent_path() / xanadu::sealedContentName;
    if (const auto bytes = slurp(content); !bytes.empty()) {
      const auto matches = xanadu::sha256Hex(bytes) == said->contentDigest &&
                           bytes.size() == said->contentLength;
      std::cout << "      "
                << (matches ? "and it is about the content sealed with it"
                            : "BUT THE CONTENT BESIDE IT IS NOT WHAT IT "
                              "DESCRIBES")
                << "\n";
      if (!matches) {
        return 1;
      }
    }
    const auto ops = record.parent_path() / xanadu::sealedOpsName;
    if (said->opsDigest.empty() && 0 == said->opsLength) {
      std::cout
          << "      but it says nothing about the history sealed beside it\n";
      return 0;
    }
    const auto opsBytes  = slurp(ops);
    const auto opsAgrees = xanadu::sha256Hex(opsBytes) == said->opsDigest &&
                           opsBytes.size() == said->opsLength;
    std::cout << "      "
              << (opsAgrees
                      ? "and about the history sealed beside it"
                      : "BUT THE HISTORY BESIDE IT IS NOT WHAT IT DESCRIBES")
              << "\n";
    return opsAgrees ? 0 : 1;
  }
  return 0;
}

int XuzzApp::executeRaster(const std::string &slicePath,
                           const std::string &storePath) {
  const std::string pathToUse = !slicePath.empty() ? slicePath : storePath;
  if (pathToUse.empty() || !fs::exists(pathToUse)) {
    std::cerr << "Error: Store path for rasterization does not exist: "
              << pathToUse << "\n";
    return 1;
  }

  try {
    xanadu::Store store;
    store.load(pathToUse);
    auto versions = store.allVersions();
    if (versions.empty()) {
      versions.push_back(xanadu::MicroversionId::parse("1"));
    }
    auto doc       = zigzag::projectStoreToZigzag(store, versions);
    const auto res = zigzag::rasterizeZzStructure(doc);
    std::cout << res.text << "\n";
    return 0;
  } catch (const std::exception &err) {
    std::cerr << "Error during rasterization: " << err.what() << "\n";
    return 1;
  }
}

int XuzzApp::run(const int argc, char **argv) {
  const auto state    = std::make_shared<AppState>();
  const bool detailed = CliParser::wantsEveryOption(argc, argv);

  argparse::ArgumentParser parser("xuzz", TOSTRING(GLEDITOR_VERSION));
  CliParser::buildParser(parser, detailed);
  if (detailed) {
    std::cout << parser << "\n";
    return 0;
  }

  const auto maybeOpts = CliParser::parse(parser, state, argc, argv);
  if (!maybeOpts) {
    return 1;
  }
  auto opts = *maybeOpts;

  if (opts.showConfig) {
    std::cout << "# " << xanadu::configPath() << "\n"
              << xanadu::loadConfig().toTsv();
    return 0;
  }

  if (!opts.checkAuthorshipPath.empty()) {
    return executeCheckAuthorship(opts.checkAuthorshipPath);
  }

  if (opts.rasterMode) {
    return executeRaster(opts.slicePath, opts.storePath);
  }

  // 1. Initialize sovereign Permascroll
  std::shared_ptr<xanadu::UserPermascroll> userPermascroll;
  if (!opts.permascrollPath.empty()) {
    xanadu::UserPermascroll::Config config;
    config.storageDir = opts.permascrollPath;
    userPermascroll =
        std::make_shared<xanadu::UserPermascroll>(std::move(config));
  } else {
    userPermascroll = xanadu::PermascrollRegistry::instance().defaultUser();
  }

  // 2. Initialize Session
  auto session =
      std::make_unique<xanadu::Session>(opts.storePath, userPermascroll);
  if (!opts.testPublicationSwarm.empty()) {
    std::vector<std::pair<std::string, std::uint16_t>> nodes;
    for (const auto &node :
         parser.get<std::vector<std::string>>("--dht-node")) {
      const auto colon = node.rfind(':');
      if (colon == std::string::npos)
        throw std::invalid_argument("DHT node requires HOST:PORT");
      std::size_t consumed = 0;
      const auto number    = std::stoul(node.substr(colon + 1), &consumed);
      if (consumed != node.size() - colon - 1 || number == 0 || number > 65535)
        throw std::invalid_argument("invalid DHT node port");
      nodes.emplace_back(node.substr(0, colon),
                         static_cast<std::uint16_t>(number));
    }
    session->configureTestPublicationSwarm(opts.testPublicationSwarm,
                                           std::move(nodes));
  }
  state->onDecoratedInsert = [&session](Doc &doc, const std::uint32_t at,
                                        const std::uint32_t length,
                                        const gleditor::DecorationMask mask) {
    session->markDecorated(doc, at, length, mask);
  };

  // 3. Batch Orchestration
  const auto batchRes =
      xanadu::BatchOrchestrator::execute(*session, parser, opts.quiet);
  if (batchRes.shouldExit) {
    return batchRes.exitCode;
  }
  std::vector<std::pair<std::size_t, xanadu::MicroversionId>> readPublications;
  std::size_t openingStore = 0;
  auto opening             = batchRes.opening;
  const auto extraImports  = batchRes.extraImports;

  if (parser.present<std::vector<std::string>>("--read")) {
    for (const auto &file : parser.get<std::vector<std::string>>("--read")) {
      const auto opened = session->readPublication(file);
      readPublications.push_back(opened);
      opts.read.push_back(opened.second);
    }
    session->save(0);
  }

  if (opening.isZero()) {
    if (!opts.askedVersion.empty()) {
      opening = xanadu::MicroversionId::parse(opts.askedVersion);
    } else if (!opts.read.empty()) {
      opening      = opts.read.front();
      openingStore = readPublications.front().first;
    } else {
      opening = session->store(0).latest();
    }
  }

  if (!opts.publishAs.empty()) {
    const auto manifest = session->publishDocument(
        opening,
        xanadu::Session::PublishRequest{.salt       = opts.publishAs,
                                        .title      = opts.publishAs,
                                        .author     = {},
                                        .extra      = {},
                                        .passphrase = {}},
        openingStore);
    if (!opts.quiet) {
      std::cout << "xudu: prepared " << opening.str() << " as " << manifest
                << "\n";
    }
  }

  if (!opts.quiet) {
    const std::string progName = fs::path(argv[0]).filename().string();
    std::cout << progName << " " << TOSTRING(GLEDITOR_VERSION) << ": "
              << session->store(0).opCount() << " operations in "
              << session->path(0) << ", opening " << opening.str() << "\n";
  }

  if (opts.headless) {
    session->saveAll();
    return 0;
  }

  // 4. Renderer & Graphical Subsystems
  auto renderer = Renderer::create(state, opts.backend);

  xanadu::ui::HypertimeGraph map(
      "",
      [&session](const std::size_t index) -> const xanadu::Store & {
        return session->store(index);
      },
      [&session] { return session->generation(); });
  map.setVisible(opts.mapVisible);

  xanadu::PouchDrawer pouchDrawer(*session, renderer);
  pouchDrawer.setOpen(opts.pouchOpen, false);

  if (!opts.aliases.empty()) {
    for (const auto &spec : opts.aliases) {
      const auto colon = spec.find(':');
      if (colon != std::string::npos) {
        const auto verStr   = spec.substr(0, colon);
        const auto aliasStr = spec.substr(colon + 1);
        const auto targetId = xanadu::MicroversionId::parse(verStr);
        auto &st            = session->store(0);
        try {
          const auto latest = st.latest();
          if (!latest.isZero()) {
            st.designateEdition(latest, aliasStr, targetId);
          } else {
            st.setVersionAnnotation(targetId, {.alias       = aliasStr,
                                               .description = {},
                                               .tag         = {},
                                               .timestamp   = {}});
          }
        } catch (...) {
          st.setVersionAnnotation(targetId, {.alias       = aliasStr,
                                             .description = {},
                                             .tag         = {},
                                             .timestamp   = {}});
        }
      }
    }
  }

  if (!opts.compares.empty()) {
    for (const auto &spec : opts.compares) {
      std::stringstream ss(spec);
      std::string item;
      while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
          map.toggleComparison(xanadu::MicroversionId::parse(item));
        }
      }
    }
  }

  xanadu::ImageOverlay images{std::string{}};
  auto docSwitcher = std::make_shared<gleditor::DocumentSwitcher>();
  gleditor::Form publishForm;

  xanadu::Views views(*session, renderer, map, images, publishForm, state,
                      docSwitcher);

  xanadu::SwarmCatalog swarmCatalog;
  views.setPublicationCatalog(&swarmCatalog);
  try {
    (void)session->publicationSubscriptions();
  } catch (const std::exception &error) {
    GLEDITOR_LOG_DEBUG("xudu.publication", "Subscription cache refused: {}",
                       error.what());
  }
  try {
    for (const auto &key : session->publicationDiscovery().followedAuthors())
      swarmCatalog.followAuthor(key);
    for (const auto &catalog : session->publicationDiscovery().cachedCatalogs())
      swarmCatalog.ingestAuthorCatalog(catalog);
  } catch (const std::exception &error) {
    GLEDITOR_LOG_DEBUG("xudu.discovery", "Cached catalog refused: {}",
                       error.what());
  }
  xanadu::SwarmTelescopeOverlay swarmTelescope(swarmCatalog, renderer, "");
  swarmTelescope.setOnSummon([&views](const xanadu::PublicationEntry &entry) {
    views.summonPublication(entry);
  });
  swarmTelescope.setOnDiscover([&views](const std::string &query) {
    views.discoverPublications(query);
  });
  if (opts.telescopeVisible) {
    swarmTelescope.setVisible(true);
  }

  xanadu::QuotationBuilderOverlay quotationOverlay(
      session->store(),
      session->views().empty() ? xanadu::MicroversionId{}
                               : session->versionOf(0),
      renderer, &swarmCatalog, "",
      [&session] {
        std::vector<xanadu::Store *> openStores;
        for (std::size_t i = 0; i < session->storeCount(); ++i) {
          openStores.push_back(&session->store(i));
        }
        return openStores;
      },
      [&session](const xanadu::MicroversionId newVersion,
                 const zigzag::CellRef /*quotationCell*/) {
        if (!session->views().empty()) {
          auto &view   = session->views()[0];
          view.version = newVersion;
          view.pieces  = session->store().rebuild(newVersion);
        }
      });

  xanadu::StoreObjectManager storeObjectManager(
      session->store(), "",
      [&views, &session](const std::uint32_t birthOp,
                         const xanadu::StructureKind /*kind*/,
                         const bool shouldBeOpen) {
        if (!shouldBeOpen) {
          for (std::size_t i = 0; i < session->views().size(); ++i) {
            if (session->views()[i].focusedBirth == birthOp) {
              views.closeDocument(static_cast<std::uint32_t>(i));
              break;
            }
          }
        } else {
          const auto ver = session->store().latest();
          views.showAlongside(ver, 0.0F, 0, birthOp);
        }
      },
      [&views, &session](const xanadu::StructureKind kind) {
        auto &st          = session->store();
        const auto parent = st.latest();
        xanadu::MicroversionId newVer;
        std::uint32_t newBirth = 0;
        if (kind == xanadu::StructureKind::Slice) {
          const auto name = "Slice " + std::to_string(st.opCount() + 1);
          if (st.homeCell() == zigzag::noCell) {
            newVer = st.sliceGenesis(parent, name);
          } else {
            newVer = st.makeSlice(parent, name);
          }
          newBirth = static_cast<std::uint32_t>(st.opCount());
        } else {
          const auto name = "Document " + std::to_string(st.opCount() + 1);
          newVer          = st.makeXanadoc(parent, name);
          newBirth        = static_cast<std::uint32_t>(st.opCount());
        }
        views.showAlongside(newVer, 0.0F, 0, newBirth);
      },
      [&views, &session](const std::uint32_t birthOp) {
        for (std::size_t i = 0; i < session->views().size(); ++i) {
          if (session->views()[i].focusedBirth == birthOp) {
            views.closeDocument(static_cast<std::uint32_t>(i));
            break;
          }
        }
      },
      [&session](const std::uint32_t birthOp) -> bool {
        for (const auto &v : session->views()) {
          if (v.focusedBirth == birthOp) {
            return true;
          }
        }
        return false;
      });

  docSwitcher->setCloseHandler([&views](const std::uint32_t docIndex) {
    views.closeDocument(docIndex);
  });
  docSwitcher->setNewDocHandler([&views]() { views.newDocument(); });
  docSwitcher->setManagerHandler(
      [&storeObjectManager]() { storeObjectManager.toggle(); });
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

  state->wheelHandler = [&views](float /*wx*/, float wy,
                                 std::uint16_t /*mods*/) -> bool {
    if (!views.onionSkinMode()) {
      return false;
    }
    if (wy > 0.1F) {
      views.cycleOnionSkin(-1);
      return true;
    }
    if (wy < -0.1F) {
      views.cycleOnionSkin(1);
      return true;
    }
    return false;
  };

  pouchDrawer.setSwingBackHandler(
      [&views](const xanadu::PouchItem &item) { views.swingBackToSpan(item); });

  xanadu::KineticTetherEngine kineticTetherEngine;
  xanadu::KineticTetherOverlay kineticTetherOverlay(kineticTetherEngine);
  xanadu::WireframeHullOverlay wireframeHullOverlay(renderer);
  views.setWireframeOverlay(&wireframeHullOverlay);
  kineticTetherEngine.setVoidSpawnHandler(
      [&views](const xanadu::TetherPayload &payload, const float sx,
               const float sy) {
        views.spawnTranscludedDocument(payload, sx, sy);
      });

  auto radialMenu = std::make_shared<gleditor::RadialMenu>();
  radialMenu->setActionHandler(
      [&session, &views, &quotationOverlay](
          const std::string &id, [[maybe_unused]] const std::string &action,
          const std::uint32_t docIndex, const std::uint32_t charOffset,
          const std::uint32_t charLength) {
        if (id == "format:bold") {
          session->markDecorated(
              docIndex, charOffset, charLength,
              gleditor::decorationBit(gleditor::Decoration::Bold));
        } else if (id == "format:italic") {
          session->markDecorated(
              docIndex, charOffset, charLength,
              gleditor::decorationBit(gleditor::Decoration::Italic));
        } else if (id == "format:underline") {
          session->markDecorated(
              docIndex, charOffset, charLength,
              gleditor::decorationBit(gleditor::Decoration::Underline));
        } else if (id == "format:superscript") {
          session->markDecorated(
              docIndex, charOffset, charLength,
              gleditor::decorationBit(gleditor::Decoration::Superscript));
        } else if (id == "format:subscript") {
          session->markDecorated(
              docIndex, charOffset, charLength,
              gleditor::decorationBit(gleditor::Decoration::Subscript));
        } else if (id == "align:left") {
          session->setAlignment(docIndex, charOffset, charLength,
                                gleditor::TextAlign::Left);
        } else if (id == "align:centre" || id == "align:center") {
          session->setAlignment(docIndex, charOffset, charLength,
                                gleditor::TextAlign::Centre);
        } else if (id == "align:right") {
          session->setAlignment(docIndex, charOffset, charLength,
                                gleditor::TextAlign::Right);
        } else if (id == "align:justify") {
          session->setAlignment(docIndex, charOffset, charLength,
                                gleditor::TextAlign::Justify);
        } else if (id == "op:pagebreak") {
          session->insertBreak(docIndex, charOffset);
        } else if (id == "op:transclude") {
          views.transcludeSelection();
        } else if (id == "op:quote") {
          quotationOverlay.toggle();
        } else if (id == "info:author") {
          std::string authorStr = "Local Sovereign Author";
          if (const auto *ps = session->userPermascroll()) {
            authorStr = std::format("Author OpenPGP: {}",
                                    ps->config().masterIdentity.view());
          }
          std::cout << "xuzz: " << authorStr << "\n";
        }
      });

  xanadu::LinkBeams links(*session, renderer);
  links.setVisible(!opts.noBeams);
  links.setSworph(!opts.noSworph);
  views.setComparisonCameraReady([&links] {
    if (links.busy()) return false;
    links.releaseCamera();
    return true;
  });
  if (opts.physicsEnabled) {
    links.setPhysicsEnabled(true);
  }
  xanadu::TenuousTetherOverlay tenuousTetherOverlay(renderer, nullptr);
  links.setTetherOverlay(&tenuousTetherOverlay);
  xanadu::SatelloidOverlay satelloidOverlay(renderer);
  links.setSatelloidOverlay(&satelloidOverlay);

  const auto readablePx = [wholePages =
                               opts.wholePages](const float configured) {
    return wholePages ? 0.0F : configured;
  };
  views.setReadableTextPx(readablePx(xanadu::LayoutConfig{}.readableTextPx));
  links.setReadableTextPx(readablePx(xanadu::LayoutConfig{}.readableTextPx));

  xanadu::LinkContext linkContext(*session);
  linkContext.setUnavailableHandler([state](std::string message) {
    state->showDialog(render::DiagnosticSeverity::Warning,
                      "Link endpoint unavailable", std::move(message));
  });
  views.selectIndependentLink = [&linkContext](const xanadu::LinkKey &key) {
    std::ignore = linkContext.execute(xanadu::nav::SelectLink{.key = key});
  };
  views.packageVisibilityChanged = [&linkContext] {
    linkContext.packageVisibilityChanged();
  };
  KeyboardPane keyboardPane(state);
  renderer->addPickObserver(&keyboardPane);

  linkContext.setFocusDocument(
      [&views, &keyboardPane](const std::size_t viewIndex,
                              const xanadu::Extent range) {
        keyboardPane.leaveZigzag(false);
        views.focusSpan(viewIndex, range.start, range.end);
      });
  linkContext.setCaretQuery(
      [&renderer]() -> std::optional<xanadu::LinkContext::CaretPosition> {
        const auto *const caret = renderer->editCaret();
        if (nullptr == caret || !caret->active()) {
          return std::nullopt;
        }
        return xanadu::LinkContext::CaretPosition{
            .view   = caret->documentIndex(),
            .offset = caret->byteOffset(),
            .selection =
                caret->hasSelection()
                    ? std::optional<xanadu::Extent>{{caret->selectionStart(),
                                                     caret->selectionEnd()}}
                    : std::nullopt};
      });
  links.setLinkContext(&linkContext);

  xanadu::LinkPanelOverlay linkPanel(linkContext, *session);
  const auto selectedPair = [&linkContext](const RenderState &rState,
                                           const auto &cellPoint)
      -> std::optional<xanadu::LinkPanelOverlay::AnchorPair> {
    const auto selected = linkContext.selection();
    if (!selected || !selected->occurrences) return std::nullopt;
    xanadu::LinkPanelOverlay::AnchorPair points;
    for (const auto side : {xanadu::LinkSide::Left, xanadu::LinkSide::Right}) {
      const auto &cursor = selected->cursor(side);
      if (!cursor.member || !cursor.occurrence) return std::nullopt;
      const auto &members = selected->occurrences->members(side);
      if (*cursor.member >= members.size() ||
          *cursor.occurrence >= members[*cursor.member].occurrences.size()) {
        return std::nullopt;
      }
      const auto &site =
          members[*cursor.member].occurrences[*cursor.occurrence].site;
      const auto point = std::visit(
          [&]<typename Site>(const Site &at) -> std::optional<glm::vec3> {
            if constexpr (std::is_same_v<Site, xanadu::CellSite>) {
              return cellPoint(at);
            } else {
              const auto view = linkContext.viewIndexOf(at);
              if (!view || *view >= rState.docs.size() || !rState.docs[*view])
                return std::nullopt;
              const auto &doc  = *rState.docs[*view];
              const auto start = doc.anchorFor(at.range.start);
              if (!start) return std::nullopt;
              const auto first = doc.worldPoint(*start);
              if (!first) return std::nullopt;
              if (at.range.end > at.range.start + 1) {
                if (const auto last = doc.anchorFor(at.range.end - 1)) {
                  if (const auto end = doc.worldPoint(*last)) {
                    return 0.5F * (*first + *end);
                  }
                }
              }
              return first;
            }
          },
          site);
      if (!point) return std::nullopt;
      points[side == xanadu::LinkSide::Left ? 0 : 1] = *point;
    }
    return points;
  };
  linkPanel.setAnchorResolver([selectedPair](const RenderState &rState) {
    return selectedPair(
        rState, [](const xanadu::CellSite &) -> std::optional<glm::vec3> {
          return std::nullopt;
        });
  });
  linkPanel.setFramingHandler(
      [&links, &state](const xanadu::LinkPanelOverlay::AnchorPair &points,
                       ch::Timeline &timeline) {
        const auto midpoint = 0.5F * (points[0] + points[1]);
        glm::vec3 target;
        {
          std::scoped_lock locker(state->view);
          const auto &view   = state->view;
          const float aspect = view.screenHeight > 0
                                   ? static_cast<float>(view.screenWidth) /
                                         static_cast<float>(view.screenHeight)
                                   : 4.0F / 3.0F;
          const float fit    = gleditor::spatial::framingDistance(
              std::max(std::abs(points[0].x - points[1].x) + 20.0F, 10.0F),
              std::max(std::abs(points[0].y - points[1].y) + 20.0F, 10.0F),
              view.fov, aspect, 1.5F);
          target =
              glm::vec3{midpoint.x, midpoint.y,
                        std::clamp(std::max(view.pos.z, fit), 50.0F, 9500.0F)};
        }
        links.sworphCameraTo(target, timeline);
      });

  xanadu::OverviewOverlay overview(state);
  const auto chosenPlaces = [&linkContext](const RenderState &rState,
                                           std::vector<glm::vec3> &out) {
    const auto selected = linkContext.selection();
    if (!selected || !selected->occurrences) {
      return;
    }
    for (const auto side : {xanadu::LinkSide::Left, xanadu::LinkSide::Right}) {
      const auto &cursor = selected->cursor(side);
      if (!cursor.member || !cursor.occurrence) {
        continue;
      }
      const auto &site = selected->occurrences->members(side)[*cursor.member]
                             .occurrences[*cursor.occurrence]
                             .site;
      const auto *const at = std::get_if<xanadu::DocumentSite>(&site);
      const auto view      = at ? linkContext.viewIndexOf(*at) : std::nullopt;
      if (!view || *view >= rState.docs.size() || !rState.docs[*view]) {
        continue;
      }
      const auto &doc = *rState.docs[*view];
      if (const auto anchor = doc.anchorFor(at->range.start)) {
        if (const auto point =
                doc.worldPoint(anchor->pageIndex, anchor->x, anchor->y)) {
          out.push_back(*point);
        }
      }
    }
  };
  overview.setMarkSource(chosenPlaces,
                         [&linkContext] { return linkContext.revision(); });
  satelloidOverlay.setLinkContext(&linkContext);

  // 5. Zigzag presentation & BridgeCoordinator
  // The application talks to the slice through the seam; only the Vortex host
  // and the visualizer's own commands still need the concrete type.
  const auto zigzagVisualizer = std::make_shared<zigzag::ZigzagVisualizer>("");
  const std::shared_ptr<xanadu::view::SlicePresentation> zigzagPresentation =
      zigzagVisualizer;
  auto &bridgeStore = session->store(0);
  zigzagPresentation->bindXuduStore(bridgeStore,
                                    bridgeStore.primaryCurrentVersion());
  const auto initialLayout = xanadu::LayoutConfig::fromStore(
      session->systemStore(xanadu::SystemDocKind::Layout));
  zigzagPresentation->setPresentationConfig(initialLayout.zigzag);
  if (auto vHost = zigzagVisualizer->vortexHost()) {
    vHost->loadConfigFromStore(
        session->systemStore(xanadu::SystemDocKind::Settings));
    vHost->loadMacrosFromStore(
        session->systemStore(xanadu::SystemDocKind::Keymap));
  }
  zigzagPresentation->setPresentationTransformResolver(
      [&views] { return views.presentationTransform(); });

  xanadu::BridgeCoordinator bridgeCoordinator(links, renderer,
                                              *state->accessibility);
  bridgeCoordinator.connectSatelloidNavigation(satelloidOverlay);
  bridgeCoordinator.setDocumentFocusHandler(
      [&views, &linkContext, &renderer,
       &session](const zigzag::CellRef cell,
                 const std::span<const xanadu::PrimediaSpan> content) {
        std::vector<xanadu::PrimediaSpan> spans(content.begin(), content.end());
        renderer->runWithState(
            [&views, &linkContext, &session, cell,
             spans = std::move(spans)](RenderState &) mutable {
              // Activating a cell that holds one of the selected link's
              // members is choosing that member, the same gesture as picking
              // it in text; any other cell still jumps to where its content
              // is open.
              const auto selected = linkContext.selection();
              if (selected && selected->occurrences) {
                std::uint32_t length = 0;
                for (const auto &span : spans) {
                  length += static_cast<std::uint32_t>(span.length);
                }
                const auto &primary = session->store();
                const xanadu::OccurrenceSite whole =
                    xanadu::CellSite{.store   = primary.documentId(),
                                     .version = primary.primaryCurrentVersion(),
                                     .cell    = cell,
                                     .range   = {.start = 0, .end = length}};
                const auto holds = [&](const xanadu::LinkMember &member) {
                  return std::ranges::any_of(
                      member.occurrences, [&](const xanadu::Occurrence &o) {
                        const auto *const at =
                            std::get_if<xanadu::CellSite>(&o.site);
                        return nullptr != at && at->cell == cell;
                      });
                };
                if (std::ranges::any_of(selected->occurrences->left, holds) ||
                    std::ranges::any_of(selected->occurrences->right, holds)) {
                  std::ignore = linkContext.execute(
                      xanadu::commandForPick(selected->key, whole));
                  return;
                }
              }
              views.focusContent(std::move(spans));
            });
      });

  const auto showZigzagFocus = [&zigzagPresentation, &renderer, &state] {
    renderer->runWithState([&zigzagPresentation, &state](RenderState &) {
      if (const auto centre = zigzagPresentation->focusCentre()) {
        std::scoped_lock locker(state->view);
        state->view.pos.x = centre->x;
        state->view.pos.y = centre->y;
      }
    });
  };

  bridgeCoordinator.attach(*zigzagPresentation);
  linkContext.setManifold(bridgeCoordinator.manifold(), 0,
                          session->store().primaryCurrentVersion());
  linkContext.setFocusCell(
      [&bridgeCoordinator, &keyboardPane,
       &showZigzagFocus](const zigzag::CellRef cell, xanadu::Extent) {
        bridgeCoordinator.onDocumentLinkActivated(cell);
        keyboardPane.enterZigzag();
        showZigzagFocus();
      });
  linkContext.setCellFocusQuery(
      [&zigzagPresentation] { return zigzagPresentation->focusCell(); });

  std::size_t zigzagStoreIndex = 0;
  std::unordered_map<std::size_t, xanadu::MicroversionId> sliceHeads;
  satelloidOverlay.setSiteFilter(
      [&session, &zigzagStoreIndex](const xanadu::CellSite &site) {
        return site.store == session->store(zigzagStoreIndex).documentId();
      });
  satelloidOverlay.setAnchorResolver(
      [zigzagPresentation](const zigzag::CellRef cell) {
        if (const auto anchor = zigzagPresentation->cellAnchor(cell)) {
          return std::optional{anchor->position};
        }
        return zigzagPresentation->focusCentre();
      });
  satelloidOverlay.setNeighborhoodResolver([zigzagPresentation](
                                               const zigzag::CellRef root) {
    std::vector<xanadu::SatelloidNeighbor> out;
    const auto &manifold = zigzagPresentation->manifold();
    const auto *store    = zigzagPresentation->store();
    if (!store || !manifold.contains(root)) return out;
    out.push_back({root, manifold.textOf(root, *store), {}, 0});
    const auto &view = zigzagPresentation->currentView();
    const std::array<std::string, 3> names{view.x_dimension, view.y_dimension,
                                           view.z_dimension};
    const std::array<glm::vec2, 3> axes{glm::vec2{1.0F, 0.0F},
                                        glm::vec2{-0.5F, 0.866F},
                                        glm::vec2{-0.5F, -0.866F}};
    const auto radius = std::max(1, zigzagPresentation->cellRadius());
    for (std::size_t axis = 0; axis < names.size(); ++axis) {
      const auto dim = manifold.dimensionNamed(names[axis], *store);
      if (!dim) continue;
      for (const auto direction :
           {zigzag::DimVector::NEG, zigzag::DimVector::POS}) {
        auto cell        = root;
        const float sign = direction == zigzag::DimVector::POS ? 1.0F : -1.0F;
        for (int depth = 1; depth <= radius; ++depth) {
          cell = manifold.linked(cell, *dim, direction);
          if (cell == zigzag::noCell || cell == root) break;
          out.push_back({cell, manifold.textOf(cell, *store),
                         axes[axis] * sign * static_cast<float>(depth),
                         static_cast<std::uint32_t>(depth)});
        }
      }
    }
    return out;
  });
  satelloidOverlay.setNeighborhoodRevision(
      [zigzagPresentation] { return zigzagPresentation->bridgeRevision(); });
  satelloidOverlay.setAxisNameResolver([zigzagPresentation] {
    return zigzagPresentation->currentView().x_dimension;
  });
  zigzagPresentation->setExternInspector(
      [&session, &zigzagStoreIndex](const zigzag::CellRef cell) {
        const auto &local = session->store(zigzagStoreIndex);
        const auto target = local.externTarget(cell);
        if (!target) return std::string{" [unreadable foreign target]"};
        const auto *record = local.scrollRegistry().recordForId(target->scroll);
        if (!record) return std::string{" [unregistered foreign scroll]"};
        for (std::size_t i = 0; i < session->storeCount(); ++i) {
          const auto &foreign = session->store(i);
          if (foreign.documentId().str() != record->globalKey) continue;
          const auto folded = foreign.rebuildManifold(foreign.latest());
          const auto found =
              xanadu::resolveLocalExternCell(local, cell, foreign, &folded);
          if (!found.isResolved()) {
            return std::string{" [foreign target absent]"};
          }
          return std::format(" [resolved in store {} cell #{}: {}]", i,
                             found.cell, folded.textOf(found.cell, foreign));
        }
        return std::string{" [foreign slice not loaded]"};
      });

  const auto bindZigzag = [&session, &zigzagPresentation, &linkContext,
                           &bridgeCoordinator, &views, &map, &zigzagStoreIndex,
                           &sliceHeads](RenderState &rState,
                                        const std::size_t storeIndex,
                                        const xanadu::MicroversionId &version) {
    if (!zigzagPresentation->sliceHead().isZero()) {
      sliceHeads[zigzagStoreIndex] = zigzagPresentation->sliceHead();
    }
    zigzagPresentation->bindXuduStore(session->store(storeIndex), version);
    zigzagStoreIndex       = storeIndex;
    sliceHeads[storeIndex] = version;
    map.setStoreIndex(storeIndex);
    map.setCurrent(version);
    linkContext.setManifold(bridgeCoordinator.manifold(), storeIndex, version);
    bridgeCoordinator.synchronize();
    const auto &open = session->views();
    for (std::size_t i = 0; i < open.size() && i < rState.docs.size(); ++i) {
      if (open[i].storeIndex == storeIndex) {
        views.anchorPresentation(rState.docs[i]);
        break;
      }
    }
  };

  docSwitcher->setSelectHandler([&views, &renderer, &session, &bindZigzag,
                                 &sliceHeads](const std::uint32_t docIndex) {
    views.selectDoc(docIndex);
    renderer->runWithState(
        [&session, &bindZigzag, &sliceHeads, docIndex](RenderState &rState) {
          if (docIndex >= session->views().size()) return;
          const auto storeIndex = session->views()[docIndex].storeIndex;
          const auto &store     = session->store(storeIndex);
          if (store.homeCell() == zigzag::noCell) return;
          const auto found = sliceHeads.find(storeIndex);
          const auto version =
              found != sliceHeads.end() ? found->second : store.latest();
          bindZigzag(rState, storeIndex, version);
        });
  });

  overview.setMarkSource(
      [chosenPlaces, &zigzagPresentation](const RenderState &rState,
                                          std::vector<glm::vec3> &out) {
        chosenPlaces(rState, out);
        if (const auto centre = zigzagPresentation->focusCentre()) {
          out.push_back(*centre);
        }
      },
      [&linkContext, &zigzagPresentation] {
        return linkContext.revision() + zigzagPresentation->bridgeRevision();
      });

  linkPanel.setCellHighlighter(
      [&zigzagPresentation](std::vector<xanadu::CellHighlight> highlights,
                            const std::uint32_t border) {
        zigzagPresentation->setCellHighlights(std::move(highlights), border);
      });
  linkPanel.setAnchorResolver([selectedPair, &linkContext, &zigzagPresentation,
                               &session,
                               &zigzagStoreIndex](const RenderState &rState) {
    std::optional<zigzag::CellRef> preview;
    const auto selected = linkContext.selection();
    if (selected && selected->occurrences) {
      for (const auto side :
           {selected->active, xanadu::opposite(selected->active)}) {
        const auto &cursor = selected->cursor(side);
        if (!cursor.member || !cursor.occurrence) continue;
        const auto &members = selected->occurrences->members(side);
        if (*cursor.member >= members.size() ||
            *cursor.occurrence >= members[*cursor.member].occurrences.size())
          continue;
        const auto &site =
            members[*cursor.member].occurrences[*cursor.occurrence].site;
        const auto *cell = std::get_if<xanadu::CellSite>(&site);
        if (cell &&
            cell->store == session->store(zigzagStoreIndex).documentId()) {
          preview = cell->cell;
          break;
        }
      }
    }
    zigzagPresentation->setPreviewCell(preview);
    return selectedPair(
        rState, [&](const xanadu::CellSite &at) -> std::optional<glm::vec3> {
          if (at.store != session->store(zigzagStoreIndex).documentId()) {
            return std::nullopt;
          }
          const auto anchor = zigzagPresentation->cellAnchor(at.cell);
          return anchor ? std::optional{anchor->position} : std::nullopt;
        });
  });

  bridgeCoordinator.applyConfig(initialLayout.bridge);

  // 6. ViewCoordinator (Unified, XanadocOnly, ZigzagOnly)
  ViewCoordinator viewCoordinator(views, zigzagPresentation, bridgeCoordinator,
                                  renderer, state);
  viewCoordinator.setViewMode(opts.viewMode);

  links.setOpener([&views](const xanadu::MicroversionId &version) {
    views.showAlongside(version);
  });
  links.setMediaRectResolver(
      [&images, &views,
       &session](const Doc &doc, const std::size_t storeIndex,
                 const xanadu::MicroversionId &version,
                 const std::uint32_t docOffset) -> std::optional<Doc::Anchor> {
        const auto spans = session->mediaSpansFor(version, storeIndex);
        for (const auto &mSpan : spans) {
          if (docOffset < mSpan.docOffset || docOffset >= mSpan.docOffset + 3) {
            continue;
          }
          if (mSpan.isImage) {
            return images.rectFor(doc, mSpan.docOffset);
          }
          return views.widgetRectFor(doc, mSpan.docOffset);
        }
        return std::nullopt;
      });

  renderer->addSpanDecorator(session.get());
  renderer->addFrameContributor(docSwitcher.get());
  renderer->addFrameContributor(&map);
  renderer->addFrameContributor(&links);
  renderer->addFrameContributor(&tenuousTetherOverlay);
  renderer->addFrameContributor(&satelloidOverlay);
  renderer->addPickObserver(&satelloidOverlay);
  renderer->addFrameContributor(&kineticTetherOverlay);
  renderer->addFrameContributor(&wireframeHullOverlay);
  renderer->addFrameContributor(&images);
  renderer->addFrameContributor(&views);
  renderer->addFrameContributor(&linkPanel);
  renderer->addSpanDecorator(&linkPanel);
  renderer->addPickObserver(&linkPanel);
  renderer->addFrameContributor(&overview);
  renderer->addPickObserver(&overview);
  renderer->addFrameContributor(radialMenu.get());
  renderer->addFrameContributor(&publishForm);
  renderer->addFrameContributor(&pouchDrawer);
  renderer->addFrameContributor(&swarmTelescope);
  renderer->addFrameContributor(&quotationOverlay);
  renderer->addFrameContributor(&storeObjectManager);

  state->accessibility->addSource(docSwitcher.get());
  state->accessibility->addSource(&links);
  state->accessibility->addSource(&satelloidOverlay);
  state->accessibility->addSource(&kineticTetherOverlay);
  state->accessibility->addSource(&wireframeHullOverlay);
  state->accessibility->addSource(&linkPanel);
  state->accessibility->addSource(&overview);
  state->accessibility->addSource(&map);
  state->accessibility->addSource(&publishForm);
  state->accessibility->addSource(&quotationOverlay);
  state->accessibility->addSource(radialMenu.get());
  state->accessibility->addSource(&pouchDrawer);
  state->accessibility->addSource(&storeObjectManager);
  state->accessibility->addSource(&swarmTelescope);
  state->accessibility->setToolkit("xuzz", TOSTRING(GLEDITOR_VERSION));

  const auto showMapVersion = [&views, &map, &renderer, &session,
                               &bindZigzag](const xanadu::MicroversionId &id) {
    const auto storeIndex = map.storeIndex();
    const auto *caret     = renderer->editCaret();
    const auto previous   = caret && caret->active() ? caret->byteOffset() : 0U;
    const auto length     = session->store(storeIndex).textOf(id).size();
    views.showOnly(id, storeIndex);
    renderer->runWithState([&renderer, at = std::min<std::size_t>(
                                           previous, length)](RenderState &) {
      if (auto *next = renderer->editCaret()) {
        next->placeAt(0, static_cast<std::uint32_t>(at));
      }
    });
    renderer->runWithState([&bindZigzag, storeIndex, id](RenderState &rState) {
      bindZigzag(rState, storeIndex, id);
    });
  };
  map.setGoer(showMapVersion);
  map.setScrubHandler(showMapVersion);
  map.setCompareHandler([&views, &session, &renderer, &map, &links, &bindZigzag,
                         zigzagPresentation, &state, &keyboardPane](
                            const std::vector<xanadu::MicroversionId> &vers) {
    if (vers.empty()) return;
    GLEDITOR_LOG_DEBUG("xuzz.diff", "opening {} compared versions",
                       vers.size());
    const auto storeIndex = map.storeIndex();
    const auto diff       = session->store(storeIndex).diffVersions(vers);
    const auto count      = session->views().size();
    for (std::size_t i = 0; i < count; i++) {
      renderer->push(RenderItemCloseDoc());
    }
    renderer->runWithState(
        [&session](RenderState &) { session->clearViews(); });
    for (const auto &v : vers) {
      views.showAlongside(v, 0.0F, storeIndex);
    }
    const auto viewIndex = vers.size() - 1;
    const auto &baseline = diff.versions.front().text;
    const auto &chosen   = diff.versions.back().text;
    const auto mismatch  = std::ranges::mismatch(baseline, chosen);
    auto changeAt =
        static_cast<std::uint32_t>(std::distance(chosen.begin(), mismatch.in2));
    const auto &spans = diff.versions.back().spans;
    const auto unique = std::ranges::find_if(spans, [](const auto &span) {
      return span.kind == xanadu::DiffKind::Unique;
    });
    if (unique != spans.end()) changeAt = unique->offset;

    const auto baselineSlice =
        session->store(storeIndex).rebuildManifold(vers.front());
    const auto &changedCells = diff.versions.back().changedCells;
    const auto freshCell     = std::ranges::find_if(
        changedCells, [&baselineSlice](const zigzag::CellRef cell) {
          return !baselineSlice.contains(cell);
        });
    const auto changedCell = changedCells.empty() ? zigzag::noCell
                             : freshCell != changedCells.end()
                                 ? *freshCell
                                 : changedCells.front();
    const bool focusSlice  = zigzagPresentation->presentationVisible() &&
                            changedCell != zigzag::noCell;

    renderer->runWithState([&views, &renderer, viewIndex, changeAt, &bindZigzag,
                            zigzagPresentation, &state, &keyboardPane, &links,
                            focusSlice, changedCell, storeIndex,
                            target = vers.back()](RenderState &rState) {
      if (viewIndex < rState.docs.size() && rState.docs[viewIndex]) {
        if (auto *caret = renderer->editCaret()) {
          caret->placeAt(static_cast<std::uint32_t>(viewIndex), changeAt);
        }
        if (!focusSlice) {
          const std::weak_ptr<Doc> document = rState.docs[viewIndex];
          views.placeCameraWhenReady([&views, document, changeAt] {
            const auto ready = document.lock();
            if (!ready) return true;
            if (!ready->anchorFor(changeAt)) return false;
            views.keepInView(*ready, changeAt);
            return true;
          });
        }
      }
      if (focusSlice) {
        keyboardPane.leaveZigzag(false);
        bindZigzag(rState, storeIndex, target);
        zigzagPresentation->focusCell(changedCell);
        views.cancelReadingFrame();
        views.placeCameraWhenReady([&views, &links, zigzagPresentation, &state,
                                    changedCell, placedFrames = 0]() mutable {
          if (!views.presentationTransform() || ++placedFrames < 2) {
            return false;
          }
          const auto anchor = zigzagPresentation->cellAnchor(changedCell);
          if (!anchor) return false;
          std::scoped_lock locker(state->view);
          auto &camera = state->view;
          links.releaseCamera();
          camera.pos.x = anchor->position.x;
          camera.pos.y = anchor->position.y;
          if (const auto distance = xanadu::readableCameraDistance(
                  anchor->lineHeight, static_cast<float>(camera.screenHeight),
                  camera.fov,
                  zigzagPresentation->presentationConfig().minReadableTextPx)) {
            camera.pos.z = anchor->position.z + *distance;
          }
          views.cancelReadingFrame();
          constexpr int kScaleSettlingFrames = 5;
          return placedFrames >= kScaleSettlingFrames;
        });
      }
    });
  });
  map.setOnionSkinHandler([&views, &session, &renderer, &map](
                              const std::vector<xanadu::MicroversionId> &vers) {
    const auto count = session->views().size();
    for (std::size_t i = 0; i < count; i++) {
      renderer->push(RenderItemCloseDoc());
    }
    renderer->runWithState(
        [&session](RenderState &) { session->clearViews(); });
    for (const auto &v : vers) {
      views.showAlongside(v, 0.0F, map.storeIndex());
    }
    views.setOnionSkin(true);
  });
  map.setQuoteHandler([&session, &views,
                       &map](const xanadu::MicroversionId &srcVer,
                             const std::uint32_t srcAt,
                             const std::uint32_t srcLen) {
    const auto chosen =
        std::ranges::find_if(session->views(), [&map](const auto &view) {
          return view.storeIndex == map.storeIndex();
        });
    if (chosen == session->views().end()) return;
    auto &st           = session->store(map.storeIndex());
    const auto headVer = chosen->version;
    const auto headLen = static_cast<std::uint32_t>(st.textOf(headVer).size());
    st.transclude(headVer, headLen, srcVer, srcAt, srcLen);
    views.showOnly(headVer, map.storeIndex());
  });
  map.setAnnotateHandler([&map, &publishForm, &session, &renderer, &bindZigzag,
                          &zigzagStoreIndex, zigzagPresentation](
                             const xanadu::MicroversionId &target) {
    if (map.storeIndex() != zigzagStoreIndex || target.isZero()) return;
    auto &store = session->store(zigzagStoreIndex);
    if (!store.getOp(target)) return;
    const auto focus = zigzagPresentation->focusCell();
    const auto index = zigzagStoreIndex;
    gleditor::Form::Field note;
    note.label    = "Annotation";
    note.hint     = "Note attached to the selected OSMIC operation";
    note.required = true;
    publishForm.open(
        "Annotate operation " + target.str(),
        "Place its handle after the focused cell on d.1", {note},
        [&session, &renderer, &bindZigzag, &map, zigzagPresentation, target,
         focus, index](const std::vector<gleditor::Form::Field> &answers) {
          if (answers.empty() || answers.front().answer().empty()) return;
          renderer->runWithState(
              [&session, &bindZigzag, &map, zigzagPresentation, target, focus,
               index, note = answers.front().answer()](RenderState &rState) {
                auto &store = session->store(index);
                auto head =
                    store.annotateVersion(zigzagPresentation->sliceHead(),
                                          target, {.description = note});
                auto folded         = store.rebuildManifold(head);
                const auto targetOp = store.segmentedOps().indexOf(target);
                const auto handle   = folded.findOpHandle(targetOp);
                const auto dim      = folded.dimensionNamed("d.1", store);
                if (!handle || !dim) return;
                const auto anchor =
                    folded.contains(focus) ? focus : folded.home();
                const auto next =
                    folded.linked(anchor, *dim, zigzag::DimVector::POS);
                head = store.setLink(head, anchor, *dim, zigzag::DimVector::POS,
                                     *handle);
                if (next != zigzag::noCell && next != *handle) {
                  folded = store.rebuildManifold(head);
                  head   = store.setLink(head, *handle, *dim,
                                         zigzag::DimVector::POS, next);
                }
                bindZigzag(rState, index, head);
                zigzagPresentation->focusCell(*handle);
                map.invalidate();
                session->save(index);
              });
        });
  });

  // Handles are destroyed before the scopes they register.
  std::vector<gleditor::ui::FocusManager::ScopeHandle> modalScopes;
  for (gleditor::ui::FocusScope *scope :
       std::initializer_list<gleditor::ui::FocusScope *>{
           zigzagPresentation->focusScope(), &swarmTelescope, &publishForm,
           &quotationOverlay, &storeObjectManager, &pouchDrawer, &map,
           radialMenu.get()}) {
    modalScopes.push_back(state->focusManager.registerScope(
        *scope, {.allowedCommands = {"quit", "std:xudu/quit"}}));
  }

  renderer->addPickObserver(docSwitcher.get());
  renderer->addPickObserver(&links);
  renderer->addPickObserver(radialMenu.get());
  renderer->addPickObserver(&map);
  renderer->addPickObserver(&pouchDrawer);
  renderer->addPickObserver(&swarmTelescope);
  renderer->addPickObserver(&quotationOverlay);
  renderer->addPickObserver(&storeObjectManager);

  state->pressOnSelection = [&kineticTetherEngine, &session, renderer,
                             state](const std::uint32_t docIdx,
                                    const std::uint32_t /*offset*/,
                                    const int mx, const int my) {
    const auto *const caret = renderer->editCaret();
    if (nullptr == caret || docIdx >= session->views().size()) {
      return false;
    }
    const auto selStart  = caret->selectionStart();
    const auto selEnd    = caret->selectionEnd();
    const auto &openView = session->views()[docIdx];
    const auto &st       = session->store(openView.storeIndex);
    const auto spans =
        st.rebuild(openView.version).spansFor(selStart, selEnd - selStart);
    if (spans.empty()) {
      return false;
    }
    const auto text    = st.textOf(openView.version);
    const auto screenX = static_cast<float>(mx);
    const auto screenY = static_cast<float>(state->view.screenHeight - my);
    kineticTetherEngine.startDrag(
        xanadu::TetherPayload{
            .span             = spans.front(),
            .previewText      = selStart < text.size()
                                    ? text.substr(selStart, selEnd - selStart)
                                    : std::string{},
            .originVersion    = openView.version,
            .originDocIndex   = docIdx,
            .originCharStart  = selStart,
            .originCharEnd    = selEnd,
            .originScreenPos  = glm::vec2(screenX, screenY),
            .originKind       = xanadu::PouchOriginKind::Document,
            .originCell       = 0,
            .originSliceIndex = 0,
            .originRankCoord  = {},
            .originOpRef      = std::nullopt,
            .originDocState   = std::nullopt,
        },
        screenX, screenY);
    return true;
  };

  state->mouseDownHandler =
      [&kineticTetherEngine, &session, renderer, state, zigzagPresentation](
          const int mx, const int my, const std::uint8_t button) -> bool {
    if (button != 1 || 0 == (SDL_GetModState() & SDL_KMOD_ALT)) {
      return false;
    }
    renderer->runWithState([&kineticTetherEngine, &session, renderer, mx, my,
                            state, zigzagPresentation](RenderState &) {
      const auto screenX = static_cast<float>(mx);
      const auto screenY = static_cast<float>(state->view.screenHeight - my);
      if (!renderer->lastPick || !renderer->lastPick->semanticTarget ||
          !renderer->lastPick->semanticTarget->cellRef) {
        return;
      }
      const auto cellRef = static_cast<zigzag::CellRef>(
          *renderer->lastPick->semanticTarget->cellRef);
      if (zigzag::isEphemeral(cellRef)) {
        return;
      }
      const auto &manifold = zigzagPresentation->manifold();
      const auto spans     = manifold.contentOf(cellRef);
      if (spans.empty()) {
        return;
      }
      const auto &bridgeStore = session->store(0);
      kineticTetherEngine.startDrag(
          xanadu::TetherPayload{
              .span            = spans.front(),
              .previewText     = manifold.textOf(cellRef, bridgeStore),
              .originVersion   = bridgeStore.primaryCurrentVersion(),
              .originDocIndex  = 0,
              .originCharStart = 0,
              .originCharEnd = static_cast<std::uint32_t>(spans.front().length),
              .originScreenPos  = glm::vec2(screenX, screenY),
              .originKind       = xanadu::PouchOriginKind::ZigzagCell,
              .originCell       = cellRef,
              .originSliceIndex = 0,
              .originRankCoord  = "d.1: #" + std::to_string(cellRef),
              .originOpRef      = std::nullopt,
              .originDocState   = std::nullopt,
          },
          screenX, screenY);
    });
    return true;
  };

  state->mouseMotionHandler = [&pouchDrawer, &kineticTetherEngine,
                               state](const int mx, const int my,
                                      const std::uint32_t /*buttons*/) -> bool {
    const auto screenX = static_cast<float>(mx);
    const auto screenY = static_cast<float>(state->view.screenHeight - my);

    if (kineticTetherEngine.isDragging()) {
      kineticTetherEngine.updateDrag(screenX, screenY);
      if (pouchDrawer.isOpen()) {
        const auto &payload = kineticTetherEngine.payload();
        pouchDrawer.forge().setDragGuide(payload.originScreenPos.x,
                                         payload.originScreenPos.y, screenX,
                                         screenY, true);
      }
      return true;
    }
    pouchDrawer.forge().setDragGuide(0.0F, 0.0F, 0.0F, 0.0F, false);
    return false;
  };

  state->mouseUpHandler = [&pouchDrawer, &kineticTetherEngine, &session, &views,
                           renderer, state](const int mx, const int my,
                                            const std::uint8_t button) -> bool {
    if (button != 1) { // 1 = SDL_BUTTON_LEFT
      return false;
    }
    const auto screenX = static_cast<float>(mx);
    const auto screenY = static_cast<float>(state->view.screenHeight - my);
    pouchDrawer.forge().setDragGuide(0.0F, 0.0F, 0.0F, 0.0F, false);

    // 1. If kinetic tether is currently dragging:
    if (kineticTetherEngine.isDragging()) {
      if (pouchDrawer.isOpen() && pouchDrawer.currentWidth() >= 50.0F) {
        const bool hitZone = pouchDrawer.zoneAt(screenX, screenY).has_value();
        const bool hitLeft = pouchDrawer.forge().containsLeft(screenX, screenY);
        const bool hitRight =
            pouchDrawer.forge().containsRight(screenX, screenY);
        if (hitZone || hitLeft || hitRight) {
          const auto &payload = kineticTetherEngine.payload();
          if (payload.originKind == xanadu::PouchOriginKind::ZigzagCell) {
            pouchDrawer.handleCellDrop(
                payload.span, payload.previewText, payload.originCell,
                payload.originRankCoord, screenX, screenY,
                payload.originSliceIndex, payload.originOpRef);
          } else {
            pouchDrawer.handleGhostDrop(
                payload.span, payload.previewText, payload.originVersion,
                screenX, screenY, payload.originDocIndex,
                payload.originCharStart, payload.originCharEnd);
          }
          kineticTetherEngine.cancelDrag();
          return true;
        }
      }
      renderer->pickThen(
          mx, my,
          [&kineticTetherEngine, &views, screenX,
           screenY](RenderState &rState, const render::PickingResult &pick) {
            const auto payload = kineticTetherEngine.payload();
            if ((render::tagKindGlyph == pick.tag.kind ||
                 render::tagKindPage == pick.tag.kind) &&
                pick.tag.docIndex < rState.docs.size()) {
              const auto doc = pick.tag.docIndex;
              if (const auto at = rState.docs[doc]->offsetForPick(pick.tag)) {
                const bool ontoItself =
                    xanadu::PouchOriginKind::Document == payload.originKind &&
                    doc == payload.originDocIndex &&
                    *at >= payload.originCharStart &&
                    *at <= payload.originCharEnd;
                kineticTetherEngine.cancelDrag();
                if (!ontoItself) {
                  views.insertSpanAt(rState, doc, *at, payload.span);
                }
                return;
              }
            }
            kineticTetherEngine.endDrag(screenX, screenY);
          });
      return true;
    }

    // 2. Direct drop into open pouch drawer from selection:
    if (!pouchDrawer.isOpen() || pouchDrawer.currentWidth() < 50.0F) {
      return false;
    }
    const bool hitZone  = pouchDrawer.zoneAt(screenX, screenY).has_value();
    const bool hitLeft  = pouchDrawer.forge().containsLeft(screenX, screenY);
    const bool hitRight = pouchDrawer.forge().containsRight(screenX, screenY);

    if (!hitZone && !hitLeft && !hitRight) {
      return false;
    }

    renderer->runWithState([&pouchDrawer, &session, screenX,
                            screenY](RenderState &rState) {
      auto *const caret = rState.caret;
      if (!caret || !caret->hasSelection()) {
        return;
      }
      const auto selStart = caret->selectionStart();
      const auto selEnd   = caret->selectionEnd();
      const auto docIdx   = caret->documentIndex();
      if (docIdx >= session->views().size() || selEnd <= selStart) {
        return;
      }
      const auto &openView = session->views()[docIdx];
      const auto &st       = session->store(openView.storeIndex);
      const auto ver       = st.rebuild(openView.version);
      const auto spans     = ver.spansFor(selStart, selEnd - selStart);
      if (spans.empty()) {
        return;
      }
      const auto text = st.textOf(openView.version);
      std::string preview;
      if (selStart < text.size()) {
        preview = text.substr(selStart, selEnd - selStart);
      }
      pouchDrawer.handleGhostDrop(spans.front(), preview, openView.version,
                                  screenX, screenY, docIdx, selStart, selEnd);
    });
    return true;
  };

  std::optional<xanadu::ReadingPlace> resuming;
  if (!opts.hasExplicitStore && opts.askedVersion.empty() &&
      opts.read.empty() && opts.alongside.empty() && extraImports.empty() &&
      opts.background.empty()) {
    resuming = session->lastPlace();
  }
  std::vector<std::optional<std::uint32_t>> resumedAt;
  std::vector<std::uint32_t> resumedLengths;
  if (resuming) {
    namespace fs       = std::filesystem;
    std::uint32_t next = 0;
    for (const auto &document : resuming->documents) {
      std::optional<std::size_t> storeIndex;
      try {
        if (document.storePath == session->path(0)) {
          storeIndex = 0;
        } else if (fs::exists(fs::path(document.storePath) / "ops.nodes")) {
          storeIndex = session->loadAuxiliaryStore(document.storePath);
        }
      } catch (const std::exception &err) {
        std::cerr << "xuzz: cannot reopen " << document.storePath << ": "
                  << err.what() << "\n";
      }
      if (!storeIndex) {
        resumedAt.emplace_back();
        resumedLengths.push_back(0);
        continue;
      }
      const auto &store = session->store(*storeIndex);
      auto version      = xanadu::MicroversionId::parse(document.version);
      if (!version.isZero() && !store.getOp(version).has_value()) {
        version = store.primaryCurrentVersion();
      }
      views.showAlongside(version, 0.0F, *storeIndex);
      resumedAt.emplace_back(next++);
      resumedLengths.push_back(
          static_cast<std::uint32_t>(store.textOf(version).size()));
    }
    if (0 == next) {
      resuming.reset();
    }
  }
  if (resuming) {
    views.restorePlace(*resuming, resumedAt, resumedLengths);
    if (!resuming->zigzagStore.empty()) {
      renderer->runWithState([&session, &bindZigzag, &zigzagPresentation,
                              &keyboardPane,
                              place = *resuming](RenderState &rState) {
        for (std::size_t i = 0; i < session->storeCount(); ++i) {
          if (session->path(i) != place.zigzagStore) {
            continue;
          }
          const auto version =
              xanadu::MicroversionId::parse(place.zigzagVersion);
          bindZigzag(rState, i,
                     version.isZero()
                         ? session->store(i).primaryCurrentVersion()
                         : version);
          if (place.zigzagFocus != zigzag::noCell) {
            zigzagPresentation->focusCell(place.zigzagFocus);
          }
          if (place.zigzagHasKeyboard) {
            keyboardPane.enterZigzag();
          }
          break;
        }
      });
    }
    renderer->runWithState([&linkContext](RenderState &) {
      linkContext.restoreCurrentSelection();
    });
  } else if (opts.askedVersion.empty() && opts.read.empty() &&
             opts.alongside.empty() && extraImports.empty()) {
    const auto &primaryStore = session->store(0);
    views.showAlongside(primaryStore.primaryCurrentVersion(), 0.0F, 0);
    // A store that is all slice -- a query's result, say -- has an empty
    // page to show and its cells pinned at the page's edge, the rows off
    // the window. Open on what it holds: the slice, as Alt+Home would.
    if (primaryStore.homeCell() != zigzag::noCell &&
        primaryStore.rebuild(primaryStore.primaryCurrentVersion()).length() ==
            0) {
      // Queued, as a resumed session's is: the pane's change handler, which
      // tells the presentation it has the keyboard, is set below.
      renderer->runWithState(
          [&views, &state, &keyboardPane, zigzagPresentation](RenderState &) {
            keyboardPane.enterZigzag();
            views.placeCameraWhenReady([&views, &state, zigzagPresentation,
                                        placedFrames = 0]() mutable {
              if (!views.presentationTransform() || ++placedFrames < 2) {
                return false;
              }
              if (const auto centre = zigzagPresentation->focusCentre()) {
                std::scoped_lock locker(state->view);
                state->view.pos.x = centre->x;
                state->view.pos.y = centre->y;
              }
              return true;
            });
          });
    }
  } else {
    views.showAlongside(opening, 0.0F, openingStore);
  }

  for (const auto &[extraVer, sIdx] : extraImports) {
    views.showAlongside(extraVer, 0.0F, sIdx);
  }
  if (!opts.alongside.empty()) {
    views.showAlongside(xanadu::MicroversionId::parse(opts.alongside), 0.0F, 0);
  }
  for (const auto &[storeIndex, also] : readPublications) {
    if (also != opening || storeIndex != openingStore) {
      views.showAlongside(also, 0.0F, storeIndex);
    }
  }
  for (const auto &behind : opts.background) {
    views.showAlongside(behind, kBackgroundDepthZ, 0);
  }
  if (opts.onionSkin) {
    views.setOnionSkin(true);
  }
  if (!resuming) {
    views.selectDoc(0);
  }

  // Audio & Video Widgets
  std::vector<std::shared_ptr<gleditor::MediaWidget>> audioWidgets;
  const auto loadOrWarn = [](auto &widget,
                             const gleditor::MediaResourcePtr &resource,
                             const std::string_view mrl) {
    if (const auto loaded = widget.load(resource); !loaded) {
      GLEDITOR_LOG_WARN("xuzz.media", "cannot load {}: {}", mrl,
                        gleditor::toString(loaded.error()));
    }
  };
  for (const auto &mrl : opts.audioMrls) {
    auto w = std::make_shared<gleditor::MediaWidget>();
    if (mrl == "white-noise" || mrl == "test") {
      std::vector<std::byte> dummy(1024, std::byte{0x55});
      auto stream =
          std::make_shared<gleditor::MemoryMediaStream>(std::move(dummy));
      loadOrWarn(
          *w,
          gleditor::MediaResource::fromStream(stream, "White Noise (48 kHz)"),
          mrl);
    } else {
      loadOrWarn(*w, gleditor::MediaResource::fromFile(mrl), mrl);
    }
    w->setTitle(fs::path(mrl).filename().string());
    w->setVisible(true);
    renderer->addFrameContributor(w.get());
    renderer->addPickObserver(w.get());
    state->accessibility->addSource(w.get());
    audioWidgets.push_back(w);
  }

  std::vector<std::shared_ptr<gleditor::MediaWidget>> videoWidgets;
  for (const auto &mrl : opts.videoMrls) {
    auto w = std::make_shared<gleditor::MediaWidget>();
    if (mrl == "test" || mrl == "pattern") {
      std::vector<std::byte> dummy(2048, std::byte{0xAA});
      auto stream =
          std::make_shared<gleditor::MemoryMediaStream>(std::move(dummy));
      loadOrWarn(
          *w,
          gleditor::MediaResource::fromStream(stream, "Sample Video (1080p)"),
          mrl);
    } else {
      loadOrWarn(*w, gleditor::MediaResource::fromFile(mrl), mrl);
    }
    w->setTitle(fs::path(mrl).filename().string());
    w->setVisible(true);
    renderer->addFrameContributor(w.get());
    renderer->addPickObserver(w.get());
    state->accessibility->addSource(w.get());
    videoWidgets.push_back(w);
  }

  renderer->runWithState([&audioWidgets, &videoWidgets](RenderState &rState) {
    if (!rState.docs.empty()) {
      for (std::size_t i = 0; i < audioWidgets.size(); ++i) {
        const auto dIdx = std::min(i, rState.docs.size() - 1);
        if (rState.docs[dIdx]) {
          audioWidgets[i]->attachToPage(rState.docs[dIdx], 0, 30.0F,
                                        110.0F +
                                            static_cast<float>(i) * 140.0F);
          audioWidgets[i]->setSize(340.0F, 120.0F);
        }
      }
      for (std::size_t i = 0; i < videoWidgets.size(); ++i) {
        videoWidgets[i]->setScreenPosition(
            30.0F, 80.0F + static_cast<float>(i) * 200.0F);
        videoWidgets[i]->setSize(340.0F, 180.0F);
      }
    }
  });

  // 7. Application & Command Registration
  gleditor::Application app(state, renderer, opts.backend, "Xuzz");

  // A. Xanadoc Commands
  app.commands().registerAction(std::string(xanadu::settings::kKeymapQuit),
                                "save and close", [state, &session] {
                                  session->saveAll();
                                  state->alive = false;
                                });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapSave),
                                "save or preserve active document",
                                [&views] { views.saveCurrent(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapClose),
                                "close the active document",
                                [&views] { views.closeActive(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapNextDoc),
                                "switch to next document",
                                [&views] { views.nextDoc(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapPrevDoc),
                                "switch to previous document",
                                [&views] { views.prevDoc(); });

  for (int i = 1; i <= 9; ++i) {
    const auto targetIndex = static_cast<std::uint32_t>(i - 1);
    app.commands().registerAction(
        "doc-" + std::to_string(i), "switch to document " + std::to_string(i),
        [&views, targetIndex] { views.selectDoc(targetIndex); });
  }

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapHypertimeMap),
      "show or hide the hypertime map", [&map] { map.toggle(); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapRadialMenu),
      "open radial menu for formatting and alignment",
      [state, radialMenu, renderer] {
        renderer->runWithState(
            [state, radialMenu, renderer](RenderState &rState) {
              if (radialMenu->isOpen()) {
                radialMenu->close();
                return;
              }
              auto *const caret   = renderer->editCaret();
              std::uint32_t doc   = 0;
              std::uint32_t start = 0;
              std::uint32_t len   = 0;
              if (caret && caret->active() &&
                  caret->documentIndex() < rState.docs.size()) {
                doc = caret->documentIndex();
                if (caret->hasSelection()) {
                  start = caret->selectionStart();
                  len   = caret->selectionEnd() - start;
                } else {
                  start = caret->byteOffset();
                }
              }
              auto mx = static_cast<float>(state->mouseX);
              auto my = static_cast<float>(state->mouseY);
              if (mx <= 0.0F && my <= 0.0F && state->clickX >= 0) {
                mx = static_cast<float>(state->clickX);
                my = static_cast<float>(state->clickY);
              }
              radialMenu->openAtWindowCoords(mx, my, doc, start, len);
            });
      });

  app.commands().registerAction(std::string(xanadu::settings::kKeymapBack),
                                "go to the previous state, losing nothing",
                                [&views] { views.back(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapNewDoc),
                                "create a new sovereign document quad",
                                [&views] { views.newDocument(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapForward),
                                "go to the next state in hypertime",
                                [&views] { views.forward(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapOpenDoc),
                                "open a document or system xanadoc",
                                [&views] { views.openDocumentPalette(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapOnionSkin),
                                "toggle 3D multi-document onion skinning mode",
                                [&views] { views.toggleOnionSkin(); });

  const auto togglePouchAction = [&pouchDrawer] { pouchDrawer.toggle(); };
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPouchToggle),
      "toggle screen-edge pouch drawer and clasp bench", togglePouchAction);
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPouchToggleF2),
      "toggle screen-edge pouch drawer and clasp bench", togglePouchAction);

  const auto toggleTelescopeAction = [&swarmTelescope] {
    swarmTelescope.toggle();
  };
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPublicationDiscovery),
      "discover signed publications by topic or author key",
      [&views, &swarmTelescope] {
        swarmTelescope.setVisible(false);
        views.discoverPublications();
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapLinkPackagePublish),
      "publish independent links", [&views, &swarmTelescope] {
        swarmTelescope.setVisible(false);
        views.publishIndependentLinks();
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapLinksResponses),
      "discover links and responses", [&views, &swarmTelescope] {
        swarmTelescope.setVisible(false);
        views.linksAndResponses();
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPublicationUpdates),
      "review verified publication updates", [&views, &swarmTelescope] {
        swarmTelescope.setVisible(false);
        views.publicationUpdates();
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapTelescopeToggle),
      "toggle decentralized swarm telescope overlay", toggleTelescopeAction);
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapTelescopeToggleF3),
      "toggle decentralized swarm telescope overlay", toggleTelescopeAction);

  const auto toggleQuotationAction = [&quotationOverlay] {
    quotationOverlay.toggle();
  };
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapQuotationToggle),
      "toggle quoted structure builder overlay", toggleQuotationAction);
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapQuotationToggleF9),
      "toggle quoted structure builder overlay (F9)", toggleQuotationAction);

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapTensionPhysicsToggle),
      "toggle 3-way tension spring layout simulation",
      [&links] { links.togglePhysics(); });

  const auto unlockTranscopyrightAction = [&views] {
    views.unlockTranscopyrightAtCaret();
  };
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapUnlockTranscopyrightF5),
      "unlock transcopyright span at caret or selection",
      unlockTranscopyrightAction);
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapUnlockTranscopyrightCtrlU),
      "unlock transcopyright span at caret or selection",
      unlockTranscopyrightAction);

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapScrubForward),
      "scrub forward in hypertime history",
      [&views] { views.scrubHistory(false); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapScrubBackward),
      "scrub backward in hypertime history",
      [&views] { views.scrubHistory(true); });

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapTransclude),
      "transclude the selection into a second document",
      [&views] { views.transcludeSelection(); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapXanalink),
      "mark one end of a xanalink, then join it to another "
      "selection -- in this document or any other open one",
      [&views] { views.linkSelection(); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapCancelLink),
      "forget a xanalink that was begun and not finished",
      [&views] { views.cancelLink(); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapBeams),
      "show or hide the links and transclusions between documents",
      [&links] { links.toggle(); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapSworph),
      "let a link coming into view bring its far document over",
      [&links] { links.setSworph(!links.sworphing()); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPublish),
      "publish the document the caret is in, so it can be read "
      "off this machine",
      [&views, &opts] {
        views.publishCurrent(opts.publishAs.empty() ? std::string{"document"}
                                                    : opts.publishAs);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPublicationStatus),
      "review publication status and retry",
      [&views] { views.publicationStatus(); });
  app.commands().registerAction("publication-status",
                                "review publication status and retry",
                                [&views] { views.publicationStatus(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapHistory),
                                "print every state to the terminal",
                                [&views] { views.printHistory(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapDelete),
                                "stop pointing at the selection",
                                [&views] { views.deleteSelection(); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapDeleteForward),
      "delete the selection or the character after the caret",
      [&views] { views.deleteForward(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapNewline),
                                "start a new line at the caret", [state] {
                                  const std::scoped_lock locker(
                                      state->typedMutex);
                                  state->typedText += '\n';
                                });
  {
    using gleditor::CaretMotion;
    namespace keys = xanadu::settings;
    const std::tuple<std::string_view, std::string_view, CaretMotion,
                     const char *>
        motions[] = {
            {keys::kKeymapCaretLeft, keys::kKeymapSelectLeft, CaretMotion::Left,
             "a character left"},
            {keys::kKeymapCaretRight, keys::kKeymapSelectRight,
             CaretMotion::Right, "a character right"},
            {keys::kKeymapCaretUp, keys::kKeymapSelectUp, CaretMotion::Up,
             "up a line"},
            {keys::kKeymapCaretDown, keys::kKeymapSelectDown, CaretMotion::Down,
             "down a line"},
            {keys::kKeymapCaretWordLeft, keys::kKeymapSelectWordLeft,
             CaretMotion::WordLeft, "to the previous word"},
            {keys::kKeymapCaretWordRight, keys::kKeymapSelectWordRight,
             CaretMotion::WordRight, "to the next word"},
            {keys::kKeymapCaretLineStart, keys::kKeymapSelectLineStart,
             CaretMotion::LineStart, "to the start of the line"},
            {keys::kKeymapCaretLineEnd, keys::kKeymapSelectLineEnd,
             CaretMotion::LineEnd, "to the end of the line"},
            {keys::kKeymapCaretDocStart,
             {},
             CaretMotion::DocumentStart,
             "to the start of the document"},
            {keys::kKeymapCaretDocEnd,
             {},
             CaretMotion::DocumentEnd,
             "to the end of the document"},
        };
    for (const auto &[move, select, motion, where] : motions) {
      app.commands().registerAction(
          std::string(move), std::string("move the caret ") + where,
          [&views, motion] { views.moveCaret(motion, false); });
      if (!select.empty()) {
        app.commands().registerAction(
            std::string(select), std::string("extend the selection ") + where,
            [&views, motion] { views.moveCaret(motion, true); });
      }
    }
  }
  app.commands().registerAction(std::string(xanadu::settings::kKeymapPageBreak),
                                "insert a page break at the caret position",
                                [&views] { views.insertPageBreakAtCaret(); });

  const auto applyDecoration = [&session,
                                renderer](const gleditor::Decoration deco) {
    renderer->runWithState([&session, renderer, deco](RenderState &rState) {
      auto *const caret = renderer->editCaret();
      if (caret && caret->active() &&
          caret->documentIndex() < rState.docs.size() &&
          caret->hasSelection()) {
        const auto doc   = caret->documentIndex();
        const auto start = caret->selectionStart();
        const auto len   = caret->selectionEnd() - start;
        session->markDecorated(doc, start, len, gleditor::decorationBit(deco));
      }
    });
  };

  app.commands().registerAction(
      "format-bold", "toggle bold on selected text",
      [applyDecoration] { applyDecoration(gleditor::Decoration::Bold); });
  app.commands().registerAction(
      "format-italic", "toggle italic on selected text",
      [applyDecoration] { applyDecoration(gleditor::Decoration::Italic); });
  app.commands().registerAction(
      "format-underline", "toggle underline on selected text",
      [applyDecoration] { applyDecoration(gleditor::Decoration::Underline); });
  app.commands().registerAction(
      "format-strikethrough", "toggle strikethrough on selected text",
      [applyDecoration] {
        applyDecoration(gleditor::Decoration::Strikethrough);
      });
  app.commands().registerAction(
      "format-superscript", "toggle superscript on selected text",
      [applyDecoration] {
        applyDecoration(gleditor::Decoration::Superscript);
      });
  app.commands().registerAction(
      "format-subscript", "toggle subscript on selected text",
      [applyDecoration] { applyDecoration(gleditor::Decoration::Subscript); });

  // Hyphenated & Legacy Script Action Aliases
  app.commands().registerAction("save", "save or preserve active document",
                                [&views] { views.saveCurrent(); });
  app.commands().registerAction("save-document",
                                "save or preserve active document",
                                [&views] { views.saveCurrent(); });
  app.commands().registerAction("close", "close active document",
                                [&views] { views.closeActive(); });
  app.commands().registerAction("close-doc", "close active document",
                                [&views] { views.closeActive(); });
  app.commands().registerAction("next-doc", "switch to next document",
                                [&views] { views.nextDoc(); });
  app.commands().registerAction("prev-doc", "switch to previous document",
                                [&views] { views.prevDoc(); });
  app.commands().registerAction("map", "show or hide the hypertime map",
                                [&map] { map.toggle(); });
  app.commands().registerAction(
      "radial-menu", "open radial menu for formatting and alignment",
      [state, radialMenu, renderer] {
        renderer->runWithState(
            [state, radialMenu, renderer](RenderState &rState) {
              if (radialMenu->isOpen()) {
                radialMenu->close();
                return;
              }
              auto *const caret   = renderer->editCaret();
              std::uint32_t doc   = 0;
              std::uint32_t start = 0;
              std::uint32_t len   = 0;
              if (caret && caret->active() &&
                  caret->documentIndex() < rState.docs.size()) {
                doc = caret->documentIndex();
                if (caret->hasSelection()) {
                  start = caret->selectionStart();
                  len   = caret->selectionEnd() - start;
                } else {
                  start = caret->byteOffset();
                }
              }
              auto mx = static_cast<float>(state->mouseX);
              auto my = static_cast<float>(state->mouseY);
              if (mx <= 0.0F && my <= 0.0F && state->clickX >= 0) {
                mx = static_cast<float>(state->clickX);
                my = static_cast<float>(state->clickY);
              }
              radialMenu->openAtWindowCoords(mx, my, doc, start, len);
            });
      });
  app.commands().registerAction("pouch-toggle", "toggle pouch drawer",
                                togglePouchAction);
  app.commands().registerAction("telescope-toggle", "toggle swarm telescope",
                                toggleTelescopeAction);
  app.commands().registerAction("quotation-toggle",
                                "toggle quoted structure builder overlay",
                                toggleQuotationAction);
  app.commands().registerAction(
      "store-manager-toggle", "toggle store object manager drawer",
      [&storeObjectManager] { storeObjectManager.toggle(); });
  app.commands().registerAction("physics-toggle",
                                "toggle 3-way tension spring layout physics",
                                [&links] { links.togglePhysics(); });
  app.commands().registerAction("unlock-transcopyright",
                                "unlock transcopyright span under caret",
                                unlockTranscopyrightAction);
  app.commands().registerAction("scrub-forward",
                                "step forward in microversion history",
                                [&views] { views.scrubHistory(false); });
  app.commands().registerAction("scrub-backward",
                                "step backward in microversion history",
                                [&views] { views.scrubHistory(true); });
  app.commands().registerAction("scrub-back",
                                "step backward in microversion history",
                                [&views] { views.scrubHistory(true); });
  app.commands().registerAction("transclude",
                                "transclude selection into new version",
                                [&views] { views.transcludeSelection(); });
  app.commands().registerAction("xanalink",
                                "connect selection with an xanalink",
                                [&views] { views.linkSelection(); });
  app.commands().registerAction("cancel-link", "drop link begun earlier",
                                [&views] { views.cancelLink(); });
  app.commands().registerAction("cancel link", "drop link begun earlier",
                                [&views] { views.cancelLink(); });
  app.commands().registerAction("beams",
                                "show or hide the ribbons connecting documents",
                                [&links] { links.toggle(); });
  app.commands().registerAction(
      "sworph", "bring far end of link into view on approach",
      [&links] { links.setSworph(!links.sworphing()); });
  app.commands().registerAction(
      "publish", "sign and seal opening document", [&views, &opts] {
        views.publishCurrent(opts.publishAs.empty() ? std::string{"document"}
                                                    : opts.publishAs);
      });
  app.commands().registerAction(
      "history", "list all microversions and operations to stdout",
      [&views] { views.printHistory(); });
  app.commands().registerAction("delete",
                                "delete selected range in active document",
                                [&views] { views.deleteSelection(); });
  app.commands().registerAction("page-break",
                                "insert a page break at the caret",
                                [&views] { views.insertPageBreakAtCaret(); });
  app.commands().registerAction("insert-break",
                                "insert a page break at the caret position",
                                [&views] { views.insertPageBreakAtCaret(); });
  app.commands().registerAction("onion-skin",
                                "toggle 3D onion skin visualization",
                                [&views] { views.toggleOnionSkin(); });
  app.commands().registerAction("back", "navigate to parent microversion",
                                [&views] { views.back(); });
  app.commands().registerAction("forward", "navigate to child microversion",
                                [&views] { views.forward(); });
  app.commands().registerAction("new-doc", "create new sovereign document",
                                [&views] { views.newDocument(); });
  app.commands().registerAction("open-doc",
                                "open document or system xanadoc palette",
                                [&views] { views.openDocumentPalette(); });
  app.commands().registerAction("export-osmic",
                                "export OSMIC text spool representation",
                                [&views] { views.exportOsmic(); });

  const auto dropSelectionToBench = [&views, &pouchDrawer,
                                     &session](const bool isLeft) {
    views.withCaret(
        [&pouchDrawer, &session,
         isLeft](RenderState &, const xanadu::Views::Where &where, Caret *) {
          if (!where.hasRange) {
            std::cout << "xuzz: select text to drop onto bench first\n";
            return;
          }
          const auto docIdx   = where.doc;
          const auto storeIdx = session->storeIndexOf(docIdx);
          const auto ver      = session->versionOf(docIdx);
          const auto spans    = session->store(storeIdx).rebuild(ver).spansFor(
              where.start, where.end - where.start);
          if (spans.empty()) {
            return;
          }
          const auto text    = session->store(storeIdx).textOf(ver);
          const auto preview = text.substr(
              where.start, std::min<std::size_t>(where.end - where.start, 64));
          xanadu::PouchItem item;
          item.itemId          = 0;
          item.span            = spans.front();
          item.previewText     = preview;
          item.originVersion   = ver;
          item.originDocIndex  = docIdx;
          item.originCharStart = where.start;
          item.originCharEnd   = where.end;
          if (isLeft) {
            pouchDrawer.forge().dropLeft(std::move(item));
            std::cout << "xuzz: dropped span onto clasp left bench: '"
                      << preview << "'\n";
          } else {
            pouchDrawer.forge().dropRight(std::move(item));
            std::cout << "xuzz: dropped span onto clasp right bench: '"
                      << preview << "'\n";
          }
        });
  };

  const auto dropSelectionToZone = [&views, &pouchDrawer,
                                    &session](const std::string_view zoneId) {
    views.withCaret([&pouchDrawer, &session,
                     zoneId](RenderState &, const xanadu::Views::Where &where,
                             Caret *) {
      if (!where.hasRange) {
        std::cout << "xuzz: select text to drop into pouch first\n";
        return;
      }
      const auto docIdx   = where.doc;
      const auto storeIdx = session->storeIndexOf(docIdx);
      const auto ver      = session->versionOf(docIdx);
      const auto spans    = session->store(storeIdx).rebuild(ver).spansFor(
          where.start, where.end - where.start);
      if (spans.empty()) {
        return;
      }
      const auto text    = session->store(storeIdx).textOf(ver);
      const auto preview = text.substr(
          where.start, std::min<std::size_t>(where.end - where.start, 64));
      const auto item = pouchDrawer.manager().dropSpan(
          zoneId, spans.front(), preview, ver, docIdx, where.start, where.end);
      std::cout << "xuzz: dropped span into pouch zone '" << zoneId
                << "' (item " << item.itemId << ")\n";
    });
  };

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPouchDropLeft),
      "drop selection onto clasp homestead bench (left)",
      [dropSelectionToBench] { dropSelectionToBench(true); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPouchDropRight),
      "drop selection onto clasp toward bench (right)",
      [dropSelectionToBench] { dropSelectionToBench(false); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPouchDrop),
      "drop selection into active pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("notes"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPouchDropNotes),
      "drop selection into notes pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("notes"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPouchDropScratch),
      "drop selection into scratch pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("scratch"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPouchDropToLinkLeft),
      "drop selection into to-link-left pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("to_link_left"); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPouchDropToLinkRight),
      "drop selection into to-link-right pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("to_link_right"); });

  app.commands().registerAction(
      "pouch-drop-left", "drop selection onto clasp homestead bench (left)",
      [dropSelectionToBench] { dropSelectionToBench(true); });
  app.commands().registerAction(
      "pouch-drop-right", "drop selection onto clasp toward bench (right)",
      [dropSelectionToBench] { dropSelectionToBench(false); });
  app.commands().registerAction(
      "pouch-drop", "drop selection into active pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("notes"); });
  app.commands().registerAction(
      "pouch-drop-notes", "drop selection into notes pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("notes"); });
  app.commands().registerAction(
      "pouch-drop-scratch", "drop selection into scratch pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("scratch"); });
  app.commands().registerAction(
      "pouch-drop-to-link-left", "drop selection into to-link-left pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("to_link_left"); });
  app.commands().registerAction(
      "pouch-drop-to-link-right",
      "drop selection into to-link-right pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("to_link_right"); });

  const auto forgeClaspAction = [&pouchDrawer, &session, renderer] {
    renderer->runWithState([&pouchDrawer, &session, renderer](RenderState &) {
      if (!pouchDrawer.forge().canForge()) {
        std::cout << "xuzz: clasp forge requires items on "
                     "both left and right benches\n";
        return;
      }
      auto *const caret = renderer->editCaret();
      const auto docIdx = (nullptr != caret && caret->active() &&
                           caret->documentIndex() < session->views().size())
                              ? caret->documentIndex()
                              : 0U;
      if (pouchDrawer.forge().forge(*session, docIdx)) {
        std::cout << "xuzz: forged clasp link on active document " << docIdx
                  << "\n";
      }
    });
  };
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapForgeClasp),
      "forge bilateral clasp link from items on bench", forgeClaspAction);
  app.commands().registerAction(
      "forge-clasp", "forge bilateral clasp link from items on bench",
      forgeClaspAction);

  app.commands().registerAction(
      "clear-bench", "clear items from clasp forge bench", [&pouchDrawer] {
        pouchDrawer.forge().clearLeft();
        pouchDrawer.forge().clearRight();
        std::cout << "xuzz: cleared clasp forge bench\n";
      });

  // Selected-link navigation
  using namespace xanadu::settings;
  const std::pair<std::string_view, const char *> linkActions[] = {
      {kKeymapLinkNext, "select the next link on screen"},
      {kKeymapLinkPrevious, "select the previous link on screen"},
      {kKeymapLinkMemberNext, "choose the next member on the active side"},
      {kKeymapLinkMemberPrevious,
       "choose the previous member on the active side"},
      {kKeymapLinkOccurrenceNext, "choose the next place the member appears"},
      {kKeymapLinkOccurrencePrevious,
       "choose the previous place the member appears"},
      {kKeymapLinkCross, "make the other side of the link active"},
      {kKeymapLinkEnter, "go to the chosen place"},
      {kKeymapLinkOrigin, "return to where the link was selected"},
      {kKeymapLinkDismiss, "put the selected link away"},
      {kKeymapActivityBack, "return to the previous visit"},
  };
  for (const auto &[name, help] : linkActions) {
    const auto command = xanadu::commandForAction(name);
    if (!command) {
      continue;
    }
    app.commands().registerAction(
        std::string(name), help, [renderer, &linkContext, command = *command] {
          renderer->runWithState([&linkContext, command](RenderState &) {
            std::ignore = linkContext.execute(command);
          });
        });
  }

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapOverviewToggle),
      "show or hide the overview of every open page", [&renderer, &overview] {
        renderer->runWithState(
            [&overview](RenderState &) { overview.toggle(); });
      });

  // B. Zigzag & Bridging Commands
  zigzag::registerZigzagCommands(
      app.commands(), zigzagVisualizer,
      {.activateFocus =
           [&keyboardPane, zigzagPresentation, &bridgeCoordinator] {
             keyboardPane.leaveZigzag(false);
             bridgeCoordinator.activateCell(zigzagPresentation->focusCell());
           },
       .leave = [&keyboardPane] { keyboardPane.leaveZigzag(true); },
       .focusMoved =
           [&keyboardPane, &showZigzagFocus] {
             keyboardPane.enterZigzag();
             showZigzagFocus();
           },
       .dispatch =
           [renderer](std::function<void()> action) {
             renderer->runWithState(
                 [action = std::move(action)](RenderState &) { action(); });
           }});

  const auto startSlice =
      [&views, &session, &keyboardPane, &bindZigzag, &state,
       zigzagPresentation](RenderState &rState, const std::size_t storeIndex,
                           const xanadu::MicroversionId &parent) {
        auto &store = session->store(storeIndex);
        if (store.homeCell() != zigzag::noCell) {
          state->showDialog(render::DiagnosticSeverity::Info,
                            "Store already has a slice",
                            "Open its existing slice to continue editing it.");
          return;
        }
        const auto version = store.sliceGenesis(parent);
        session->save(storeIndex);
        bindZigzag(rState, storeIndex, version);
        keyboardPane.enterZigzag();
        views.placeCameraWhenReady(
            [&views, &state, zigzagPresentation, placedFrames = 0]() mutable {
              if (!views.presentationTransform() || ++placedFrames < 2)
                return false;
              if (const auto centre = zigzagPresentation->focusCentre()) {
                std::scoped_lock locker(state->view);
                state->view.pos.x = centre->x;
                state->view.pos.y = centre->y;
              }
              return true;
            });
        std::cout << "xuzz: started a new slice (store " << storeIndex << ")\n";
      };
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapNewSlice),
      "start a new ZigZag slice, in a new xanadoc of its own",
      [&views, &session, &renderer, startSlice] {
        const auto storeIndex = views.newDocument();
        renderer->runWithState(
            [&session, startSlice, storeIndex](RenderState &rState) {
              startSlice(rState, storeIndex,
                         session->store(storeIndex).primaryCurrentVersion());
            });
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapNewSliceInStore),
      "add a ZigZag slice to the current document's store",
      [&views, &session, startSlice] {
        views.withCaret(
            [&session, startSlice](RenderState &rState,
                                   const xanadu::Views::Where &where, Caret *) {
              startSlice(rState, session->storeIndexOf(where.doc),
                         session->versionOf(where.doc));
            });
      });

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapFocusToggle),
      "move the keyboard between the document and ZigZag",
      [&keyboardPane, &showZigzagFocus] {
        if (keyboardPane.inZigzag()) {
          keyboardPane.leaveZigzag(true);
        } else {
          keyboardPane.enterZigzag();
          showZigzagFocus();
        }
      });

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapViewXanadocs), "show only xanadocs",
      [&viewCoordinator] {
        viewCoordinator.setViewMode(ViewMode::XanadocOnly);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapViewSlices), "show only slices",
      [&viewCoordinator] {
        viewCoordinator.setViewMode(ViewMode::ZigzagOnly);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapViewBoth),
      "show xanadocs and slices together",
      [&viewCoordinator] { viewCoordinator.setViewMode(ViewMode::Unified); });

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapLinkAddCell),
      "add the focused cell to the pending document link",
      [&renderer, &views, &bridgeCoordinator, zigzagPresentation] {
        renderer->runWithState(
            [&views, &bridgeCoordinator, zigzagPresentation](RenderState &) {
              const auto *manifold = bridgeCoordinator.manifold();
              const auto cell      = zigzagPresentation->focusCell();
              if (manifold == nullptr || cell == zigzag::noCell) return;
              views.addCellToPendingLink(manifold->contentOf(cell));
            });
      });

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapLinkFinish),
      "finish the pending document-to-cell link", [&renderer, &views] {
        renderer->runWithState(
            [&views](RenderState &) { views.finishCellLink(); });
      });

  std::vector<xanadu::PrimediaSpan> quotedCellSpans;
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapTranscludeCellToDoc),
      "transclude the focused cell content at the document caret",
      [&renderer, &views, &bridgeCoordinator, zigzagPresentation,
       &quotedCellSpans] {
        renderer->runWithState([&views, &bridgeCoordinator, zigzagPresentation,
                                &quotedCellSpans](RenderState &) {
          const auto *manifold = bridgeCoordinator.manifold();
          const auto cell      = zigzagPresentation->focusCell();
          if (manifold == nullptr || cell == zigzag::noCell) return;
          const auto content = manifold->contentOf(cell);
          if (content.empty()) {
            std::cout << "xuzz: focused cell has no content to "
                         "transclude\n";
            return;
          }
          quotedCellSpans.assign(content.begin(), content.end());
          views.transcludeSpansAtCaret(quotedCellSpans);
        });
      });

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapTranscludeCellToCell),
      "transclude the same content into a new connected cell",
      [&renderer, &session, &bridgeCoordinator, zigzagPresentation,
       &zigzagStoreIndex, &quotedCellSpans, &linkContext] {
        renderer->runWithState([&session, &bridgeCoordinator,
                                zigzagPresentation, &zigzagStoreIndex,
                                &quotedCellSpans, &linkContext](RenderState &) {
          if (quotedCellSpans.empty()) {
            std::cout << "xuzz: transclude cell content to a document "
                         "first\n";
            return;
          }
          if (zigzagPresentation->insertConnectedTransclusion(
                  quotedCellSpans)) {
            session->save(zigzagStoreIndex);
            bridgeCoordinator.synchronize();
            linkContext.setManifold(bridgeCoordinator.manifold(),
                                    zigzagStoreIndex,
                                    zigzagPresentation->sliceHead());
            std::cout << "xuzz: transcluded into new cell "
                      << zigzagPresentation->focusCell() << "\n";
          }
        });
      });

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapInsertExternRef),
      "choose a cell in another loaded slice and reference its birth",
      [&renderer, &session, &publishForm, &bindZigzag, zigzagPresentation,
       &zigzagStoreIndex] {
        renderer->runWithState([&renderer, &session, &publishForm, &bindZigzag,
                                zigzagPresentation,
                                &zigzagStoreIndex](RenderState &) {
          gleditor::Form::Field choice;
          choice.label         = "Foreign cell";
          choice.hint          = "Select a cell from another open slice";
          choice.kind          = gleditor::Form::Kind::Choice;
          choice.submitOnEnter = true;
          for (std::size_t i = 0; i < session->storeCount(); ++i) {
            if (i == zigzagStoreIndex) continue;
            const auto &foreign = session->store(i);
            if (foreign.isSystem() || foreign.homeCell() == zigzag::noCell)
              continue;
            const auto folded = foreign.rebuildManifold(foreign.latest());
            for (const auto &cell : folded.cells()) {
              if (cell.birthOp == folded.home() ||
                  cell.birthOp == folded.dimsDimension())
                continue;
              const auto label = folded.textOf(cell.birthOp, foreign);
              choice.options.push_back(std::to_string(i) + ": cell #" +
                                       std::to_string(cell.birthOp) + " " +
                                       label.substr(0, 48));
              choice.optionValues.push_back(std::to_string(i) + ":" +
                                            std::to_string(cell.birthOp));
            }
          }
          if (choice.options.empty()) {
            std::cout << "xuzz: load another slice before adding a "
                         "foreign reference\n";
            return;
          }
          const auto localIndex = zigzagStoreIndex;
          const auto focus      = zigzagPresentation->focusCell();
          publishForm.open(
              "Reference a cell in another slice",
              "The placeholder keeps that slice's identity and the "
              "cell's original operation",
              {std::move(choice)},
              [&renderer, &session, &bindZigzag, zigzagPresentation, localIndex,
               focus](const std::vector<gleditor::Form::Field> &answers) {
                if (answers.empty()) return;
                const auto selected = answers.front().answer();
                const auto colon    = selected.find(':');
                if (colon == std::string::npos) return;
                const auto foreignIndex =
                    std::stoull(selected.substr(0, colon));
                const auto foreignCell = static_cast<zigzag::CellRef>(
                    std::stoul(selected.substr(colon + 1)));
                renderer->runWithState([&session, &bindZigzag,
                                        zigzagPresentation, localIndex, focus,
                                        foreignIndex,
                                        foreignCell](RenderState &rState) {
                  if (foreignIndex >= session->storeCount() ||
                      foreignIndex == localIndex)
                    return;
                  const auto &foreign = session->store(foreignIndex);
                  const auto birth = foreign.segmentedOps().idOf(foreignCell);
                  if (birth.isZero()) return;
                  auto &local = session->store(localIndex);
                  auto head =
                      local.registerScroll(zigzagPresentation->sliceHead(),
                                           foreign.documentId().str());
                  const auto scroll = local.scrollRegistry().scrollIdForKey(
                      foreign.documentId().str());
                  if (!scroll) return;
                  const xanadu::ExternOpRef target{.scroll   = *scroll,
                                                   .produces = birth};
                  head                   = local.makeExternRef(head, target);
                  const auto placeholder = local.placeholderForExtern(target);
                  auto folded            = local.rebuildManifold(head);
                  const auto dim         = folded.dimensionNamed("d.1", local);
                  if (!placeholder || !dim) return;
                  const auto anchor =
                      folded.contains(focus) ? focus : folded.home();
                  const auto next =
                      folded.linked(anchor, *dim, zigzag::DimVector::POS);
                  head = local.setLink(head, anchor, *dim,
                                       zigzag::DimVector::POS, *placeholder);
                  if (next != zigzag::noCell && next != *placeholder) {
                    folded = local.rebuildManifold(head);
                    head   = local.setLink(head, *placeholder, *dim,
                                           zigzag::DimVector::POS, next);
                  }
                  bindZigzag(rState, localIndex, head);
                  zigzagPresentation->focusCell(*placeholder);
                  session->save(localIndex);
                });
              });
        });
      });

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapActivityForward),
      "choose one of the current visit's forward branches",
      [&renderer, &linkContext, &publishForm] {
        renderer->runWithState([&renderer, &linkContext,
                                &publishForm](RenderState &) {
          const auto choices = linkContext.forwardChoices();
          if (choices.empty()) {
            std::cout << "xuzz: no forward activity visit\n";
            return;
          }
          if (choices.size() == 1) {
            std::ignore = linkContext.execute(
                xanadu::nav::ActivityForward{.child = choices.front().id});
            return;
          }
          gleditor::Form::Field choice;
          choice.label         = "Forward visit";
          choice.hint          = "choose one saved branch";
          choice.kind          = gleditor::Form::Kind::Choice;
          choice.submitOnEnter = true;
          for (const auto &visit : choices) {
            choice.options.push_back("Visit " + std::to_string(visit.id.value) +
                                     ": " + linkContext.describe(visit.target));
            choice.optionValues.push_back(std::to_string(visit.id.value));
          }
          publishForm.open(
              "Activity Forward", "Choose a saved destination",
              {std::move(choice)},
              [&renderer, &linkContext](
                  const std::vector<gleditor::Form::Field> &answers) {
                if (answers.empty() || answers.front().answer().empty()) return;
                const auto child = xanadu::VisitId{
                    .value = std::stoull(answers.front().answer())};
                renderer->runWithState([&linkContext, child](RenderState &) {
                  std::ignore = linkContext.execute(
                      xanadu::nav::ActivityForward{.child = child});
                });
              });
        });
      });

  // C. View Mode Toggling Commands
  app.commands().registerAction(
      "std:xuzz/cycle_view_mode",
      "cycle view mode (Unified <-> Xanadoc <-> ZigZag)",
      [&viewCoordinator] { viewCoordinator.cycleViewMode(); });
  app.commands().registerAction(
      "std:xuzz/view_unified",
      "switch to Unified view (Xanadoc + ZigZag bridge)",
      [&viewCoordinator] { viewCoordinator.setViewMode(ViewMode::Unified); });
  app.commands().registerAction(
      "std:xuzz/view_xanadoc", "switch to Xanadoc Only view (ZigZag suspended)",
      [&viewCoordinator] {
        viewCoordinator.setViewMode(ViewMode::XanadocOnly);
      });
  app.commands().registerAction(
      "std:xuzz/view_zigzag", "switch to ZigZag Only view (Hypergrid centered)",
      [&viewCoordinator] {
        viewCoordinator.setViewMode(ViewMode::ZigzagOnly);
      });

  // D. Scope & Keyboard Routing
  app.commands().setScopeResolver(
      [&keyboardPane] { return keyboardPane.scope(); });
  state->documentTakesText = [&keyboardPane] {
    return !keyboardPane.inZigzag();
  };
  zigzagPresentation->setHasKeyboard(false);
  keyboardPane.setChangeHandler([zigzagPresentation](const bool zigzagHas) {
    zigzagPresentation->setHasKeyboard(zigzagHas);
  });
  for (const auto &command : std::vector(app.commands().all())) {
    app.commands().setScope(command.name,
                            std::string(xanadu::keymapScope(command.name)));
  }

  const auto showKeyHints = [&app, &renderer, zigzagPresentation] {
    auto [here, elsewhere] = zigzag::zigzagKeyHints(
        app.commands(), xanadu::settings::kKeymapFocusToggle);
    renderer->runWithState([zigzagPresentation, here = std::move(here),
                            elsewhere =
                                std::move(elsewhere)](RenderState &) mutable {
      zigzagPresentation->setKeyHints(std::move(here), std::move(elsewhere));
    });
  };

  const auto applyTypography = [state](const xanadu::UIConfig &config) {
    state->uiScale           = config.uiScale;
    state->fontScale         = config.uiFontScale;
    state->uiSafeMarginShare = config.uiSafeMarginShare;
    state->uiTheme.store(
        std::make_shared<const gleditor::ui::Theme>(config.uiTheme));
  };

  // 8. System Store Change Watcher Callback
  session->setSystemDocChangedCallback(
      [&app, radialMenu, docSwitcher, &pouchDrawer, &links, &map, &linkPanel,
       &views, &overview, &storeObjectManager, &quotationOverlay,
       &swarmTelescope, &satelloidOverlay, &kineticTetherOverlay,
       &wireframeHullOverlay, readablePx, &session, zigzagPresentation,
       zigzagVisualizer, &bridgeCoordinator, &showKeyHints, state,
       applyTypography](const xanadu::SystemDocKind kind,
                        const xanadu::Store &store) {
        std::cout << "xuzz: system doc updated (" << xanadu::systemDocUri(kind)
                  << ")\n";
        const auto model = xanadu::SystemStoreModel::fromStore(store);
        if (!model.isValid()) {
          std::cerr << "xuzz: rejecting invalid system store "
                    << xanadu::systemDocUri(kind) << ": "
                    << model.validationError() << "\n";
          return;
        }
        switch (kind) {
        case xanadu::SystemDocKind::Keymap: {
          const auto vHost =
              zigzagVisualizer ? zigzagVisualizer->vortexHost() : nullptr;
          state->focusManager.setGlobalCommandAllowList(
              xanadu::KeymapConfig::fromStore(store).modalGlobalCommands);
          applyKeymap(app.commands(), store, zigzagPresentation, vHost);
          showKeyHints();
          break;
        }
        case xanadu::SystemDocKind::Settings: {
          session->setAutoSave(std::chrono::seconds(
              xanadu::SettingsConfig::fromStore(store).autoSaveSeconds));
          if (auto vHost = zigzagVisualizer->vortexHost()) {
            vHost->loadConfigFromStore(store);
          }
          break;
        }
        case xanadu::SystemDocKind::Layout: {
          const auto layout = xanadu::LayoutConfig::fromStore(store);
          views.setReadableTextPx(readablePx(layout.readableTextPx));
          links.setReadableTextPx(readablePx(layout.readableTextPx));
          links.setVisible(layout.xanalinkRibbons);
          links.setBeamConfig(layout.beams);
          links.tensionEngine().setParams(layout.physics.toTensionParams());
          pouchDrawer.setDockSide(layout.pouchDock == xanadu::PouchDock::Left
                                      ? xanadu::PouchDrawer::DockSide::Left
                                      : xanadu::PouchDrawer::DockSide::Right);
          zigzagPresentation->setPresentationConfig(layout.zigzag);
          bridgeCoordinator.applyConfig(layout.bridge);
          break;
        }
        case xanadu::SystemDocKind::UI: {
          const auto uiCfg = xanadu::UIConfig::fromStore(store);
          applyTypography(uiCfg);
          radialMenu->setConfig(uiCfg.radialMenu);
          linkPanel.setConfig(uiCfg.linkPanel);
          overview.setConfig(uiCfg.overview);
          pouchDrawer.setConfig(uiCfg.pouchPanel);
          storeObjectManager.setConfig(uiCfg.storePanel);
          satelloidOverlay.setConfig(uiCfg.satelloidCard);
          kineticTetherOverlay.setConfig(uiCfg.tetherCard);
          wireframeHullOverlay.setConfig(uiCfg.hullCard);
          quotationOverlay.setConfig(uiCfg.quotationModal);
          swarmTelescope.setConfig(uiCfg.telescopeModal);
          map.setConfig(uiCfg.hypertimeModal);
          docSwitcher->setVisible(uiCfg.tabBarVisible);
          map.setVisible(uiCfg.hypertimeMapVisible);
          break;
        }
        case xanadu::SystemDocKind::Pouches:
        case xanadu::SystemDocKind::Count:
          break;
        }
      });

  // Apply active system doc configurations at launch
  {
    session->setAutoSave(std::chrono::seconds(
        xanadu::SettingsConfig::fromStore(
            session->systemStore(xanadu::SystemDocKind::Settings))
            .autoSaveSeconds));
    const auto kmIdx = session->systemStoreIndex(xanadu::SystemDocKind::Keymap);
    const auto &kmStore = session->store(kmIdx);
    state->focusManager.setGlobalCommandAllowList(
        xanadu::KeymapConfig::fromStore(kmStore).modalGlobalCommands);
    if (kmStore.opCount() > 0) {
      const auto vHost =
          zigzagVisualizer ? zigzagVisualizer->vortexHost() : nullptr;
      applyKeymap(app.commands(), kmStore, zigzagPresentation, vHost);
    }
    showKeyHints();
    const auto uiIdx    = session->systemStoreIndex(xanadu::SystemDocKind::UI);
    const auto &uiStore = session->store(uiIdx);
    if (uiStore.opCount() > 0) {
      const auto uiCfg = xanadu::UIConfig::fromStore(uiStore);
      applyTypography(uiCfg);
      radialMenu->setConfig(uiCfg.radialMenu);
      linkPanel.setConfig(uiCfg.linkPanel);
      overview.setConfig(uiCfg.overview);
      pouchDrawer.setConfig(uiCfg.pouchPanel);
      storeObjectManager.setConfig(uiCfg.storePanel);
      satelloidOverlay.setConfig(uiCfg.satelloidCard);
      kineticTetherOverlay.setConfig(uiCfg.tetherCard);
      wireframeHullOverlay.setConfig(uiCfg.hullCard);
      quotationOverlay.setConfig(uiCfg.quotationModal);
      swarmTelescope.setConfig(uiCfg.telescopeModal);
      map.setConfig(uiCfg.hypertimeModal);
      docSwitcher->setVisible(uiCfg.tabBarVisible);
      map.setVisible(uiCfg.hypertimeMapVisible);
    }
    const auto loIdx = session->systemStoreIndex(xanadu::SystemDocKind::Layout);
    const auto &loStore = session->store(loIdx);
    if (loStore.opCount() > 0) {
      const auto loCfg = xanadu::LayoutConfig::fromStore(loStore);
      views.setReadableTextPx(readablePx(loCfg.readableTextPx));
      links.setReadableTextPx(readablePx(loCfg.readableTextPx));
      links.setVisible(loCfg.xanalinkRibbons);
      links.setBeamConfig(loCfg.beams);
      links.tensionEngine().setParams(loCfg.physics.toTensionParams());
      pouchDrawer.setDockSide(loCfg.pouchDock == xanadu::PouchDock::Left
                                  ? xanadu::PouchDrawer::DockSide::Left
                                  : xanadu::PouchDrawer::DockSide::Right);
      zigzagPresentation->setPresentationConfig(loCfg.zigzag);
      bridgeCoordinator.applyConfig(loCfg.bridge);
    }
  }

  if (!opts.quiet) {
    std::cout << "xuzz commands:\n" << app.commands().helpText();
  }

  renderer->setShutdownHook(
      [&views](RenderState &) { views.keepFinalPlace(); });

  // 9. Run application loop
  const auto status = app.run();

  // 10. Persistence & Exit Cleanup
  {
    auto place              = views.finalPlace();
    place.zigzagStore       = session->path(zigzagStoreIndex);
    place.zigzagVersion     = zigzagPresentation->sliceHead().str();
    place.zigzagFocus       = zigzagPresentation->focusCell();
    place.zigzagHasKeyboard = keyboardPane.inZigzag();
    session->rememberPlace(place);
  }
  session->saveAll();
  if (opts.exportOsmic) {
    session->saveOsmicTextAll();
  }
  if (!opts.dumpPermascrollPath.empty()) {
    session->dumpPermascroll(opts.dumpPermascrollPath);
  }
  return status;
}

} // namespace xuzz
