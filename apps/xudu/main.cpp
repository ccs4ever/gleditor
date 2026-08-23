/**
 * @file main.cpp
 * @brief The OSMIC client: versioning, transclusion and links.
 *
 * Runs on the library and adds the things that make Xanadu different from an
 * ordinary editor: a history where nothing is deleted, connections drawn
 * between passages, and a map of hypertime.
 *
 * The window and the renderer are the library's; this file is the client
 * logic, the commands and the command-line options.
 */
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config.h" // for GLEDITOR_VERSION, TOSTRING
#include <argparse/argparse.hpp>

#include <gleditor/app.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/doc_switcher.hpp>
#include <gleditor/form.hpp>
#include <gleditor/render/diagnostics.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/state.hpp>
#include <gleditor/text_source.hpp>

#include "xudu/beams.hpp"
#include "xudu/core/provenance.hpp"
#include "xudu/core/config.hpp"
#include "xudu/core/ops.hpp"
#include "xudu/core/microversion.hpp"
#include "xudu/core/publication.hpp"
#include "xudu/core/resolver.hpp"
#include "xudu/core/store.hpp"
#include "xudu/session.hpp"

using gleditor::Mod;
using xudu::Author;
using xudu::Config;
using xudu::HypertimeMap;
using xudu::Link;
using xudu::LinkBeams;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::Provenance;
using xudu::Session;

namespace {

/**
 * @brief Z a --background document opens at. LinkBeams::align() fits the camera
 *        to the foreground row (documents with depthZ < 0 are deliberately
 *        excluded from that envelope), so the distance from camera to background
 *        is roughly (cameraDistance + |backgroundDepthZ|).
 *
 * -500 is a large enough fraction of a typical camera distance to shrink
 * a background document to roughly half its normal size on screen.
 */
constexpr float backgroundDepthZ = -500.0F;

/**
 * @brief Report who signed the authorship record at @p where.
 *
 * A directory, or the record itself; the signature is the file beside it. The
 * two questions are kept apart deliberately: gpg accepting a signature says
 * the record is unaltered since whoever holds that key signed it, and says
 * nothing at all about whether you have any reason to believe that person is
 * who the record claims. Reporting the two as one answer is how a signature
 * becomes a rubber stamp.
 */
int checkAuthorship(const std::string &where) {
  namespace fs = std::filesystem;
  const fs::path given(where);
  const auto record = fs::is_directory(given)
                          ? given / xudu::provenanceFileName
                          : given;
  const auto sig = fs::path(record.string() + ".asc");

  const auto slurp = [](const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
  };
  xudu::SignedProvenance sealed{slurp(record), slurp(sig)};
  if (sealed.yaml.empty()) {
    std::cerr << "no authorship record at " << record << "\n";
    return 1;
  }

  const auto check = xudu::verifyProvenance(sealed);
  std::cout << sealed.yaml;
  if (!check.signatureValid) {
    std::cout << "\nxudu: this record is NOT vouched for -- " << check.detail
              << "\n";
    return 1;
  }
  std::cout << "\nxudu: signed by " << check.signer << "\n"
            << "      key " << check.fingerprint << "\n"
            << "      "
            << (check.keyTrusted
                    ? "which is a key this keyring trusts"
                    : "which this keyring has no reason to trust -- the "
                      "signature is real, but that it is this person's key is "
                      "only their say-so")
            << "\n";

  if (const auto said = xudu::parseProvenance(sealed.yaml); said) {
    const auto content = record.parent_path() / xudu::sealedContentName;
    if (const auto bytes = slurp(content); !bytes.empty()) {
      const auto matches = xudu::sha256Hex(bytes) == said->contentDigest &&
                           bytes.size() == said->contentLength;
      std::cout << "      "
                << (matches ? "and it is about the content sealed with it"
                            : "BUT THE CONTENT BESIDE IT IS NOT WHAT IT "
                              "DESCRIBES")
                << "\n";
      return matches ? 0 : 1;
    }
  }
  return 0;
}

/**
 * @brief Whether --help-all appears on the command line.
 */
bool wantsEveryOption(const int argc, const char *const *const argv) {
  for (int i = 1; i < argc; i++) {
    if (nullptr != argv[i] && std::string_view{"--help-all"} == argv[i]) {
      return true;
    }
  }
  return false;
}

/**
 * @brief What is on screen, and the commands that change it.
 */
class Views {
public:
  Views(Session &aSession, RendererRef aRenderer, HypertimeMap &aMap,
        gleditor::Form &aForm, AppStateRef aState,
        std::shared_ptr<gleditor::DocumentSwitcher> aSwitcher)
      : session(aSession), renderer(std::move(aRenderer)), map(aMap),
        form(aForm), state(std::move(aState)), switcher(std::move(aSwitcher)) {}

  /// Replace everything on screen with one document showing @p version.
  void showOnly(const MicroversionId &version, const std::size_t storeIndex = 0) {
    const auto count = session.views().size();
    for (std::size_t i = 0; i < count; i++) {
      renderer->push(RenderItemCloseDoc());
    }
    renderer->runWithState([this](RenderState &) { session.clearViews(); });
    showAlongside(version, 0.0F, storeIndex);
  }

  /// Open @p version as another document beside whatever is already there.
  void showAlongside(const MicroversionId &version, const float depthZ = 0.0F,
                     const std::size_t storeIndex = 0) {
    renderer->push(
        RenderItemOpenDoc(session.sourceFor(version, storeIndex), depthZ));
    renderer->runWithState([this, version, storeIndex](RenderState &rState) {
      if (rState.docs.empty()) {
        return;
      }
      rState.docs.back()->addObserver(&session);
      session.viewOpened(version, storeIndex);
      map.setCurrent(session.views().front().version);
    });
  }

  /// Where the caret is, and what it has selected, on the render thread.
  struct Where {
    std::uint32_t doc{};
    std::uint32_t start{};
    std::uint32_t end{};
    bool hasRange{};
  };

  template <typename Fun> void withCaret(Fun fun) {
    renderer->runWithState([this, fun](RenderState &rState) {
      auto *const caret = renderer->editCaret();
      if (nullptr == caret || !caret->active() ||
          caret->documentIndex() >= rState.docs.size()) {
        return;
      }
      Where where{caret->documentIndex(), caret->byteOffset(),
                  caret->byteOffset(), caret->hasSelection()};
      if (where.hasRange) {
        where.start = caret->selectionStart();
        where.end   = caret->selectionEnd();
      }
      fun(rState, where, caret);
    });
  }

  void back() {
    renderer->runWithState([this](RenderState &) {
      if (session.views().empty()) {
        return;
      }
      const auto here = session.views().front().version;
      if (here.isZero()) {
        std::cout << "xudu: already at the null document\n";
        return;
      }
      const auto there = here.parent();
      std::cout << "xudu: " << here.str() << " -> " << there.str() << "\n";
      showOnly(there, session.views().front().storeIndex);
    });
  }

  void forward() {
    renderer->runWithState([this](RenderState &) {
      if (session.views().empty()) {
        return;
      }
      const auto sIdx     = session.views().front().storeIndex;
      const auto here     = session.views().front().version;
      const auto children = session.store(sIdx).children(here);
      if (children.empty()) {
        std::cout << "xudu: " << here.str() << " has no successor\n";
        return;
      }
      std::cout << "xudu: " << here.str() << " -> " << children.front().str();
      if (children.size() > 1) {
        std::cout << " (of " << children.size() << " futures)";
      }
      std::cout << "\n";
      showOnly(children.front(), sIdx);
    });
  }

  void deleteSelection() {
    withCaret([](RenderState &rState, const Where &where, Caret *caret) {
      if (!where.hasRange) {
        return;
      }
      rState.docs[where.doc]->erase(rState, where.start, where.end - where.start,
                                    caret);
    });
  }

  void transcludeSelection() {
    withCaret([this](RenderState &, const Where &where, Caret *) {
      if (!where.hasRange) {
        std::cout << "xudu: select something to quote first\n";
        return;
      }
      const auto from = session.versionOf(where.doc);
      const auto sIdx = session.storeIndexOf(where.doc);
      const auto quoted = session.store(sIdx).transclude(
          MicroversionId{}, 0, from, where.start, where.end - where.start);
      std::cout << "xudu: quoted [" << where.start << "," << where.end
                << ") of " << from.str() << " into " << quoted.str() << "\n";
      showAlongside(quoted, 0.0F, sIdx);
    });
  }

  void linkSelection() {
    withCaret([this](RenderState &, const Where &where, Caret *) {
      if (!where.hasRange) {
        std::cout << "xudu: select something to link first\n";
        return;
      }
      const auto version = session.versionOf(where.doc);
      const auto sIdx    = session.storeIndexOf(where.doc);
      auto spans         = session.store(sIdx).rebuild(version).spansFor(
          where.start, where.end - where.start);

      if (!pending) {
        pending = Pending{where.doc, where.start, where.end, std::move(spans)};
        std::cout << "xudu: link from doc " << where.doc << " [" << where.start
                  << "," << where.end
                  << ") -- select the other end and press ctrl-l again\n";
        return;
      }

      xudu::Link link;
      link.type        = xudu::LinkType::Comment;
      link.owner       = "you";
      link.left        = std::move(pending->spans);
      link.right       = std::move(spans);
      const auto after = session.addLink(where.doc, link);
      std::cout << "xudu: link doc " << pending->doc << " [" << pending->start
                << "," << pending->end << ") -> doc " << where.doc << " ["
                << where.start << "," << where.end << ") at " << after.str()
                << "\n";
      pending.reset();
    });
  }

  void cancelLink() {
    renderer->runWithState([this](RenderState &) {
      if (!pending) {
        std::cout << "xudu: no link waiting for its other end\n";
        return;
      }
      std::cout << "xudu: dropped the link begun at doc " << pending->doc
                << " [" << pending->start << "," << pending->end << ")\n";
      pending.reset();
    });
  }

  void publishCurrent(const std::string &salt) {
    renderer->runWithState([this, salt](RenderState &) {
      if (session.views().empty()) {
        std::cout << "xudu: nothing open to publish\n";
        state->showDialog(render::DiagnosticSeverity::Warning,
                          "Nothing to publish",
                          "No document is open. Open one, and what is under "
                          "the caret is what gets published.");
        return;
      }
      auto *const caret = renderer->editCaret();
      const auto which = nullptr != caret && caret->active() &&
                                 caret->documentIndex() < session.views().size()
                             ? caret->documentIndex()
                             : 0U;
      const auto version  = session.versionOf(which);
      const auto storeIdx = session.storeIndexOf(which);
      const auto who      = session.author();
      const auto where    = session.publishedDir(storeIdx);

      using Field = gleditor::Form::Field;
      using Kind  = gleditor::Form::Kind;

      Field keys{"Signing key",
                 {},
                 "no signing key in the keyring",
                 false,
                 Kind::Choice};
      for (const auto &key : signingKeys(session.settings().signing())) {
        keys.options.push_back(key.describe());
        keys.optionValues.push_back(key.fingerprint);
        const bool wanted = who.gpgKey.empty()
                                ? key.preferred
                                : key.fingerprint.ends_with(who.gpgKey) ||
                                      key.identity.contains(who.gpgKey);
        if (wanted) {
          keys.chosen = keys.options.size() - 1;
        }
      }
      if (keys.options.empty()) {
        std::cout << "xudu: no signing key in the keyring\n";
        state->showDialog(
            render::DiagnosticSeverity::Error, "No signing key",
            "The keyring has no secret key to sign an authorship record with, "
            "and a document is signed before it is sealed. Make one with `gpg "
            "--quick-generate-key`, or point gpg_home in " +
                xudu::configPath() + " at the keyring that holds yours.");
        return;
      }

      std::vector<Field> asked{
          Field{"Name", salt.empty() ? std::string{"document"} : salt,
                "one word; publishing again under it is a further state of "
                "this document",
                true},
          Field{"Title", {}, "what this document is called", true},
          Field{"Author", who.name, "who is publishing this", true},
          Field{"Email", who.email, "how to reach them", true},
          std::move(keys),
          Field{"Passphrase",
                {},
                "only if the agent is not holding it",
                false,
                Kind::Secret},
          [] {
            Field toggle{"", {}, {}, false, Kind::Toggle};
            toggle.revealsSecrets = true;
            return toggle;
          }(),
          Field{"Rights", {}, "how others may use this; optional", false},
          Field{"Note", {}, "anything else worth recording; optional", false},
      };
      asked[1].value = asked[0].value;

      form.open("Publish " + version.str(),
                "Signed as an authorship record, then sealed into " + where,
                std::move(asked),
                [this, version, which, storeIdx](const std::vector<Field> &answers) {
                  publishAnswers(version, which, storeIdx, answers);
                });
    });
  }

  void publishAnswers(const MicroversionId &version, const std::uint32_t which,
                      const std::size_t storeIdx,
                      const std::vector<gleditor::Form::Field> &answers) {
    Session::PublishRequest request;
    request.salt          = answers[0].answer();
    request.title         = answers[1].answer();
    request.author.name   = answers[2].answer();
    request.author.email  = answers[3].answer();
    request.author.gpgKey = answers[4].answer();
    request.passphrase    = answers[5].answer();
    if (!answers[7].answer().empty()) {
      request.extra.emplace_back("rights", answers[7].answer());
    }
    if (!answers[8].answer().empty()) {
      request.extra.emplace_back("note", answers[8].answer());
    }

    renderer->runWithState([this, version, which, storeIdx, request](RenderState &) {
      try {
        const auto path = session.publishDocument(version, request, storeIdx);
        std::cout << "xudu: published doc " << which << " as " << path << "\n";
      } catch (const std::exception &err) {
        std::cout << "xudu: cannot publish: " << err.what() << "\n";
        state->showDialog(render::DiagnosticSeverity::Error,
                          "Could not publish " + version.str(), err.what());
      }
    });
  }

  /// Save or preserve current document
  void saveCurrent() {
    renderer->runWithState([this](RenderState &) {
      if (session.views().empty()) {
        session.saveAll();
        return;
      }
      auto *const caret = renderer->editCaret();
      const auto which = nullptr != caret && caret->active() &&
                                 caret->documentIndex() < session.views().size()
                             ? caret->documentIndex()
                             : (switcher ? switcher->activeDocIndex() : 0U);
      if (which >= session.views().size()) {
        session.saveAll();
        return;
      }
      const auto storeIdx = session.storeIndexOf(which);
      if (session.isTemporaryStore(storeIdx)) {
        // Open folder dialog to preserve temporary store
        using Field = gleditor::Form::Field;
        namespace fs = std::filesystem;
        std::string curDir = fs::current_path().string();
        std::string defName = "doc_" + std::to_string(storeIdx) + ".xanadoc";

        std::vector<Field> fields{
            Field{"Folder", curDir, "directory where the xanadoc folder will live", true},
            Field{"Name", defName, "name of the xanadoc folder", true},
        };

        form.open("Preserve Temporary Xanadoc",
                  "Designate a permanent directory and name for this temporary store",
                  std::move(fields),
                  [this, storeIdx](const std::vector<Field> &answers) {
                    preserveAnswers(storeIdx, answers);
                  });
      } else {
        session.save(storeIdx);
        std::cout << "xudu: saved to " << session.path(storeIdx) << "\n";
      }
    });
  }

  void preserveAnswers(const std::size_t storeIdx,
                       const std::vector<gleditor::Form::Field> &answers) {
    namespace fs = std::filesystem;
    const std::string folder = answers[0].answer();
    const std::string name   = answers[1].answer();
    const fs::path targetDir = fs::path(folder) / name;

    renderer->runWithState([this, storeIdx, targetDir](RenderState &) {
      try {
        namespace fs = std::filesystem;
        fs::create_directories(targetDir);
        auto &st = session.store(storeIdx);
        st.save(targetDir.string());
        session.setStorePath(storeIdx, targetDir.string(), false);
        std::cout << "xudu: preserved temporary store to " << targetDir.string() << "\n";
      } catch (const std::exception &err) {
        std::cout << "xudu: cannot preserve store: " << err.what() << "\n";
        state->showDialog(render::DiagnosticSeverity::Error,
                          "Could not preserve xanadoc", err.what());
      }
    });
  }

  void closeActive() {
    renderer->runWithState([this](RenderState &rState) {
      if (rState.docs.empty()) {
        return;
      }
      auto *const caret = renderer->editCaret();
      const auto which = nullptr != caret && caret->active() &&
                                 caret->documentIndex() < rState.docs.size()
                             ? caret->documentIndex()
                             : (switcher ? switcher->activeDocIndex() : 0U);
      if (which < rState.docs.size()) {
        renderer->push(RenderItemCloseDoc(which));
      }
    });
  }

  void selectDoc(const std::uint32_t index) {
    renderer->runWithState([this, index](RenderState &rState) {
      if (index < rState.docs.size() && rState.docs[index]) {
        if (switcher) {
          switcher->setActiveDocIndex(index);
        }
        auto *const caret = renderer->editCaret();
        if (caret) {
          caret->placeAt(index, 0);
        }
      }
    });
  }

  void nextDoc() {
    renderer->runWithState([this](RenderState &rState) {
      if (rState.docs.empty()) {
        return;
      }
      const auto total = static_cast<std::uint32_t>(rState.docs.size());
      const auto cur   = switcher ? switcher->activeDocIndex() : 0U;
      const auto next  = (cur + 1U) % total;
      selectDoc(next);
    });
  }

  void prevDoc() {
    renderer->runWithState([this](RenderState &rState) {
      if (rState.docs.empty()) {
        return;
      }
      const auto total = static_cast<std::uint32_t>(rState.docs.size());
      const auto cur   = switcher ? switcher->activeDocIndex() : 0U;
      const auto prev  = (cur + total - 1U) % total;
      selectDoc(prev);
    });
  }

  void printHistory() {
    renderer->runWithState([this](RenderState &) {
      const auto here = session.views().empty()
                            ? MicroversionId{}
                            : session.views().front().version;
      const auto sIdx = session.views().empty() ? 0 : session.views().front().storeIndex;
      const auto &st  = session.store(sIdx);
      std::cout << "xudu: " << st.opCount() << " operations, "
                << st.primedia().size() << " bytes of primedia\n";
      for (const auto &id : st.allVersions()) {
        const auto *const op = st.getOp(id);
        std::cout << (id == here ? "  * " : "    ") << id.str() << "  "
                  << (nullptr == op ? "?" : xudu::opKindName(op->kind)) << "\n";
      }
    });
  }

private:
  struct Pending {
    std::uint32_t doc{};
    std::uint32_t start{};
    std::uint32_t end{};
    std::vector<xudu::PrimediaSpan> spans;
  };

  Session &session;
  RendererRef renderer;
  HypertimeMap &map;
  gleditor::Form &form;
  AppStateRef state;
  std::shared_ptr<gleditor::DocumentSwitcher> switcher;
  std::optional<Pending> pending;
};

void bindCommands(gleditor::Application &app, const AppStateRef &state,
                  Views &views, HypertimeMap &map, LinkBeams &links,
                  Session &session, const std::string &publishAs) {
  app.bindDefaultViewCommands();

  app.commands().bind(SDL_SCANCODE_Q, Mod::Ctrl, "quit", "save and close",
                      [state, &session] {
                        session.saveAll();
                        state->alive = false;
                      });
  app.commands().bind(
      SDL_SCANCODE_S, Mod::Ctrl, "save", "save or preserve active document",
      [&views] { views.saveCurrent(); });
  app.commands().bind(SDL_SCANCODE_W, Mod::Ctrl, "close",
                      "close the active document",
                      [&views] { views.closeActive(); });
  app.commands().bind(SDL_SCANCODE_TAB, Mod::Ctrl, "next-doc",
                      "switch to next document", [&views] { views.nextDoc(); });
  app.commands().bind(SDL_SCANCODE_TAB, Mod::Ctrl | Mod::Shift, "prev-doc",
                      "switch to previous document",
                      [&views] { views.prevDoc(); });

  for (int i = 1; i <= 9; ++i) {
    const auto scancode    = static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (i - 1));
    const auto targetIndex = static_cast<std::uint32_t>(i - 1);
    app.commands().bind(
        scancode, Mod::Ctrl, "doc-" + std::to_string(i),
        "switch to document " + std::to_string(i),
        [&views, targetIndex] { views.selectDoc(targetIndex); });
  }

  app.commands().bind(SDL_SCANCODE_M, Mod::Ctrl, "map",
                      "show or hide the hypertime map",
                      [&map] { map.toggle(); });
  app.commands().bind(SDL_SCANCODE_B, Mod::Ctrl, "back",
                      "go to the previous state, losing nothing",
                      [&views] { views.back(); });
  app.commands().bind(SDL_SCANCODE_N, Mod::Ctrl, "forward",
                      "go to the next state", [&views] { views.forward(); });
  app.commands().bind(SDL_SCANCODE_T, Mod::Ctrl, "transclude",
                      "quote the selection into a second document",
                      [&views] { views.transcludeSelection(); });
  app.commands().bind(SDL_SCANCODE_L, Mod::Ctrl, "link",
                      "mark one end of a link, then join it to another "
                      "selection -- in this document or any other open one",
                      [&views] { views.linkSelection(); });
  app.commands().bind(SDL_SCANCODE_L, Mod::Ctrl | Mod::Shift, "cancel link",
                      "forget a link that was begun and not finished",
                      [&views] { views.cancelLink(); });
  app.commands().bind(SDL_SCANCODE_K, Mod::Ctrl, "beams",
                      "show or hide the links between documents",
                      [&links] { links.toggle(); });
  app.commands().bind(SDL_SCANCODE_K, Mod::Ctrl | Mod::Shift, "sworph",
                      "let a link coming into view bring its far document over",
                      [&links] { links.setSworph(!links.sworphing()); });
  app.commands().bind(SDL_SCANCODE_S, Mod::Ctrl | Mod::Shift, "publish",
                      "publish the document the caret is in, so it can be read "
                      "off this machine",
                      [&views, publishAs] { views.publishCurrent(publishAs); });
  app.commands().bind(SDL_SCANCODE_P, Mod::Ctrl, "history",
                      "print every state to the terminal",
                      [&views] { views.printHistory(); });
  app.commands().bind(SDL_SCANCODE_BACKSPACE, "delete",
                      "stop pointing at the selection",
                      [&views] { views.deleteSelection(); });
}

} // namespace

int main(const int argc, char **argv) {
  gleditor::initLocale();

  const auto state    = std::make_shared<AppState>();
  const bool detailed = wantsEveryOption(argc, argv);

  argparse::ArgumentParser parser("xudu", TOSTRING(GLEDITOR_VERSION));
  gleditor::addCommonArguments(parser, detailed);
  parser.add_argument("store")
      .help("directory the primary spools live in; created if it is not there")
      .default_value(std::string{"xanadoc"});
  parser.add_argument("--version-id")
      .help("microversion to open, for example 2a4; the default is the most "
            "recent state in the store")
      .default_value(std::string{});
  parser.add_argument("--torrent")
      .help("a .torrent file, a magnet link naming one already given, or a "
            "name (magnet:?xs=urn:btpk:KEY); repeatable. Content referenced "
            "by the document is resolved from these")
      .append();
  parser.add_argument("--torrent-data")
      .help("directory where files described by --torrent are; empty means "
            "beside each .torrent file")
      .default_value(std::string{});
  parser.add_argument("--swarm")
      .help("fetch quoted content from the BitTorrent network rather than from "
            "a disk here")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--private-dht")
      .help("allow more than one DHT node on the same /8 network. Used for "
            "automated tests where several nodes run on 127.0.0.1")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--peer")
      .help("introduce a peer as HOST:PORT; repeatable. Useful when two "
            "machines are testing together and have not found each other "
            "through the DHT")
      .append();
  parser.add_argument("--dht-node")
      .help("bootstrap the DHT from a known node as HOST:PORT; repeatable")
      .append();
  parser.add_argument("--quote")
      .help("quote a byte range of a torrent-backed file, as "
            "FILE_INDEX,OFFSET,LENGTH; repeatable. Appended to the store's "
            "latest state")
      .append();
  parser.add_argument("--alongside")
      .help("a second microversion to show beside the opening one, for instance "
            "to compare two states")
      .default_value(std::string{});
  parser.add_argument("--background")
      .help("open this microversion as a background document (depthZ < 0), "
            "standing behind the foreground row and excluded from camera "
            "auto-framing; repeatable")
      .append();
  parser.add_argument("--no-beams")
      .help("do not draw the connections between documents")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--no-sworph")
      .help("do not let a link coming into view bring its far document over")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--map")
      .help("show the hypertime map on startup; ctrl-m toggles it while "
            "running")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--author-name")
      .help("name to record on publications made from this machine")
      .default_value(std::string{});
  parser.add_argument("--author-email")
      .help("email to record alongside the author name")
      .default_value(std::string{});
  parser.add_argument("--gpg-key")
      .help("fingerprint of the secret key to sign authorship records with")
      .default_value(std::string{});
  parser.add_argument("--author-here")
      .help("keep the --author-* settings in this store (author.yaml) rather "
            "than in the per-user configuration (~/.config/xudu/config.yaml)")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--show-config")
      .help("print the current configuration and quit")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--check-authorship")
      .help("verify the signature on this publication, report who signed it, "
            "and quit. Answers 'who is this from?' without importing it into "
            "a store -- the two are different questions")
      .default_value(std::string{});
  parser.add_argument("--read")
      .help("open a published document from a manifest file; repeatable. It is "
            "taken into this store, so it can then be read, quoted and linked "
            "to by documents here -- including ones that have never been "
            "published themselves. Refused if the manifest is not signed by "
            "whoever it claims to be from")
      .append();
  parser.add_argument("--publish")
      .help("publish the opening document under this name, writing a signed "
            "manifest and the torrent carrying what was typed here into "
            "<store>/published. The name is a salt under this machine's key: "
            "publishing again under the same name is a further state of the "
            "same document. Ctrl-shift-s does the same while running")
      .default_value(std::string{});
  parser.add_argument("--import")
      .help("read files into stores as initial operations; repeatable. The "
            "first file goes into the primary store (if empty), while additional "
            "files receive independent temporary stores")
      .append();
  parser.add_argument("files").help("source files to import or open").remaining();

  if (detailed) {
    std::cout << parser << "\n";
    return 0;
  }

  render::Backend backend = render::Backend::OpenGL;
  RendererRef renderer;
  std::unique_ptr<Session> session;
  bool quiet = false;
  MicroversionId opening;
  std::string alongside;
  std::string publishAs;
  std::vector<MicroversionId> read;
  std::vector<MicroversionId> background;
  std::vector<std::pair<MicroversionId, std::size_t>> extraImports;

  try {
    parser.parse_args(argc, argv);
    if (parser["--show-config"] == true) {
      std::cout << "# " << xudu::configPath() << "\n"
                << xudu::loadConfig().toYaml();
      return 0;
    }
    if (const auto where = parser.get<std::string>("--check-authorship");
        !where.empty()) {
      return checkAuthorship(where);
    }
    backend  = gleditor::applyCommonArguments(parser, state, argc, argv);
    renderer = Renderer::create(state, backend);
    quiet    = parser["--print-asset-dir"] == true;

    session = std::make_unique<Session>(parser.get<std::string>("store"));
    state->onDecoratedInsert = [&session](Doc &doc, const std::uint32_t at,
                                          const std::uint32_t length,
                                          const gleditor::DecorationMask mask) {
      session->markDecorated(doc, at, length, mask);
    };

    // Collect import files from --import and positional files
    std::vector<std::string> importFiles;
    if (parser.present<std::vector<std::string>>("--import")) {
      for (const auto &f : parser.get<std::vector<std::string>>("--import")) {
        if (!f.empty()) {
          importFiles.push_back(f);
        }
      }
    }
    if (parser.present<std::vector<std::string>>("files")) {
      for (const auto &f : parser.get<std::vector<std::string>>("files")) {
        if (!f.empty()) {
          importFiles.push_back(f);
        }
      }
    }

    if (!importFiles.empty()) {
      std::size_t startIdx = 0;
      if (0 == session->store(0).opCount()) {
        const auto &firstFile = importFiles[0];
        const gleditor::FileTextSource source(firstFile);
        const auto imported =
            session->store(0).insert(MicroversionId{}, 0, source.text());
        session->save(0);
        opening = imported;
        quiet || std::cout << "xudu: imported " << firstFile << " as "
                           << imported.str() << "\n";
        startIdx = 1;
      }
      for (std::size_t i = startIdx; i < importFiles.size(); ++i) {
        const auto &f = importFiles[i];
        const auto [sIdx, imported] = session->importFileToTemporaryStore(f);
        extraImports.emplace_back(imported, sIdx);
        quiet || std::cout << "xudu: imported " << f << " to temp store "
                           << sIdx << " as " << imported.str() << "\n";
      }
    }

    if (parser["--swarm"] == true) {
      session->useSwarm(parser["--private-dht"] == true);
      quiet || std::cout << "xudu: swarm listening on port "
                         << session->swarmPort() << "\n";
    }

    if (parser.present<std::vector<std::string>>("--dht-node")) {
      for (const auto &spec :
           parser.get<std::vector<std::string>>("--dht-node")) {
        const auto colon = spec.rfind(':');
        if (std::string::npos == colon) {
          throw std::runtime_error("--dht-node expects HOST:PORT, got: " +
                                   spec);
        }
        session->addDhtNode(
            spec.substr(0, colon),
            static_cast<std::uint16_t>(std::stoul(spec.substr(colon + 1))));
        quiet || std::cout << "xudu: joining the DHT through " << spec << "\n";
      }
    }

    std::vector<xudu::InfoHash> available;
    if (parser.present<std::vector<std::string>>("--torrent")) {
      const auto root = parser.get<std::string>("--torrent-data");
      for (const auto &file :
           parser.get<std::vector<std::string>>("--torrent")) {
        const auto hash = xudu::MutableLink::looksLikeMutableLink(file)
                              ? session->addName(file)
                          : xudu::MagnetLink::looksLikeMagnet(file)
                              ? session->addMagnet(file)
                              : session->addTorrent(file, root);
        available.push_back(hash);
        if (const auto *const meta = session->content().metainfo(hash);
            nullptr != meta) {
          quiet || std::cout << "xudu: " << file << " is " << meta->magnet()
                             << " (" << meta->files().size() << " file(s), "
                             << meta->totalLength() << " bytes)\n";
        } else {
          quiet || std::cout << "xudu: " << file << " is " << hash.hex()
                             << " (awaiting metadata)\n";
        }
      }
    }

    if (parser.present<std::vector<std::string>>("--peer")) {
      if (available.empty()) {
        throw std::runtime_error("--peer needs a --torrent to attach it to");
      }
      for (const auto &spec : parser.get<std::vector<std::string>>("--peer")) {
        const auto colon = spec.rfind(':');
        if (std::string::npos == colon) {
          throw std::runtime_error("--peer expects HOST:PORT, got: " + spec);
        }
        session->connectPeer(
            available.back(), spec.substr(0, colon),
            static_cast<std::uint16_t>(std::stoul(spec.substr(colon + 1))));
        quiet || std::cout << "xudu: asking " << spec << " for "
                           << available.back().hex() << "\n";
      }
    }

    if (parser.present<std::vector<std::string>>("--quote")) {
      if (available.empty()) {
        throw std::runtime_error("--quote needs a --torrent to quote from");
      }
      if (!session->awaitMetadata(available.back(), std::chrono::seconds{60})) {
        throw std::runtime_error(
            "--quote: no metadata arrived for " + available.back().hex() +
            ". A magnet or a name carries only which content is meant; the "
            "piece hashes come from a peer, so one has to be reachable -- try "
            "--peer HOST:PORT.");
      }
      for (const auto &spec : parser.get<std::vector<std::string>>("--quote")) {
        const auto first  = spec.find(',');
        const auto second = spec.find(',', first + 1);
        if (std::string::npos == first || std::string::npos == second) {
          throw std::runtime_error("--quote expects FILE,OFFSET,LENGTH, got: " +
                                   spec);
        }
        const auto fileIndex =
            static_cast<std::uint32_t>(std::stoul(spec.substr(0, first)));
        const auto offset =
            std::stoull(spec.substr(first + 1, second - first - 1));
        const auto length = std::stoull(spec.substr(second + 1));

        const auto at = static_cast<std::uint32_t>(
            session->store(0).rebuild(session->store(0).latest()).length());
        const auto produced =
            session->quoteTorrent(session->store(0).latest(), at,
                                  available.back(), fileIndex, offset, length);
        quiet || std::cout << "xudu: " << produced.str() << " quotes "
                           << available.back().hex() << " file " << fileIndex
                           << " [" << offset << "," << offset + length << ")\n";
      }
      session->save(0);
    }

    if (const auto name = parser.get<std::string>("--author-name"),
        email           = parser.get<std::string>("--author-email"),
        key             = parser.get<std::string>("--gpg-key");
        !name.empty() || !email.empty() || !key.empty()) {
      xudu::Author who{name, email, key};
      if (!who.named() && parser["--author-here"] != true) {
        const auto existing = xudu::loadConfig();
        if (!Author{name.empty() ? existing.author.name : name,
                    email.empty() ? existing.author.email : email, key}
                 .named()) {
          throw std::runtime_error(
              "--author-name and --author-email go together: an authorship "
              "record with only half of a person in it names nobody");
        }
      }
      const bool here = parser["--author-here"] == true;
      auto recorded   = who;
      if (here) {
        session->setAuthor(who);
      } else {
        auto config = xudu::loadConfig();
        if (!who.name.empty()) {
          config.author.name = who.name;
        }
        if (!who.email.empty()) {
          config.author.email = who.email;
        }
        if (!who.gpgKey.empty()) {
          config.author.gpgKey = who.gpgKey;
        }
        xudu::saveConfig(config);
        recorded = config.author;
      }
      quiet || std::cout << "xudu: publishing as " << recorded.name << " <"
                         << recorded.email << ">"
                         << (recorded.gpgKey.empty()
                                 ? std::string{}
                                 : ", signed by " + recorded.gpgKey)
                         << (here ? " from " + session->path(0)
                                  : " (kept in " + xudu::configPath() + ")")
                         << "\n";
    }

    if (parser.present<std::vector<std::string>>("--read")) {
      for (const auto &file : parser.get<std::vector<std::string>>("--read")) {
        read.push_back(session->readPublication(file));
      }
      session->save(0);
    }
    if (parser.present<std::vector<std::string>>("--background")) {
      for (const auto &verStr :
           parser.get<std::vector<std::string>>("--background")) {
        background.push_back(MicroversionId::parse(verStr));
      }
    }

    const auto asked = parser.get<std::string>("--version-id");
    if (opening.isZero()) {
      opening = asked.empty()
                    ? (read.empty() ? session->store(0).latest() : read.front())
                    : MicroversionId::parse(asked);
    }
    alongside = parser.get<std::string>("--alongside");
    publishAs = parser.get<std::string>("--publish");
    if (!publishAs.empty()) {
      const auto manifest = session->publishDocument(
          opening, Session::PublishRequest{publishAs, publishAs, {}, {}, {}}, 0);
      quiet || std::cout << "xudu: published " << opening.str() << " as "
                         << manifest << "\n";
    }

    quiet || std::cout << "xudu " << TOSTRING(GLEDITOR_VERSION) << ": "
                       << session->store(0).opCount() << " operations in "
                       << session->path(0) << ", opening " << opening.str()
                       << "\n";
  } catch (const std::exception &err) {
    std::cerr << err.what() << "\n" << parser;
    return 1;
  }

  try {
    HypertimeMap map("Sans 10", *session);
    map.setVisible(parser["--map"] == true);

    auto docSwitcher = std::make_shared<gleditor::DocumentSwitcher>("Sans 10");
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

    gleditor::Form publishForm("Sans 11");
    Views views(*session, renderer, map, publishForm, state, docSwitcher);

    LinkBeams links(*session, renderer);
    links.setVisible(parser["--no-beams"] != true);
    links.setSworph(parser["--no-sworph"] != true);
    links.setOpener([&views](const MicroversionId &version) {
      views.showAlongside(version);
    });

    renderer->addSpanDecorator(session.get());
    renderer->addFrameContributor(docSwitcher.get());
    renderer->addFrameContributor(&map);
    renderer->addFrameContributor(&links);

    state->accessibility->addSource(docSwitcher.get());
    state->accessibility->addSource(&links);
    state->accessibility->addSource(&map);
    state->accessibility->addSource(&publishForm);
    state->accessibility->setToolkit("gleditor", TOSTRING(GLEDITOR_VERSION));

    map.setGoer([&views](const MicroversionId &id) { views.showOnly(id); });
    renderer->addFrameContributor(&publishForm);
    state->modal = &publishForm;
    renderer->addPickObserver(docSwitcher.get());
    renderer->addPickObserver(&links);

    views.showAlongside(opening, 0.0F, 0);
    for (const auto &[extraVer, sIdx] : extraImports) {
      views.showAlongside(extraVer, 0.0F, sIdx);
    }
    if (!alongside.empty()) {
      views.showAlongside(MicroversionId::parse(alongside), 0.0F, 0);
    }
    for (const auto &also : read) {
      if (also != opening) {
        views.showAlongside(also, 0.0F, 0);
      }
    }
    for (const auto &behind : background) {
      views.showAlongside(behind, backgroundDepthZ, 0);
    }

    gleditor::Application app(state, renderer, backend, "Xudu");
    bindCommands(app, state, views, map, links, *session,
                 publishAs.empty() ? std::string{"document"} : publishAs);
    quiet || std::cout << "commands:\n" << app.commands().helpText();

    const auto status = app.run();
    session->saveAll();
    return status;
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    return 1;
  }
}
