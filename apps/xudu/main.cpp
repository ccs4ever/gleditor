/**
 * @file main.cpp
 * @brief Xudu: a xanadoc editor.
 *
 * The same library the plain editor uses, told about a different idea of what
 * a document is. Text typed here is appended to a permanent spool and the edit
 * is recorded as an operation, so every state the document has ever been in
 * stays reachable and going back to one and editing it branches rather than
 * destroys. A passage quoted from one document into another is one copy with
 * two pointers to it, which is why both show it shaded and why editing around
 * it does not break the connection.
 *
 * None of that is in the library. What the library was given is the ability to
 * take its text from something other than a file, to say what changed, to
 * colour ranges somebody else cares about, and to let a program draw. Xanadu
 * is assembled out of those here.
 */
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "config.h" // for GLEDITOR_VERSION, TOSTRING
#include <argparse/argparse.hpp>

#include <gleditor/app.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/state.hpp>
#include <gleditor/text_source.hpp>

#include "core/microversion.hpp"
#include "core/ops.hpp"
#include "core/torrent.hpp"
#include "session.hpp"

namespace {

using gleditor::Mod;
using xudu::HypertimeMap;
using xudu::MicroversionId;
using xudu::Session;

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
 *
 * Every command runs on the event thread and does its work inside
 * runWithState(), because touching a document or the caret is the render
 * thread's business. That is also what makes the ordering work: the render
 * queue is first in, first out, so a close pushed before an open happens
 * before it.
 */
class Views {
public:
  Views(Session &aSession, RendererRef aRenderer, HypertimeMap &aMap)
      : session(aSession), renderer(std::move(aRenderer)), map(aMap) {}

  /// Replace everything on screen with one document showing @p version.
  void showOnly(const MicroversionId &version) {
    const auto count = session.views().size();
    for (std::size_t i = 0; i < count; i++) {
      renderer->push(RenderItemCloseDoc());
    }
    renderer->runWithState([this](RenderState &) { session.clearViews(); });
    showAlongside(version);
  }

  /// Open @p version as another document beside whatever is already there.
  void showAlongside(const MicroversionId &version) {
    renderer->push(RenderItemOpenDoc(session.sourceFor(version)));
    renderer->runWithState([this, version](RenderState &state) {
      if (state.docs.empty()) {
        return;
      }
      // The document the queue has just opened is the last one, and it needs
      // to report its edits here: that is what turns typing into operations.
      state.docs.back()->addObserver(&session);
      session.viewOpened(version);
      // The map marks where the reader is, which is the first document --
      // the one the commands act on. A second opened beside it is context,
      // and marking that instead would point at the wrong state.
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

  /// Run @p fun with the caret's position, if it has one and it lands on an
  /// open document.
  template <typename Fun> void withCaret(Fun fun) {
    renderer->runWithState([this, fun](RenderState &state) {
      auto *const caret = renderer->editCaret();
      if (nullptr == caret || !caret->active() ||
          caret->documentIndex() >= state.docs.size()) {
        return;
      }
      Where where{caret->documentIndex(), caret->byteOffset(),
                  caret->byteOffset(), caret->hasSelection()};
      if (where.hasRange) {
        where.start = caret->selectionStart();
        where.end   = caret->selectionEnd();
      }
      fun(state, where, caret);
    });
  }

  /// Go back one state in hypertime. Nothing is undone: the state being left
  /// keeps existing, and editing from here makes a branch.
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
      showOnly(there);
    });
  }

  /// Go forward, along the first continuation this state has.
  void forward() {
    renderer->runWithState([this](RenderState &) {
      if (session.views().empty()) {
        return;
      }
      const auto here     = session.views().front().version;
      const auto children = session.store().children(here);
      if (children.empty()) {
        std::cout << "xudu: " << here.str() << " has no successor\n";
        return;
      }
      std::cout << "xudu: " << here.str() << " -> " << children.front().str();
      if (children.size() > 1) {
        std::cout << " (of " << children.size() << " futures)";
      }
      std::cout << "\n";
      showOnly(children.front());
    });
  }

  /// Stop pointing at the selection. The content stays in the spool.
  void deleteSelection() {
    withCaret([](RenderState &state, const Where &where, Caret *caret) {
      if (!where.hasRange) {
        return;
      }
      state.docs[where.doc]->erase(state, where.start, where.end - where.start,
                                   caret);
    });
  }

  /// Quote the selection into a second document beside this one.
  void transcludeSelection() {
    withCaret([this](RenderState &, const Where &where, Caret *) {
      if (!where.hasRange) {
        std::cout << "xudu: select something to quote first\n";
        return;
      }
      const auto from = session.versionOf(where.doc);
      // A virtual copy: what goes into the new document is pointers to the
      // addresses this one already uses, so there is one copy of the content
      // and two documents showing it.
      const auto quoted =
          session.store().transclude(MicroversionId{}, 0, from, where.start,
                                     where.end - where.start);
      std::cout << "xudu: quoted [" << where.start << "," << where.end
                << ") of " << from.str() << " into " << quoted.str() << "\n";
      showAlongside(quoted);
    });
  }

  /// Attach a link to the selected content.
  void linkSelection() {
    withCaret([this](RenderState &, const Where &where, Caret *) {
      if (!where.hasRange) {
        std::cout << "xudu: select something to link first\n";
        return;
      }
      const auto version = session.versionOf(where.doc);
      xudu::Link link;
      link.type  = xudu::LinkType::Comment;
      link.owner = "you";
      // The ends are primedia addresses rather than positions in this
      // document, which is what makes the link show up on everything that
      // quotes the content and survive editing around it.
      link.left = session.store().rebuild(version).spansFor(
          where.start, where.end - where.start);

      const auto after = session.store().addLink(version, link);
      std::cout << "xudu: link over [" << where.start << "," << where.end
                << ") at " << after.str() << "\n";
      showOnly(after);
    });
  }

  /// Print the whole of hypertime, which is easier to read than the map when
  /// there is a lot of it.
  void printHistory() {
    renderer->runWithState([this](RenderState &) {
      const auto here = session.views().empty()
                            ? MicroversionId{}
                            : session.views().front().version;
      std::cout << "xudu: " << session.store().opCount() << " operations, "
                << session.store().primedia().size() << " bytes of primedia\n";
      for (const auto &id : session.store().allVersions()) {
        const auto *const op = session.store().getOp(id);
        std::cout << (id == here ? "  * " : "    ") << id.str() << "  "
                  << (nullptr == op ? "?" : xudu::opKindName(op->kind)) << "\n";
      }
    });
  }

private:
  Session &session;
  RendererRef renderer;
  HypertimeMap &map;
};

void bindCommands(gleditor::Application &app, const AppStateRef &state,
                  Views &views, HypertimeMap &map, Session &session) {
  app.bindDefaultViewCommands();

  // Commands take control, because a bare letter is text: this program's
  // whole point is that typing is an edit, so it must reach the document.
  app.commands().bind(SDL_SCANCODE_Q, Mod::Ctrl, "quit", "save and close",
                      [state, &session] {
                        session.save();
                        state->alive = false;
                      });
  app.commands().bind(SDL_SCANCODE_S, Mod::Ctrl, "save",
                      "write the spools out",
                      [&session] {
                        session.save();
                        std::cout << "xudu: saved to " << session.path() << "\n";
                      });
  app.commands().bind(SDL_SCANCODE_M, Mod::Ctrl, "map",
                      "show or hide the hypertime map", [&map] { map.toggle(); });
  app.commands().bind(SDL_SCANCODE_B, Mod::Ctrl, "back",
                      "go to the previous state, losing nothing",
                      [&views] { views.back(); });
  app.commands().bind(SDL_SCANCODE_N, Mod::Ctrl, "forward",
                      "go to the next state", [&views] { views.forward(); });
  app.commands().bind(SDL_SCANCODE_T, Mod::Ctrl, "transclude",
                      "quote the selection into a second document",
                      [&views] { views.transcludeSelection(); });
  app.commands().bind(SDL_SCANCODE_L, Mod::Ctrl, "link",
                      "attach a link to the selected content",
                      [&views] { views.linkSelection(); });
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
      .help("directory the two spools live in; created if it is not there")
      .default_value(std::string{"xanadoc"});
  parser.add_argument("--version-id")
      .help("microversion to open, for example 2a4; the default is the most "
            "recent state in the store")
      .default_value(std::string{});
  parser.add_argument("--torrent")
      .help("a .torrent file, or a magnet link naming one already given; "
            "repeatable. The info hash is a content-derived name, so a "
            "quotation into it means the same thing to anyone who has the "
            "reference and keeps meaning it after this machine is gone. A "
            "magnet carries only that name -- the piece hashes it must be "
            "verified against are in the torrent's info dictionary, which a "
            "client normally fetches from the swarm")
      .append();
  parser.add_argument("--torrent-data")
      .help("directory the torrent's files are in; the default is the "
            "directory the .torrent file itself is in")
      .default_value(std::string{});
  parser.add_argument("--quote")
      .help("quote a range of the most recently given --torrent into the "
            "document, as FILE,OFFSET,LENGTH. A length of 0 means the rest of "
            "the file. Repeatable")
      .append();
  parser.add_argument("--map")
      .help("start with the hypertime map shown; ctrl-m toggles it")
      .flag();
  parser.add_argument("--alongside")
      .help("also open this microversion as a second document, for reading two "
            "states or two documents against each other; passages they share "
            "are shaded in both")
      .default_value(std::string{});
  parser.add_argument("--import")
      .help("read this file into an empty store as its first operation; a "
            "store that already has operations is left alone, since importing "
            "into one would be an edit rather than a beginning")
      .default_value(std::string{});

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
  try {
    parser.parse_args(argc, argv);
    backend  = gleditor::applyCommonArguments(parser, state);
    renderer = Renderer::create(state, backend);
    quiet    = parser["--print-asset-dir"] == true;

    session = std::make_unique<Session>(parser.get<std::string>("store"));

    // A new xanadoc is the null document, which has nothing to click on and
    // no text to quote. Importing is how one gets started, and is refused on a
    // store with a history because putting a file into that would be an
    // ordinary edit and should be made as one.
    if (const auto file = parser.get<std::string>("--import"); !file.empty()) {
      if (0 != session->store().opCount()) {
        throw std::runtime_error("--import: " + session->path() +
                                 " already has operations; import into an "
                                 "empty store");
      }
      const gleditor::FileTextSource source(file);
      const auto imported =
          session->store().insert(MicroversionId{}, 0, source.text());
      session->save();
      quiet || std::cout << "xudu: imported " << file << " as "
                         << imported.str() << "\n";
    }

    // Torrents first, so a --quote has something to name.
    std::vector<xudu::InfoHash> available;
    if (parser.present<std::vector<std::string>>("--torrent")) {
      const auto root = parser.get<std::string>("--torrent-data");
      for (const auto &file : parser.get<std::vector<std::string>>("--torrent")) {
        const auto hash = xudu::MagnetLink::looksLikeMagnet(file)
                              ? session->addMagnet(file)
                              : session->addTorrent(file, root);
        available.push_back(hash);
        const auto *const meta = session->content().metainfo(hash);
        quiet || std::cout << "xudu: " << file << " is " << meta->magnet()
                           << " (" << meta->files().size() << " file(s), "
                           << meta->totalLength() << " bytes)\n";
      }
    }

    if (parser.present<std::vector<std::string>>("--quote")) {
      if (available.empty()) {
        throw std::runtime_error("--quote needs a --torrent to quote from");
      }
      for (const auto &spec : parser.get<std::vector<std::string>>("--quote")) {
        const auto first  = spec.find(',');
        const auto second = spec.find(',', first + 1);
        if (std::string::npos == first || std::string::npos == second) {
          throw std::runtime_error("--quote expects FILE,OFFSET,LENGTH, got: " +
                                   spec);
        }
        const auto fileIndex = static_cast<std::uint32_t>(
            std::stoul(spec.substr(0, first)));
        const auto offset = std::stoull(spec.substr(first + 1, second - first - 1));
        const auto length = std::stoull(spec.substr(second + 1));

        // Appended to whatever the document is now, which for a fresh store is
        // the null document -- so a store made entirely of quotations holds no
        // content of its own at all.
        const auto at = static_cast<std::uint32_t>(
            session->store().rebuild(session->store().latest()).length());
        const auto produced =
            session->quoteTorrent(session->store().latest(), at,
                                  available.back(), fileIndex, offset, length);
        quiet || std::cout << "xudu: " << produced.str() << " quotes "
                           << available.back().hex() << " file " << fileIndex
                           << " [" << offset << "," << offset + length << ")\n";
      }
      session->save();
    }

    const auto asked = parser.get<std::string>("--version-id");
    opening = asked.empty() ? session->store().latest()
                            : MicroversionId::parse(asked);
    alongside = parser.get<std::string>("--alongside");

    quiet || std::cout << "xudu " << TOSTRING(GLEDITOR_VERSION) << ": "
                       << session->store().opCount() << " operations in "
                       << session->path() << ", opening " << opening.str()
                       << "\n";
  } catch (const std::exception &err) {
    std::cerr << err.what() << "\n" << parser;
    return 1;
  }

  try {
    // Deliberately not the document's font. The map is chrome: it has to stay
    // legible and the same size whatever the document is being read at, and a
    // 24-point label does not fit in a node box.
    HypertimeMap map("Sans 10", *session);
    map.setVisible(parser["--map"] == true);
    Views views(*session, renderer, map);

    // Both registered before the render thread starts, which is the contract:
    // the decorator is asked every frame, and the map needs the device the
    // moment there is one.
    renderer->addSpanDecorator(session.get());
    renderer->addFrameContributor(&map);

    views.showAlongside(opening);
    if (!alongside.empty()) {
      views.showAlongside(MicroversionId::parse(alongside));
    }

    gleditor::Application app(state, renderer, backend, "Xudu");
    bindCommands(app, state, views, map, *session);
    // A query prints its answer and nothing else: --print-asset-dir is read by
    // scripts, and a command listing in front of the path is not a path.
    quiet || std::cout << "commands:\n" << app.commands().helpText();

    const auto status = app.run();
    session->save();
    return status;
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    return 1;
  }
}

// vi: set sw=2 sts=2 ts=2 et:
