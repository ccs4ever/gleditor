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
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "config.h" // for GLEDITOR_VERSION, TOSTRING
#include <argparse/argparse.hpp>

#include <gleditor/app.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/form.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/state.hpp>
#include <gleditor/text_source.hpp>

#include "beams.hpp"
#include "core/config.hpp"
#include "core/microversion.hpp"
#include "core/mutable_link.hpp"
#include "core/ops.hpp"
#include "core/provenance.hpp"
#include "core/torrent.hpp"
#include "session.hpp"

namespace {

using gleditor::Mod;
using xudu::Author;
using xudu::HypertimeMap;
using xudu::LinkBeams;
using xudu::MicroversionId;
using xudu::Session;
using xudu::signingKeys;

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
  const auto record =
      fs::is_directory(given) ? given / xudu::provenanceFileName : given;
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
    // Covers both "the signature is bad" and "your keyring has never heard of
    // this key", which are different problems with the same consequence: you
    // have no more reason to believe this record than any other text.
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

  // What the record claims to cover, checked against what is actually there.
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
  Views(Session &aSession, RendererRef aRenderer, HypertimeMap &aMap,
        gleditor::Form &aForm, AppStateRef aState)
      : session(aSession), renderer(std::move(aRenderer)), map(aMap),
        form(aForm), state(std::move(aState)) {}

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
      const auto quoted = session.store().transclude(
          MicroversionId{}, 0, from, where.start, where.end - where.start);
      std::cout << "xudu: quoted [" << where.start << "," << where.end
                << ") of " << from.str() << " into " << quoted.str() << "\n";
      showAlongside(quoted);
    });
  }

  /**
   * @brief Mark one end of a link, then join it to another selection.
   *
   * Two presses rather than one, because a link has two ends and the second is
   * usually in another document -- which is the case worth being able to make
   * at all. The far end may be a document read in from somebody else's
   * publication, and this one need never be published: what a link relates is
   * content, so only an end that has to travel needs a name that travels.
   */
  void linkSelection() {
    withCaret([this](RenderState &, const Where &where, Caret *) {
      if (!where.hasRange) {
        std::cout << "xudu: select something to link first\n";
        return;
      }
      const auto version = session.versionOf(where.doc);
      // The ends are primedia addresses rather than positions in this
      // document, which is what makes the link show up on everything that
      // quotes the content and survive editing around it.
      auto spans = session.store().rebuild(version).spansFor(
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

  /// Forget a link that was begun and not finished.
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

  /**
   * @brief Ask what this publication should say, then make it.
   *
   * Asked rather than assumed, because publishing is the irreversible act in
   * this program: a document goes out under somebody's name, signed, and
   * cannot be recalled from whoever has it. Everything the record will say is
   * on screen and editable first, filled in from the configuration and the
   * store -- so the common case is still one keystroke and a look.
   *
   * The form runs on the event thread and the store is the render thread's,
   * so what is offered is gathered inside runWithState() and what comes back
   * is applied inside another.
   */
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
      auto *const caret  = renderer->editCaret();
      const auto which   = nullptr != caret && caret->active() &&
                                 caret->documentIndex() < session.views().size()
                               ? caret->documentIndex()
                               : 0U;
      const auto version = session.versionOf(which);
      const auto who     = session.author();
      const auto where   = session.publishedDir();

      using Field = gleditor::Form::Field;
      using Kind  = gleditor::Form::Kind;

      // What the keyring can sign with, offered rather than typed: a
      // fingerprint is not something anybody remembers, and a key that is not
      // there is a failure at the last step of publishing.
      Field keys{"Signing key",
                 {},
                 "no signing key in the keyring",
                 false,
                 Kind::Choice};
      for (const auto &key : signingKeys(session.settings().signing())) {
        keys.options.push_back(key.describe());
        keys.optionValues.push_back(key.fingerprint);
        // Whichever the configuration names, or failing that whichever gpg
        // would reach for on its own, is what the list starts on.
        const bool wanted = who.gpgKey.empty()
                                ? key.preferred
                                : key.fingerprint.ends_with(who.gpgKey) ||
                                      key.identity.contains(who.gpgKey);
        if (wanted) {
          keys.chosen = keys.options.size() - 1;
        }
      }
      // Nothing to sign with is the end of it: the record is signed before the
      // content is sealed, so there is no half-published state to offer. Said
      // now rather than after nine fields have been filled in.
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
      // The title defaults to the name, since a one-word name is a passable
      // title and an empty required field would stop somebody who meant to
      // accept everything.
      asked[1].value = asked[0].value;

      form.open("Publish " + version.str(),
                "Signed as an authorship record, then sealed into " + where,
                std::move(asked),
                [this, version, which](const std::vector<Field> &answers) {
                  publishAnswers(version, which, answers);
                });
    });
  }

  /// Carry out what the dialog was told. Called on the event thread by the
  /// form, so the work goes back through the queue like every other command.
  void publishAnswers(const MicroversionId &version, const std::uint32_t which,
                      const std::vector<gleditor::Form::Field> &answers) {
    Session::PublishRequest request;
    request.salt          = answers[0].answer();
    request.title         = answers[1].answer();
    request.author.name   = answers[2].answer();
    request.author.email  = answers[3].answer();
    request.author.gpgKey = answers[4].answer();
    // Used for this one signature and not kept anywhere: a passphrase written
    // down is a passphrase somebody else can read.
    request.passphrase = answers[5].answer();
    if (!answers[7].answer().empty()) {
      request.extra.emplace_back("rights", answers[7].answer());
    }
    if (!answers[8].answer().empty()) {
      request.extra.emplace_back("note", answers[8].answer());
    }

    renderer->runWithState([this, version, which, request](RenderState &) {
      try {
        const auto path = session.publishDocument(version, request);
        std::cout << "xudu: published doc " << which << " as " << path << "\n";
      } catch (const std::exception &err) {
        // Said in the platform's own dialog rather than drawn in the window.
        // The form was a question and it has been answered; this is the answer
        // going wrong, and what gpg says when it will not sign is several lines
        // of somebody else's diagnostics -- a shape a message box already
        // handles, wraps and lets somebody select from, on every platform,
        // without this program growing a scrollable text panel to hold it.
        //
        // On the terminal as well, since a scripted run has no window.
        std::cout << "xudu: cannot publish: " << err.what() << "\n";
        state->showDialog(render::DiagnosticSeverity::Error,
                          "Could not publish " + version.str(), err.what());
      }
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
  /// One end of a link that has been marked and not yet joined to another.
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
  /// Held for the one thing it offers that this class cannot do itself: a
  /// native dialog, put up by the thread that owns the window.
  AppStateRef state;
  std::optional<Pending> pending;
};

void bindCommands(gleditor::Application &app, const AppStateRef &state,
                  Views &views, HypertimeMap &map, LinkBeams &links,
                  Session &session, const std::string &publishAs) {
  app.bindDefaultViewCommands();

  // Commands take control, because a bare letter is text: this program's
  // whole point is that typing is an edit, so it must reach the document.
  app.commands().bind(SDL_SCANCODE_Q, Mod::Ctrl, "quit", "save and close",
                      [state, &session] {
                        session.save();
                        state->alive = false;
                      });
  app.commands().bind(
      SDL_SCANCODE_S, Mod::Ctrl, "save", "write the spools out", [&session] {
        session.save();
        std::cout << "xudu: saved to " << session.path() << "\n";
      });
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
      .help("directory the two spools live in; created if it is not there")
      .default_value(std::string{"xanadoc"});
  parser.add_argument("--version-id")
      .help("microversion to open, for example 2a4; the default is the most "
            "recent state in the store")
      .default_value(std::string{});
  parser.add_argument("--torrent")
      .help("a .torrent file, a magnet link naming one already given, or a "
            "BEP 46 name (magnet:?xs=urn:btpk:KEY); repeatable. The info hash "
            "is a content-derived name, so a quotation into it means the same "
            "thing to anyone who has the reference and keeps meaning it after "
            "this machine is gone. A magnet carries only that name -- the "
            "piece hashes it must be verified against are in the torrent's "
            "info dictionary, which a client normally fetches from the swarm. "
            "A btpk name carries less still: it is a public key, and what it "
            "points at is looked up in the DHT and believed only if the "
            "answer is signed by that key")
      .append();
  parser.add_argument("--dht-node")
      .help("introduce a DHT node as HOST:PORT; repeatable. A DHT is joined by "
            "knowing somebody already in it, and no public routers are "
            "contacted unless named here. Needed to resolve a btpk name")
      .append();
  parser.add_argument("--private-dht")
      .help("this DHT is one private network, so do not apply the public rule "
            "that keeps the routing table to one node per /8 -- on a LAN every "
            "node is in the same /8 and the rule leaves a DHT of one")
      .flag();
  parser.add_argument("--swarm")
      .help("fetch quoted content from BitTorrent peers rather than only from "
            "this disk. Without it a reference resolves only when this machine "
            "already holds the bytes, which is the dependency on one machine "
            "that addressing content by its hash exists to remove")
      .flag();
  parser.add_argument("--peer")
      .help("introduce a peer as HOST:PORT; repeatable. Needed only when there "
            "is no tracker or DHT to find one through")
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
  parser.add_argument("--no-beams")
      .help("do not draw the links between open documents as beams; ctrl-k "
            "toggles them. A link is a visible relation between two passages "
            "and both ends are meant to be on screen, which is what the beams "
            "are for")
      .flag();
  parser.add_argument("--no-sworph")
      .help("leave the documents where they are put. By default a link coming "
            "into view brings the document at its far end alongside the one at "
            "its near end and lines the two ends up, opening that document if "
            "nothing on screen shows it; clicking a beam still does so, since "
            "that was asked for")
      .flag();
  parser.add_argument("--alongside")
      .help("also open this microversion as a second document, for reading two "
            "states or two documents against each other; passages they share "
            "are shaded in both")
      .default_value(std::string{});
  parser.add_argument("--author-name")
      .help("who publishes, as a person. Kept in the per-user configuration "
            "file, so it is said once for every store; --author-here puts it "
            "in this store instead. Publishing writes an authorship record "
            "naming them and has GnuPG sign it before the content is sealed, "
            "so that the torrent's info hash covers the content and the claim "
            "about who wrote it together")
      .default_value(std::string{});
  parser.add_argument("--author-email")
      .help("the author's email address, recorded alongside the name")
      .default_value(std::string{});
  parser.add_argument("--gpg-key")
      .help("which OpenPGP key signs the authorship record: a fingerprint, a "
            "long key id, or an email address -- anything gpg accepts. The "
            "default is gpg's own default signing key. A key kept outside the "
            "usual keyring is named by gpg_secret_key or gpg_home in the "
            "configuration file")
      .default_value(std::string{});
  parser.add_argument("--author-here")
      .help("record the author in this store rather than in the per-user "
            "configuration: a pen name, or an identity used for one project")
      .flag();
  parser.add_argument("--show-config")
      .help("print where the configuration file is and what it says, and stop")
      .flag();
  parser.add_argument("--check-authorship")
      .help("check the authorship record sealed with somebody's content and "
            "print who signed it. Give the directory the torrent's files are "
            "in, or the record itself; the signature beside it is what gpg is "
            "asked about. Says both whether the signature is good and whether "
            "the key is one you have any reason to trust, because those are "
            "different questions")
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
  std::string publishAs;
  std::vector<MicroversionId> read;
  try {
    parser.parse_args(argc, argv);
    if (parser["--show-config"] == true) {
      std::cout << "# " << xudu::configPath() << "\n"
                << xudu::loadConfig().toYaml();
      return 0;
    }
    // Answered before a window is opened or a store is touched: this asks
    // about a file somebody was sent, and needs neither.
    if (const auto where = parser.get<std::string>("--check-authorship");
        !where.empty()) {
      return checkAuthorship(where);
    }
    backend  = gleditor::applyCommonArguments(parser, state, argc, argv);
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

    if (parser["--swarm"] == true) {
      session->useSwarm(parser["--private-dht"] == true);
      quiet || std::cout << "xudu: swarm listening on port "
                         << session->swarmPort() << "\n";
    }

    // Before any torrent, since resolving a name needs a DHT to ask.
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

    // Torrents first, so a --quote has something to name.
    std::vector<xudu::InfoHash> available;
    if (parser.present<std::vector<std::string>>("--torrent")) {
      const auto root = parser.get<std::string>("--torrent-data");
      for (const auto &file :
           parser.get<std::vector<std::string>>("--torrent")) {
        // A name is tried first: both spellings begin "magnet:?", and the one
        // that has to be looked up is the one carrying xs=urn:btpk.
        const auto hash = xudu::MutableLink::looksLikeMutableLink(file)
                              ? session->addName(file)
                          : xudu::MagnetLink::looksLikeMagnet(file)
                              ? session->addMagnet(file)
                              : session->addTorrent(file, root);
        available.push_back(hash);
        // A magnet, and so a name, joins a swarm knowing only which content is
        // meant: the file list and the piece hashes come from a peer
        // afterwards. Until they do there is nothing to describe, which is a
        // stage to report rather than a failure -- the wait happens where the
        // metadata is first needed, by which time a --peer has been given.
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
      // Nothing can be quoted out of content that cannot be described, since
      // the piece hashes are what a quotation is checked against. This is the
      // first point where that is true, and peers have been introduced by now.
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

    // Who publishes here, when it was given. Before anything is published,
    // and kept, so that it is said once rather than every time.
    if (const auto name = parser.get<std::string>("--author-name"),
        email           = parser.get<std::string>("--author-email"),
        key             = parser.get<std::string>("--gpg-key");
        !name.empty() || !email.empty() || !key.empty()) {
      xudu::Author who{name, email, key};
      if (!who.named() && parser["--author-here"] != true) {
        // Half of one is accepted when it is being added to what is already
        // there -- somebody changing only their key -- and refused when it is
        // all there is, since a record with half a person in it names nobody.
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
        // Merged into whatever is already there, so setting only the key does
        // not blank the name somebody set last week.
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
      // What it now says, rather than what was passed: somebody setting only
      // their key wants to see the name it was added to.
      quiet || std::cout << "xudu: publishing as " << recorded.name << " <"
                         << recorded.email << ">"
                         << (recorded.gpgKey.empty()
                                 ? std::string{}
                                 : ", signed by " + recorded.gpgKey)
                         << (here ? " from " + session->path()
                                  : " (kept in " + xudu::configPath() + ")")
                         << "\n";
    }

    // Before the opening version is chosen, so that reading a document in and
    // saying nothing else opens the thing that was just read rather than
    // whatever this store happened to be showing.
    if (parser.present<std::vector<std::string>>("--read")) {
      for (const auto &file : parser.get<std::vector<std::string>>("--read")) {
        read.push_back(session->readPublication(file));
      }
      session->save();
    }

    const auto asked = parser.get<std::string>("--version-id");
    opening          = asked.empty()
                           ? (read.empty() ? session->store().latest() : read.front())
                           : MicroversionId::parse(asked);
    alongside        = parser.get<std::string>("--alongside");
    publishAs        = parser.get<std::string>("--publish");
    if (!publishAs.empty()) {
      // Done before anything is printed about it: publishing may have to mint
      // this machine's name, which says so, and a half-written line with that
      // in the middle of it is not a message anybody can read.
      const auto manifest = session->publishDocument(
          opening, Session::PublishRequest{publishAs, publishAs});
      quiet || std::cout << "xudu: published " << opening.str() << " as "
                         << manifest << "\n";
    }

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
    // The panel a publication is described in. Chrome rather than document, so
    // it keeps its own small font whatever the text is being read at.
    gleditor::Form publishForm("Sans 11");
    Views views(*session, renderer, map, publishForm, state);

    LinkBeams links(*session, renderer);
    links.setVisible(parser["--no-beams"] != true);
    links.setSworph(parser["--no-sworph"] != true);
    // A link's far end is usually in a document nobody has opened, and this is
    // how one gets opened. Queued rather than done on the spot: it is called
    // from the render thread, and opening a document is that thread's work.
    links.setOpener([&views](const MicroversionId &version) {
      views.showAlongside(version);
    });

    // All registered before the render thread starts, which is the contract:
    // the decorator is asked every frame, and the map and the beams need the
    // device the moment there is one.
    renderer->addSpanDecorator(session.get());
    renderer->addFrameContributor(&map);
    renderer->addFrameContributor(&links);
    // And the same three to the accessibility tree, in the order somebody
    // moving through the window should meet them: the links between the
    // documents, then the map of hypertime, then whatever modal is up.
    state->accessibility->addSource(&links);
    state->accessibility->addSource(&map);
    state->accessibility->addSource(&publishForm);
    state->accessibility->setToolkit("gleditor", TOSTRING(GLEDITOR_VERSION));
    // Going to a state is the program's business rather than the map's; this
    // is the same move ctrl-b and ctrl-n make.
    map.setGoer([&views](const MicroversionId &id) { views.showOnly(id); });
    // Last of the three, so the modal draws over both of them.
    renderer->addFrameContributor(&publishForm);
    // And it takes the keyboard while it is up, so that typing a title does
    // not type into the document behind it.
    state->modal = &publishForm;
    // Before the caret gets a click, so that clicking a beam follows it rather
    // than putting the caret behind it.
    renderer->addPickObserver(&links);

    views.showAlongside(opening);
    if (!alongside.empty()) {
      views.showAlongside(MicroversionId::parse(alongside));
    }
    // Anything else read in, so that a link between two published documents is
    // a link between two documents on screen.
    for (const auto &also : read) {
      if (also != opening) {
        views.showAlongside(also);
      }
    }

    gleditor::Application app(state, renderer, backend, "Xudu");
    bindCommands(app, state, views, map, links, *session,
                 publishAs.empty() ? std::string{"document"} : publishAs);
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
