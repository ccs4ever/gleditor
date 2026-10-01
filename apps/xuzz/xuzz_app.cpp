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
#include <gleditor/audio.hpp>
#include <gleditor/audio_widget.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc_switcher.hpp>
#include <gleditor/form.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/modal_input.hpp>
#include <gleditor/radial_menu.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/state.hpp>

#include "cli.hpp"
#include "view_coordinator.hpp"

#include "common/ui/quotation_builder_overlay.hpp"
#include "common/ui/store_object_manager.hpp"
#include "common/xanadu/config.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/provenance.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/torrent.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/zz_xudu_projector.hpp"
#include "common/xanadu/zigzag/zzcore.hpp"

#include "xudu/batch_orchestrator.hpp"
#include "xudu/beams.hpp"
#include "xudu/bridge_coordinator.hpp"
#include "xudu/hypertime_graph.hpp"
#include "xudu/kinetic_tether_overlay.hpp"
#include "xudu/pouch_drawer.hpp"
#include "xudu/satelloid.hpp"
#include "xudu/session.hpp"
#include "xudu/swarm_telescope_overlay.hpp"
#include "xudu/tenuous_tether.hpp"
#include "xudu/views.hpp"
#include "zigzag/zigzag_visualizer.hpp"

namespace fs = std::filesystem;

namespace xuzz {

namespace {

constexpr float kBackgroundDepthZ = -500.0F;

} // namespace

XuzzApp::XuzzApp()  = default;
XuzzApp::~XuzzApp() = default;

int XuzzApp::executeCheckAuthorship(const std::string &where) {
  const fs::path given(where);
  const auto record =
      fs::is_directory(given) ? given / xudu::provenanceFileName : given;
  const auto sig = fs::path(record.string() + ".asc");

  const auto slurp = [](const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
  };
  xudu::SignedProvenance sealed{.tsv = slurp(record), .signature = slurp(sig)};
  if (sealed.tsv.empty()) {
    std::cerr << "no authorship record at " << record << "\n";
    return 1;
  }

  const auto check = xudu::verifyProvenance(sealed);
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

  if (const auto said = xudu::parseProvenance(sealed.tsv); said) {
    const auto content = record.parent_path() / xudu::sealedContentName;
    if (const auto bytes = slurp(content); !bytes.empty()) {
      const auto matches = xudu::sha256Hex(bytes) == said->contentDigest &&
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
    const auto ops = record.parent_path() / xudu::sealedOpsName;
    if (said->opsDigest.empty() && 0 == said->opsLength) {
      std::cout
          << "      but it says nothing about the history sealed beside it\n";
      return 0;
    }
    const auto opsBytes  = slurp(ops);
    const auto opsAgrees = xudu::sha256Hex(opsBytes) == said->opsDigest &&
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
    std::cout << "# " << xudu::configPath() << "\n"
              << xudu::loadConfig().toTsv();
    return 0;
  }

  if (!opts.checkAuthorshipPath.empty()) {
    return executeCheckAuthorship(opts.checkAuthorshipPath);
  }

  if (opts.rasterMode) {
    return executeRaster(opts.slicePath, opts.storePath);
  }

  // 1. Initialize sovereign Permascroll
  std::shared_ptr<xudu::UserPermascroll> userPermascroll;
  if (!opts.permascrollPath.empty()) {
    xudu::UserPermascroll::Config config;
    config.storageDir = opts.permascrollPath;
    userPermascroll =
        std::make_shared<xudu::UserPermascroll>(std::move(config));
  } else {
    userPermascroll = xudu::PermascrollRegistry::instance().defaultUser();
  }

  // 2. Initialize Session
  auto session =
      std::make_unique<xudu::Session>(opts.storePath, userPermascroll);
  state->onDecoratedInsert = [&session](Doc &doc, const std::uint32_t at,
                                        const std::uint32_t length,
                                        const gleditor::DecorationMask mask) {
    session->markDecorated(doc, at, length, mask);
  };

  // 3. Batch Orchestration
  const auto batchRes =
      xudu::BatchOrchestrator::execute(*session, parser, opts.quiet);
  if (batchRes.shouldExit) {
    return batchRes.exitCode;
  }
  auto opening            = batchRes.opening;
  const auto extraImports = batchRes.extraImports;

  if (parser.present<std::vector<std::string>>("--read")) {
    for (const auto &file : parser.get<std::vector<std::string>>("--read")) {
      opts.read.push_back(session->readPublication(file));
    }
    session->save(0);
  }

  if (opening.isZero()) {
    if (!opts.askedVersion.empty()) {
      opening = xanadu::MicroversionId::parse(opts.askedVersion);
    } else if (!opts.read.empty()) {
      opening = opts.read.front();
    } else {
      opening = session->store(0).latest();
    }
  }

  if (!opts.publishAs.empty()) {
    const auto manifest = session->publishDocument(
        opening,
        xudu::Session::PublishRequest{.salt       = opts.publishAs,
                                      .title      = opts.publishAs,
                                      .author     = {},
                                      .extra      = {},
                                      .passphrase = {}},
        0);
    if (!opts.quiet) {
      std::cout << "xudu: published " << opening.str() << " as " << manifest
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

  xudu::HypertimeMap map("Sans 10", *session);
  map.setVisible(opts.mapVisible);

  xudu::PouchDrawer pouchDrawer(*session, renderer, "Sans 10");
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

  xudu::ImageOverlay images("Sans 11");
  auto docSwitcher = std::make_shared<gleditor::DocumentSwitcher>("Sans 10");
  gleditor::Form publishForm("Sans 11");

  xudu::Views views(*session, renderer, map, images, publishForm, state,
                    docSwitcher);

  xudu::SwarmCatalog swarmCatalog;
  xudu::SwarmTelescopeOverlay swarmTelescope(swarmCatalog, renderer, "Sans 10");
  swarmTelescope.setOnSummon([&views](const xudu::PublicationEntry &entry) {
    views.summonPublication(entry);
  });
  if (opts.telescopeVisible) {
    swarmTelescope.setVisible(true);
  }

  xanadu::QuotationBuilderOverlay quotationOverlay(
      session->store(),
      session->views().empty() ? xanadu::MicroversionId{}
                               : session->versionOf(0),
      renderer, &swarmCatalog, "Sans 10",
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
      session->store(), "Sans 10",
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
      [&views](const xudu::PouchItem &item) { views.swingBackToSpan(item); });

  xudu::KineticTetherEngine kineticTetherEngine;
  kineticTetherEngine.setVoidSpawnHandler(
      [&views](const xudu::TetherPayload &payload, const float sx,
               const float sy) {
        views.spawnTranscludedDocument(payload, sx, sy);
      });

  auto radialMenu = std::make_shared<gleditor::RadialMenu>("Sans 11");
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

  xudu::LinkBeams links(*session, renderer);
  links.setVisible(!opts.noBeams);
  links.setSworph(!opts.noSworph);
  if (opts.physicsEnabled) {
    links.setPhysicsEnabled(true);
  }
  xudu::TenuousTetherOverlay tenuousTetherOverlay(renderer, nullptr);
  links.setTetherOverlay(&tenuousTetherOverlay);
  xudu::SatelloidOverlay satelloidOverlay(renderer);
  links.setSatelloidOverlay(&satelloidOverlay);

  // 5. Zigzag presentation & BridgeCoordinator
  auto zigzagPresentation =
      std::make_shared<zigzag::ZigzagVisualizer>(state->defaultFontName);
  auto &bridgeStore = session->store(0);
  zigzagPresentation->bindXuduStore(bridgeStore,
                                    bridgeStore.primaryCurrentVersion());
  const auto initialLayout = xudu::LayoutConfig::fromStore(
      session->systemStore(xudu::SystemDocKind::Layout));
  zigzagPresentation->setPresentationConfig(initialLayout.zigzag);
  if (auto vHost = zigzagPresentation->vortexHost()) {
    vHost->loadConfigFromStore(
        session->systemStore(xudu::SystemDocKind::Settings));
    vHost->loadMacrosFromStore(
        session->systemStore(xudu::SystemDocKind::Keymap));
  }
  zigzagPresentation->setPresentationTransformResolver(
      [&views] { return views.presentationTransform(); });

  xudu::BridgeCoordinator bridgeCoordinator(links, renderer,
                                            *state->accessibility);
  bridgeCoordinator.connectSatelloidNavigation(satelloidOverlay);
  bridgeCoordinator.setDocumentFocusHandler(
      [&views](const zigzag::CellRef cell, const xudu::PrimediaSpan &span) {
        views.focusSpan(cell, span);
      });
  bridgeCoordinator.attach(*zigzagPresentation);
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
  renderer->addFrameContributor(&images);
  renderer->addFrameContributor(&views);
  renderer->addFrameContributor(radialMenu.get());
  renderer->addFrameContributor(&publishForm);
  renderer->addFrameContributor(&pouchDrawer);
  renderer->addFrameContributor(&swarmTelescope);
  renderer->addFrameContributor(&quotationOverlay);
  renderer->addFrameContributor(&storeObjectManager);

  state->accessibility->addSource(docSwitcher.get());
  state->accessibility->addSource(&links);
  state->accessibility->addSource(&map);
  state->accessibility->addSource(&publishForm);
  state->accessibility->addSource(radialMenu.get());
  state->accessibility->addSource(&pouchDrawer);
  state->accessibility->addSource(&quotationOverlay);
  state->accessibility->addSource(&storeObjectManager);
  state->accessibility->setToolkit("xuzz", TOSTRING(GLEDITOR_VERSION));

  map.setGoer(
      [&views](const xanadu::MicroversionId &id) { views.showOnly(id); });
  map.setScrubHandler(
      [&views](const xanadu::MicroversionId &id) { views.showOnly(id); });
  map.setCompareHandler([&views, &session, &renderer](
                            const std::vector<xanadu::MicroversionId> &vers) {
    const auto count = session->views().size();
    for (std::size_t i = 0; i < count; i++) {
      renderer->push(RenderItemCloseDoc());
    }
    renderer->runWithState(
        [&session](RenderState &) { session->clearViews(); });
    for (const auto &v : vers) {
      views.showAlongside(v, 0.0F, 0);
    }
  });
  map.setOnionSkinHandler([&views, &session, &renderer](
                              const std::vector<xanadu::MicroversionId> &vers) {
    const auto count = session->views().size();
    for (std::size_t i = 0; i < count; i++) {
      renderer->push(RenderItemCloseDoc());
    }
    renderer->runWithState(
        [&session](RenderState &) { session->clearViews(); });
    for (const auto &v : vers) {
      views.showAlongside(v, 0.0F, 0);
    }
    views.setOnionSkin(true);
  });
  map.setQuoteHandler([&session, &views](const xanadu::MicroversionId &srcVer,
                                         const std::uint32_t srcAt,
                                         const std::uint32_t srcLen) {
    if (session->views().empty()) {
      return;
    }
    auto &st           = session->store(0);
    const auto headVer = session->views().front().version;
    const auto headLen = static_cast<std::uint32_t>(st.textOf(headVer).size());
    st.transclude(headVer, headLen, srcVer, srcAt, srcLen);
    views.showOnly(headVer);
  });

  gleditor::CompositeModalInput compositeModal(
      {&publishForm, zigzagPresentation.get(), &quotationOverlay});
  state->modal = &compositeModal;

  renderer->addPickObserver(docSwitcher.get());
  renderer->addPickObserver(&links);
  renderer->addPickObserver(radialMenu.get());
  renderer->addPickObserver(&map);
  renderer->addPickObserver(&pouchDrawer);
  renderer->addPickObserver(&swarmTelescope);
  renderer->addPickObserver(&quotationOverlay);
  renderer->addPickObserver(&storeObjectManager);

  state->mouseDownHandler =
      [&kineticTetherEngine, &session, renderer, state, zigzagPresentation](
          const int mx, const int my, const std::uint8_t button) -> bool {
    if (button != 1) {
      return false;
    }
    const auto modState = SDL_GetModState();
    const bool altHeld  = (0 != (modState & SDL_KMOD_ALT));
    bool dragStarted    = false;

    renderer->runWithState([&kineticTetherEngine, &session, renderer,
                            &dragStarted, mx, my, state, altHeld,
                            zigzagPresentation](RenderState &rState) {
      auto *const caret = rState.caret;
      if (caret && caret->hasSelection()) {
        const auto selStart = caret->selectionStart();
        const auto selEnd   = caret->selectionEnd();
        const auto docIdx   = caret->documentIndex();
        if (docIdx < session->views().size() && selEnd > selStart) {
          const auto &openView = session->views()[docIdx];
          const auto &st       = session->store(openView.storeIndex);
          const auto ver       = st.rebuild(openView.version);
          const auto spans     = ver.spansFor(selStart, selEnd - selStart);
          if (!spans.empty()) {
            const auto text = st.textOf(openView.version);
            std::string preview;
            if (selStart < text.size()) {
              preview = text.substr(selStart, std::min(selEnd - selStart, 40U));
            }

            const auto screenX = static_cast<float>(mx);
            const auto screenY =
                static_cast<float>(state->view.screenHeight - my);

            xudu::TetherPayload payload{
                .span             = spans.front(),
                .previewText      = std::move(preview),
                .originVersion    = openView.version,
                .originDocIndex   = docIdx,
                .originCharStart  = selStart,
                .originCharEnd    = selEnd,
                .originScreenPos  = glm::vec2(screenX, screenY),
                .originKind       = xanadu::PouchOriginKind::Document,
                .originCell       = 0,
                .originSliceIndex = 0,
                .originRankCoord  = {},
            };

            if (altHeld) {
              kineticTetherEngine.startDrag(std::move(payload), screenX,
                                            screenY);
              dragStarted = true;
              return;
            }
          }
        }
      }

      if (altHeld && zigzagPresentation) {
        const auto screenX = static_cast<float>(mx);
        const auto screenY = static_cast<float>(state->view.screenHeight - my);
        std::optional<zigzag::CellRef> cellTarget;
        if (renderer->lastPick && renderer->lastPick->semanticTarget &&
            renderer->lastPick->semanticTarget->cellRef) {
          cellTarget = static_cast<zigzag::CellRef>(
              *renderer->lastPick->semanticTarget->cellRef);
        } else {
          cellTarget = zigzagPresentation->focusCell();
        }

        if (cellTarget && !zigzag::isEphemeral(*cellTarget)) {
          const auto cellRef   = *cellTarget;
          const auto &manifold = zigzagPresentation->manifold();
          const auto spans     = manifold.contentOf(cellRef);
          if (!spans.empty()) {
            const auto &bridgeStore     = session->store(0);
            const auto preview          = manifold.textOf(cellRef, bridgeStore);
            const std::string rankCoord = "d.1: #" + std::to_string(cellRef);
            xudu::TetherPayload payload{
                .span            = spans.front(),
                .previewText     = preview,
                .originVersion   = bridgeStore.primaryCurrentVersion(),
                .originDocIndex  = 0,
                .originCharStart = 0,
                .originCharEnd =
                    static_cast<std::uint32_t>(spans.front().length),
                .originScreenPos  = glm::vec2(screenX, screenY),
                .originKind       = xanadu::PouchOriginKind::ZigzagCell,
                .originCell       = cellRef,
                .originSliceIndex = 0,
                .originRankCoord  = rankCoord,
            };

            kineticTetherEngine.startDrag(std::move(payload), screenX, screenY);
            dragStarted = true;
            return;
          }
        }
      }
    });

    return dragStarted;
  };

  state->mouseUpHandler = [&pouchDrawer, &kineticTetherEngine, &session,
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
            pouchDrawer.handleCellDrop(payload.span, payload.previewText,
                                       payload.originCell,
                                       payload.originRankCoord, screenX,
                                       screenY, payload.originSliceIndex);
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
      kineticTetherEngine.endDrag(screenX, screenY);
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
        preview = text.substr(selStart, std::min(selEnd - selStart, 40U));
      }
      pouchDrawer.handleGhostDrop(spans.front(), preview, openView.version,
                                  screenX, screenY, docIdx, selStart, selEnd);
    });
    return true;
  };

  // Open initial documents
  if (opts.askedVersion.empty() && opts.read.empty() &&
      opts.alongside.empty() && extraImports.empty()) {
    const auto &primaryStore = session->store(0);
    if (opts.viewMode == ViewMode::XanadocOnly) {
      const auto allVers = primaryStore.allVersions();
      if (allVers.size() > 1) {
        for (const auto &allVer : allVers) {
          views.showAlongside(allVer, 0.0F, 0);
        }
      } else {
        views.showAlongside(opening, 0.0F, 0);
      }
    } else {
      views.showAlongside(primaryStore.primaryCurrentVersion(), 0.0F, 0);
    }
  } else {
    views.showAlongside(opening, 0.0F, 0);
  }

  for (const auto &[extraVer, sIdx] : extraImports) {
    views.showAlongside(extraVer, 0.0F, sIdx);
  }
  if (!opts.alongside.empty()) {
    views.showAlongside(xanadu::MicroversionId::parse(opts.alongside), 0.0F, 0);
  }
  for (const auto &also : opts.read) {
    if (also != opening) {
      views.showAlongside(also, 0.0F, 0);
    }
  }
  for (const auto &behind : opts.background) {
    views.showAlongside(behind, kBackgroundDepthZ, 0);
  }
  if (opts.onionSkin) {
    views.setOnionSkin(true);
  }

  // Audio & Video Widgets
  std::vector<std::shared_ptr<gleditor::AudioWidget>> audioWidgets;
  for (const auto &mrl : opts.audioMrls) {
    auto w = std::make_shared<gleditor::AudioWidget>("Sans 11");
    if (mrl == "white-noise" || mrl == "test") {
      std::vector<std::byte> dummy(1024, std::byte{0x55});
      auto stream =
          std::make_shared<gleditor::MemoryMediaStream>(std::move(dummy));
      w->load(
          gleditor::MediaResource::fromStream(stream, "White Noise (48 kHz)"));
    } else {
      w->load(gleditor::MediaResource::fromFile(mrl));
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
    auto w = std::make_shared<gleditor::MediaWidget>("Sans 11");
    if (mrl == "test" || mrl == "pattern") {
      std::vector<std::byte> dummy(2048, std::byte{0xAA});
      auto stream =
          std::make_shared<gleditor::MemoryMediaStream>(std::move(dummy));
      w->load(
          gleditor::MediaResource::fromStream(stream, "Sample Video (1080p)"));
    } else {
      w->load(gleditor::MediaResource::fromFile(mrl));
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

  app.commands().registerAction(std::string(xanadu::settings::kKeymapMap),
                                "show or hide the hypertime map",
                                [&map] { map.toggle(); });
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

  const auto togglePouchAction = [&pouchDrawer] { pouchDrawer.toggle(); };
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPouchToggle), "toggle pouch drawer",
      togglePouchAction);
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPouchToggleF2),
      "toggle pouch drawer (F2)", togglePouchAction);

  const auto toggleTelescopeAction = [&swarmTelescope] {
    swarmTelescope.toggle();
  };
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapTelescopeToggle),
      "toggle swarm telescope", toggleTelescopeAction);
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapTelescopeToggleF3),
      "toggle swarm telescope (F3)", toggleTelescopeAction);

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
      "toggle 3-way tension spring layout physics",
      [&links] { links.togglePhysics(); });

  const auto unlockTranscopyrightAction = [&views] {
    views.unlockTranscopyrightAtCaret();
  };
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapUnlockTranscopyright),
      "unlock transcopyright span under caret", unlockTranscopyrightAction);
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapUnlockTranscopyrightF5),
      "unlock transcopyright span under caret (F5)",
      unlockTranscopyrightAction);
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapUnlockTranscopyrightCtrlU),
      "unlock transcopyright span under caret (Ctrl+U)",
      unlockTranscopyrightAction);

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapScrubForward),
      "step forward in microversion history",
      [&views] { views.scrubHistory(false); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapScrubBackward),
      "step backward in microversion history",
      [&views] { views.scrubHistory(true); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapScrubBack),
                                "step backward in microversion history",
                                [&views] { views.scrubHistory(true); });

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapTransclude),
      "transclude selection into new version",
      [&views] { views.transcludeSelection(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapXanalink),
                                "connect selection with an xanalink",
                                [&views] { views.linkSelection(); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapCancelLink),
      "drop link begun earlier", [&views] { views.cancelLink(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapBeams),
                                "show or hide the ribbons connecting documents",
                                [&links] { links.toggle(); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapSworph),
      "bring far end of link into view on approach",
      [&links] { links.setSworph(!links.sworphing()); });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapPublish),
      "sign and seal opening document", [&views, &opts] {
        views.publishCurrent(opts.publishAs.empty() ? std::string{"document"}
                                                    : opts.publishAs);
      });
  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapHistory),
      "list all microversions and operations to stdout",
      [&views] { views.printHistory(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapDelete),
                                "delete selected range in active document",
                                [&views] { views.deleteSelection(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapPageBreak),
                                "insert a page break at the caret",
                                [&views] { views.insertPageBreakAtCaret(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapOnionSkin),
                                "toggle 3D onion skin visualization",
                                [&views] { views.toggleOnionSkin(); });

  app.commands().registerAction(std::string(xanadu::settings::kKeymapBack),
                                "navigate to parent microversion",
                                [&views] { views.back(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapForward),
                                "navigate to child microversion",
                                [&views] { views.forward(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapNewDoc),
                                "create new sovereign document",
                                [&views] { views.newDocument(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapOpenDoc),
                                "open document or system xanadoc palette",
                                [&views] { views.openDocumentPalette(); });
  app.commands().registerAction(std::string(xanadu::settings::kKeymapCloseDoc),
                                "close active document view",
                                [&views] { views.closeActive(); });

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

  const auto dropSelectionToBench = [&views, &pouchDrawer,
                                     &session](const bool isLeft) {
    views.withCaret(
        [&pouchDrawer, &session,
         isLeft](RenderState &, const xudu::Views::Where &where, Caret *) {
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
          xudu::PouchItem item;
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
                     zoneId](RenderState &, const xudu::Views::Where &where,
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

  app.commands().registerAction(
      "forge-clasp", "forge bilateral clasp link from items on bench",
      [&pouchDrawer, &session, renderer] {
        renderer->runWithState([&pouchDrawer, &session,
                                renderer](RenderState &) {
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
      });

  app.commands().registerAction(
      "clear-bench", "clear items from clasp forge bench", [&pouchDrawer] {
        pouchDrawer.forge().clearLeft();
        pouchDrawer.forge().clearRight();
        std::cout << "xuzz: cleared clasp forge bench\n";
      });

  // B. Complete Zigzag Actions (Phase 3: all 28 actions bound!)
  const auto registerZigzagAction = [&](std::string_view name1,
                                        std::string_view name2,
                                        const char *desc, auto handler) {
    app.commands().registerAction(std::string(name1), desc, handler);
    if (!name2.empty() && name2 != name1) {
      app.commands().registerAction(std::string(name2), desc, handler);
    }
  };

  // View Modes & Bundles
  registerZigzagAction(xanadu::settings::kKeymapZigzagViewModeContent,
                       xanadu::settings::kKeymapViewModeContentV,
                       "switch Zigzag to Cell Content View",
                       [zigzagPresentation] {
                         zigzagPresentation->setViewMode(
                             zigzag::ZigzagVisualizer::ViewMode::CellContent);
                       });
  registerZigzagAction(xanadu::settings::kKeymapViewModeContent1, "",
                       "switch Zigzag to Cell Content View",
                       [zigzagPresentation] {
                         zigzagPresentation->setViewMode(
                             zigzag::ZigzagVisualizer::ViewMode::CellContent);
                       });
  registerZigzagAction(xanadu::settings::kKeymapZigzagViewModeTopology,
                       xanadu::settings::kKeymapViewModeTopologyT,
                       "switch Zigzag to Topology View", [zigzagPresentation] {
                         zigzagPresentation->setViewMode(
                             zigzag::ZigzagVisualizer::ViewMode::Topology);
                       });
  registerZigzagAction(xanadu::settings::kKeymapViewModeTopology, "",
                       "switch Zigzag to Topology View", [zigzagPresentation] {
                         zigzagPresentation->setViewMode(
                             zigzag::ZigzagVisualizer::ViewMode::Topology);
                       });

  registerZigzagAction(
      xanadu::settings::kKeymapZigzagBundleExecution,
      xanadu::settings::kKeymapBundleExecution,
      "switch to Execution dimension bundle (d.spin, d.step, d.branch)",
      [zigzagPresentation] {
        zigzagPresentation->setDimensionBundle(
            zigzag::ZigzagVisualizer::DimensionBundle::Execution);
      });
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagBundleScope,
      xanadu::settings::kKeymapBundleScope,
      "switch to Scope dimension bundle (d.lexical, d.dynamic, d.env)",
      [zigzagPresentation] {
        zigzagPresentation->setDimensionBundle(
            zigzag::ZigzagVisualizer::DimensionBundle::Scope);
      });
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagBundleContract,
      xanadu::settings::kKeymapBundleContract,
      "switch to Contract dimension bundle (d.require, d.ensure, d.invariant)",
      [zigzagPresentation] {
        zigzagPresentation->setDimensionBundle(
            zigzag::ZigzagVisualizer::DimensionBundle::Contract);
      });
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagBundleLogic,
      xanadu::settings::kKeymapBundleLogic,
      "switch to Logic dimension bundle (d.clause, d.predicate, d.var)",
      [zigzagPresentation] {
        zigzagPresentation->setDimensionBundle(
            zigzag::ZigzagVisualizer::DimensionBundle::Logic);
      });
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagBundleStdlib,
      xanadu::settings::kKeymapBundleStdlib,
      "switch to Stdlib dimension bundle (d.stdlib, d.symbol, d.version)",
      [zigzagPresentation] {
        zigzagPresentation->setDimensionBundle(
            zigzag::ZigzagVisualizer::DimensionBundle::Stdlib);
      });
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagBundleCycle,
      xanadu::settings::kKeymapBundleCycle,
      "cycle active dimension bundle forward",
      [zigzagPresentation] { zigzagPresentation->cycleDimensionBundle(true); });

  // Palette & Omnibar
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagTogglePalette,
      xanadu::settings::kKeymapTogglePalette,
      "toggle Vortex opcode and library palette HUD",
      [zigzagPresentation] { zigzagPresentation->togglePalette(); });
  registerZigzagAction(xanadu::settings::kKeymapZigzagVqlTranslateAttach,
                       xanadu::settings::kKeymapVqlTranslateAttach,
                       "translate VQL filter text and attach to active chain",
                       [zigzagPresentation] {
                         if (zigzagPresentation->isPaletteVisible()) {
                           zigzagPresentation->paletteTranslateVQL();
                           zigzagPresentation->setPaletteVisible(false);
                         }
                       });
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagToggleCommandBar,
      xanadu::settings::kKeymapToggleCommandBar,
      "toggle interactive VQL Command Omnibar",
      [zigzagPresentation] { zigzagPresentation->toggleCommandBar(); });
  registerZigzagAction(xanadu::settings::kKeymapZigzagOpenCommandBarSlash,
                       xanadu::settings::kKeymapOpenCommandBarSlash,
                       "open VQL Command Omnibar with '/' prefix",
                       [zigzagPresentation] {
                         zigzagPresentation->setCommandBarVisible(true);
                         if (zigzagPresentation->commandBarText().empty()) {
                           zigzagPresentation->commandBarInputChar('/');
                         }
                       });
  registerZigzagAction(xanadu::settings::kKeymapZigzagOpenCommandBarColon,
                       xanadu::settings::kKeymapOpenCommandBarColon,
                       "open VQL Command Omnibar with ':' prefix",
                       [zigzagPresentation] {
                         zigzagPresentation->setCommandBarVisible(true);
                         if (zigzagPresentation->commandBarText().empty()) {
                           zigzagPresentation->commandBarInputChar(':');
                         }
                       });

  app.commands().registerAction(
      std::string(xanadu::settings::kKeymapConfirmAction),
      "activate focused Zigzag cell or execute Omnibar / Palette",
      [zigzagPresentation, &bridgeCoordinator] {
        if (zigzagPresentation->isCommandBarVisible()) {
          zigzagPresentation->executeCommandBar();
        } else if (zigzagPresentation->isPaletteVisible()) {
          zigzagPresentation->paletteCloneSelectedToFocus();
          zigzagPresentation->setPaletteVisible(false);
        } else {
          bridgeCoordinator.activateCell(zigzagPresentation->focusCell());
        }
      });

  registerZigzagAction(xanadu::settings::kKeymapDismissOverlay, "",
                       "dismiss Command Omnibar or palette HUD",
                       [zigzagPresentation] {
                         if (zigzagPresentation->isCommandBarVisible()) {
                           zigzagPresentation->setCommandBarVisible(false);
                         } else if (zigzagPresentation->isPaletteVisible()) {
                           zigzagPresentation->setPaletteVisible(false);
                         }
                       });

  // Navigation
  registerZigzagAction(xanadu::settings::kKeymapZigzagStepXPos,
                       xanadu::settings::kKeymapStepXPos,
                       "step focus positive along X dimension",
                       [zigzagPresentation] {
                         zigzagPresentation->dispatchAction("step-x-pos");
                       });
  registerZigzagAction(xanadu::settings::kKeymapZigzagStepXNeg,
                       xanadu::settings::kKeymapStepXNeg,
                       "step focus negative along X dimension",
                       [zigzagPresentation] {
                         zigzagPresentation->dispatchAction("step-x-neg");
                       });
  registerZigzagAction(xanadu::settings::kKeymapZigzagStepYPos,
                       xanadu::settings::kKeymapStepYPos,
                       "step focus positive along Y dimension",
                       [zigzagPresentation] {
                         if (zigzagPresentation->isPaletteVisible()) {
                           zigzagPresentation->palettePrev();
                         } else {
                           zigzagPresentation->dispatchAction("step-y-pos");
                         }
                       });
  registerZigzagAction(xanadu::settings::kKeymapZigzagStepYNeg,
                       xanadu::settings::kKeymapStepYNeg,
                       "step focus negative along Y dimension",
                       [zigzagPresentation] {
                         if (zigzagPresentation->isPaletteVisible()) {
                           zigzagPresentation->paletteNext();
                         } else {
                           zigzagPresentation->dispatchAction("step-y-neg");
                         }
                       });
  registerZigzagAction(xanadu::settings::kKeymapZigzagStepZPos,
                       xanadu::settings::kKeymapStepZPos,
                       "step focus positive along Z dimension",
                       [zigzagPresentation] {
                         zigzagPresentation->dispatchAction("step-z-pos");
                       });
  registerZigzagAction(xanadu::settings::kKeymapZigzagStepZNeg,
                       xanadu::settings::kKeymapStepZNeg,
                       "step focus negative along Z dimension",
                       [zigzagPresentation] {
                         zigzagPresentation->dispatchAction("step-z-neg");
                       });

  // Axes & Jumping
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagSwapXY, xanadu::settings::kKeymapSwapXY,
      "swap X and Y dimension bindings",
      [zigzagPresentation] { zigzagPresentation->dispatchAction("swap-xy"); });
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagCycleDimsForward,
      xanadu::settings::kKeymapCycleDimsForward,
      "cycle active dimension bindings forward", [zigzagPresentation] {
        zigzagPresentation->dispatchAction("cycle-dims-forward");
      });
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagCycleDimsBackward,
      xanadu::settings::kKeymapCycleDimsBackward,
      "cycle active dimension bindings backward", [zigzagPresentation] {
        zigzagPresentation->dispatchAction("cycle-dims-backward");
      });

  registerZigzagAction(xanadu::settings::kKeymapZigzagJumpHome,
                       xanadu::settings::kKeymapJumpHome,
                       "jump focus to home cell", [zigzagPresentation] {
                         zigzagPresentation->dispatchAction("jump-home");
                       });
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagHopHead, xanadu::settings::kKeymapHopHead,
      "hop to head of current rank along X dimension",
      [zigzagPresentation] { zigzagPresentation->dispatchAction("hop-head"); });
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagHopTail, xanadu::settings::kKeymapHopTail,
      "hop to tail of current rank along X dimension",
      [zigzagPresentation] { zigzagPresentation->dispatchAction("hop-tail"); });

  // Cells & Persistence
  registerZigzagAction(
      xanadu::settings::kKeymapZigzagDuplicateCell,
      xanadu::settings::kKeymapDuplicateFocusCell,
      "duplicate focused cell along d.clone", [zigzagPresentation] {
        zigzagPresentation->dispatchAction("duplicate-focus-cell");
      });
  registerZigzagAction(xanadu::settings::kKeymapZigzagSaveStore,
                       xanadu::settings::kKeymapSaveStore,
                       "save current Zigzag slice to sovereign store",
                       [zigzagPresentation] {
                         if (zigzagPresentation->saveStore("")) {
                           std::cout << "Successfully saved ZigZag store.\n";
                         }
                       });

  registerZigzagAction(
      xanadu::settings::kKeymapInsertCellXPos, "",
      "insert connected cell positive along X dimension", [zigzagPresentation] {
        zigzagPresentation->dispatchAction("insert-cell-x-pos");
      });
  registerZigzagAction(
      xanadu::settings::kKeymapInsertCellXNeg, "",
      "insert connected cell negative along X dimension", [zigzagPresentation] {
        zigzagPresentation->dispatchAction("insert-cell-x-neg");
      });
  registerZigzagAction(
      xanadu::settings::kKeymapInsertCellYPos, "",
      "insert connected cell positive along Y dimension", [zigzagPresentation] {
        zigzagPresentation->dispatchAction("insert-cell-y-pos");
      });
  registerZigzagAction(
      xanadu::settings::kKeymapInsertCellYNeg, "",
      "insert connected cell negative along Y dimension", [zigzagPresentation] {
        zigzagPresentation->dispatchAction("insert-cell-y-neg");
      });
  registerZigzagAction(xanadu::settings::kKeymapUnlinkXPos, "",
                       "unlink focused cell along positive X dimension",
                       [zigzagPresentation] {
                         zigzagPresentation->dispatchAction("unlink-x-pos");
                       });
  registerZigzagAction(xanadu::settings::kKeymapUnlinkXNeg, "",
                       "unlink focused cell along negative X dimension",
                       [zigzagPresentation] {
                         zigzagPresentation->dispatchAction("unlink-x-neg");
                       });
  registerZigzagAction(xanadu::settings::kKeymapDeleteFocusCell,
                       xanadu::settings::kKeymapDeleteFocusCellBksp,
                       "delete currently focused cell", [zigzagPresentation] {
                         zigzagPresentation->dispatchAction(
                             "delete-focus-cell");
                       });

  registerZigzagAction(
      xanadu::settings::kKeymapRasterizePrint, "",
      "print 2D raster reading text to stdout", [zigzagPresentation] {
        const auto res = zigzagPresentation->rasterize();
        std::cout << "\n=== ZigZag 2D Raster Reading Stream ===\n"
                  << res.text << "\n=======================================\n";
      });
  registerZigzagAction(
      xanadu::settings::kKeymapExportLinkPackage, "",
      "export current slice as Xudu LinkPackage", [zigzagPresentation] {
        xanadu::MutableKeys keys{};
        const auto pkg = zigzagPresentation->exportAsLinkPackage(keys);
        std::cout << "Exported Xudu LinkPackage: " << pkg.describe() << " ("
                  << pkg.links.size() << " links)\n";
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

  // 8. System Store Change Watcher Callback
  session->setSystemDocChangedCallback(
      [&app, radialMenu, docSwitcher, &pouchDrawer, &links, &map,
       zigzagPresentation, &bridgeCoordinator](const xudu::SystemDocKind kind,
                                               const xudu::Store &store) {
        std::cout << "xuzz: system doc updated (" << xudu::systemDocUri(kind)
                  << ")\n";
        const auto model = xudu::SystemStoreModel::fromStore(store);
        if (!model.isValid()) {
          std::cerr << "xuzz: rejecting invalid system store "
                    << xudu::systemDocUri(kind) << ": "
                    << model.validationError() << "\n";
          return;
        }
        switch (kind) {
        case xudu::SystemDocKind::Keymap: {
          const auto kmCfg = xudu::KeymapConfig::fromStore(store);
          for (const auto &[act, comboStr] : kmCfg.bindings) {
            if (const auto combo = gleditor::parseKeyCombo(comboStr)) {
              app.commands().rebind(act, combo->first, combo->second);
            }
          }
          if (auto vHost = zigzagPresentation->vortexHost()) {
            vHost->loadMacrosFromStore(store);
          }
          break;
        }
        case xudu::SystemDocKind::Settings: {
          if (auto vHost = zigzagPresentation->vortexHost()) {
            vHost->loadConfigFromStore(store);
          }
          break;
        }
        case xudu::SystemDocKind::Layout: {
          const auto layout = xudu::LayoutConfig::fromStore(store);
          links.setVisible(layout.xanalinkRibbons);
          links.setBeamConfig(layout.beams);
          links.tensionEngine().setParams(layout.physics.toTensionParams());
          pouchDrawer.setDockSide(layout.pouchDock == xudu::PouchDock::Left
                                      ? xudu::PouchDrawer::DockSide::Left
                                      : xudu::PouchDrawer::DockSide::Right);
          zigzagPresentation->setPresentationConfig(layout.zigzag);
          bridgeCoordinator.applyConfig(layout.bridge);
          break;
        }
        case xudu::SystemDocKind::UI: {
          const auto uiCfg = xudu::UIConfig::fromStore(store);
          radialMenu->setConfig(uiCfg.radialMenu);
          docSwitcher->setVisible(uiCfg.tabBarVisible);
          map.setVisible(uiCfg.hypertimeMapVisible);
          break;
        }
        case xudu::SystemDocKind::Pouches:
        case xudu::SystemDocKind::Count:
          break;
        }
      });

  // Apply active system doc configurations at launch
  {
    const auto kmIdx = session->systemStoreIndex(xudu::SystemDocKind::Keymap);
    const auto &kmStore = session->store(kmIdx);
    if (kmStore.opCount() > 0) {
      const auto kmCfg = xudu::KeymapConfig::fromStore(kmStore);
      for (const auto &[act, comboStr] : kmCfg.bindings) {
        if (const auto combo = gleditor::parseKeyCombo(comboStr)) {
          app.commands().rebind(act, combo->first, combo->second);
        }
      }
    }
    const auto uiIdx    = session->systemStoreIndex(xudu::SystemDocKind::UI);
    const auto &uiStore = session->store(uiIdx);
    if (uiStore.opCount() > 0) {
      const auto uiCfg = xudu::UIConfig::fromStore(uiStore);
      radialMenu->setConfig(uiCfg.radialMenu);
      docSwitcher->setVisible(uiCfg.tabBarVisible);
      map.setVisible(uiCfg.hypertimeMapVisible);
    }
    const auto loIdx = session->systemStoreIndex(xudu::SystemDocKind::Layout);
    const auto &loStore = session->store(loIdx);
    if (loStore.opCount() > 0) {
      const auto loCfg = xudu::LayoutConfig::fromStore(loStore);
      links.setVisible(loCfg.xanalinkRibbons);
      links.setBeamConfig(loCfg.beams);
      links.tensionEngine().setParams(loCfg.physics.toTensionParams());
      pouchDrawer.setDockSide(loCfg.pouchDock == xudu::PouchDock::Left
                                  ? xudu::PouchDrawer::DockSide::Left
                                  : xudu::PouchDrawer::DockSide::Right);
      zigzagPresentation->setPresentationConfig(loCfg.zigzag);
      bridgeCoordinator.applyConfig(loCfg.bridge);
    }
  }

  if (!opts.quiet) {
    std::cout << "xuzz commands:\n" << app.commands().helpText();
  }

  // 9. Run application loop
  const auto status = app.run();

  // 10. Persistence & Exit Cleanup
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
