/**
 * @file session.hpp
 * @brief What holds a xanadoc open on screen.
 *
 * The library draws documents and reports what happens to them. The store
 * keeps content, operations and links, and knows nothing about drawing. This
 * is the piece between them, and it is deliberately the only piece that knows
 * both: everything Xanadu-shaped lives on this side of the line, and the
 * library is reached only through the hooks it publishes -- a TextSource, a
 * DocumentObserver, a SpanDecorator, a FrameContributor.
 *
 * There is nothing in the library that had to learn what a transclusion is.
 */
#ifndef XUDU_SESSION_H
#define XUDU_SESSION_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gleditor/canvas.hpp>
#include <gleditor/document_observer.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/span_decorator.hpp>
#include <gleditor/text_source.hpp>

#include "core/microversion.hpp"
#include "core/resolver.hpp"
#include "core/store.hpp"
#include "core/swarm.hpp"

class Caret;
class Doc;

namespace xudu {

class Session;

/**
 * @brief The text of one microversion, rebuilt on demand.
 *
 * OSMIC's "versioning on demand" reaching the screen: what the library is
 * handed is not a stored document but the result of replaying the operations
 * the version's name spells out. Held by shared pointer through the render
 * queue, and consulted on a loader thread, which is why it takes a copy of the
 * text rather than a reference to the store.
 */
class VersionTextSource : public gleditor::TextSource {
public:
  VersionTextSource(std::string aText, MicroversionId aVersion)
      : contents(std::move(aText)), id(std::move(aVersion)) {}

  [[nodiscard]] std::string text() const override { return contents; }
  [[nodiscard]] std::string name() const override { return id.str(); }
  [[nodiscard]] const MicroversionId &version() const { return id; }

private:
  std::string contents;
  MicroversionId id;
};

/**
 * @brief One open document: which version it shows, and where it sits.
 *
 * The library indexes open documents by position, and the picking tags carry
 * that index, so this is kept in the same order.
 */
struct OpenView {
  MicroversionId version;
  /// The version rebuilt, kept so that decorating does not replay the whole
  /// history once per frame per document.
  Version pieces;
  /// The spans this document was last decorated with, and what was true when
  /// they were worked out. Finding them compares every piece of this version
  /// against every piece of each other one, which is quadratic in the number
  /// of edits a document has had -- affordable when something changes, and not
  /// affordable sixty times a second.
  std::vector<gleditor::SpanStyle> decorations;
  std::uint64_t decoratedAt{};
};

/**
 * @class Session
 * @brief A store, the views onto it, and the hooks that keep the two in step.
 */
class Session : public gleditor::DocumentObserver,
                public gleditor::SpanDecorator {
public:
  /// Colours the decorator paints with. Backgrounds behind text, so they are
  /// pale enough to read through.
  static constexpr std::uint32_t transclusionColour = 0xFFE9A8FFU;
  static constexpr std::uint32_t linkColour         = 0xB9E8C4FFU;

  explicit Session(std::string aStorePath);

  /**
   * @brief Make a torrent's content available to this store.
   *
   * @param torrentPath A .torrent file.
   * @param dataRoot Where its files are. Empty means the directory the torrent
   *        file itself is in, which is where a downloader usually leaves them.
   * @return The info hash, which is the name the content is known by
   *         everywhere.
   * @throws std::runtime_error if the torrent cannot be read or parsed.
   */
  /**
   * @brief Fetch quoted content from peers rather than from a disk here.
   *
   * Must be called before any torrent is added, since it decides where content
   * comes from. Without it a reference resolves only if this machine already
   * holds the bytes -- which quietly reintroduces the dependency on one
   * machine that addressing content by its hash was meant to remove.
   *
   * @throws std::runtime_error if this build has no libtorrent.
   */
  /// @param privateNetwork True when every node is on one private network, so
  ///        the public-DHT rule limiting the routing table to one node per /8
  ///        would leave a DHT of one.
  void useSwarm(bool privateNetwork = false);
  [[nodiscard]] bool swarmEnabled() const { return nullptr != swarmSource; }
  /// The port the swarm listens on, or zero when there is no swarm.
  [[nodiscard]] std::uint16_t swarmPort() const;

  /**
   * @brief Introduce a peer that is known to hold some of this content.
   *
   * Ordinarily peers are found through a tracker or the DHT. Naming one is
   * useful when they are not, and is what makes a swarm of two known machines
   * possible. Does nothing without a swarm.
   */
  void connectPeer(const InfoHash &hash, const std::string &host,
                   std::uint16_t port);

  InfoHash addTorrent(const std::string &torrentPath,
                      const std::string &dataRoot);

  /**
   * @brief Name content by a magnet link.
   *
   * A magnet carries the info hash and nothing else that matters: the piece
   * hashes and the file list are in the info dictionary, which the link only
   * names. So this can say which content is meant, and can resolve it only
   * when the metadata has been obtained some other way -- a .torrent given
   * alongside, or eventually a swarm.
   *
   * @throws std::runtime_error when the link is malformed, or when its
   *         metadata is not available. Reporting that plainly beats recording
   *         a reference that would silently read as empty forever.
   */
  InfoHash addMagnet(const std::string &uri);

  /**
   * @brief Wait until @p hash can be described.
   *
   * A magnet, and therefore a name, joins a swarm knowing only which content
   * is meant; the piece hashes come from a peer afterwards. So there is a
   * window where a reference is held and nothing about it can be checked, and
   * anything needing to verify has to wait the window out.
   *
   * @return True when the metadata is there, including when it always was.
   */
  [[nodiscard]] bool awaitMetadata(const InfoHash &hash,
                                   std::chrono::milliseconds timeout);

  /**
   * @brief Tell the DHT about a node, as HOST and PORT.
   *
   * A DHT is joined by knowing somebody already in it. Does nothing without a
   * swarm.
   */
  void addDhtNode(const std::string &host, std::uint16_t port);

  /**
   * @brief Make available whatever a BEP 46 name currently points at.
   *
   * The name is an ed25519 public key rather than a hash, so it can be handed
   * out before the content it will point at exists -- which is what a
   * reference to something still being written has to be. Asking what it means
   * is a DHT lookup, and the answer is believed only if it carries the
   * publisher's signature.
   *
   * @return The info hash the name resolved to.
   * @throws std::runtime_error without a swarm, or when the name has no
   *         answer. Saying so beats recording a reference to nothing.
   */
  InfoHash addName(const std::string &uri);

  /// Where content is fetched from: a swarm when one was asked for, otherwise
  /// whatever is already on this disk.
  [[nodiscard]] const ContentSource &content() const;

  /**
   * @brief Quote a range of a torrent-backed file into @p parent.
   *
   * The document ends up pointing at content addressed by its own hash rather
   * than by an offset into this machine's spool, so the reference means the
   * same thing to anyone who has it and keeps meaning it after this machine is
   * gone.
   */
  MicroversionId quoteTorrent(const MicroversionId &parent, std::uint32_t at,
                              const InfoHash &hash, std::uint32_t fileIndex,
                              std::uint64_t offset, std::uint64_t length);

  [[nodiscard]] Store &store() { return docStore; }
  [[nodiscard]] const Store &store() const { return docStore; }

  /// Where the store is kept, and writing it there.
  [[nodiscard]] const std::string &path() const { return storePath; }
  void save() const { docStore.save(storePath); }

  /// The version each open document shows, in the library's document order.
  [[nodiscard]] const std::vector<OpenView> &views() const { return open; }
  [[nodiscard]] MicroversionId versionOf(std::uint32_t docIndex) const;

  /**
   * @brief What has changed that anything derived from the views depends on.
   *
   * Bumped when a view is opened, edited or cleared. Anything that computes
   * something across the open documents -- which passages they share, where
   * the links between them run -- compares against this rather than doing the
   * work again per frame.
   */
  [[nodiscard]] std::uint64_t generation() const { return epoch; }

  /**
   * @brief A recorded state that shows any of @p ends, most recent first.
   *
   * What "follow this link" needs when the other end of it is not on screen:
   * a link names content, not a document, so the document to open is one that
   * happens to be showing that content. There may be several -- content is
   * quoted -- and the most recent is the one most likely to be meant.
   *
   * Expensive: every state is rebuilt until one matches, and rebuilding is
   * linear in the operations behind it. Meant to be asked once when a link
   * first needs following, not per frame.
   *
   * @param except States to pass over, which is how the documents already open
   *        are excluded.
   */
  [[nodiscard]] std::optional<MicroversionId>
  versionShowing(const std::vector<PrimediaSpan> &ends,
                 const std::vector<MicroversionId> &except) const;

  /// Note that a document showing @p version has been opened. Called from the
  /// render thread as documents come and go.
  ///
  /// There is no matching "one closed": travelling to another state replaces
  /// everything on screen rather than editing what is there, so clearViews()
  /// is the only way a view goes away. A version an open document is showing
  /// changes only by being edited, which comes back through textInserted() and
  /// textErased().
  void viewOpened(const MicroversionId &version);

  /// A source for @p version, ready to hand to the render queue.
  [[nodiscard]] std::shared_ptr<VersionTextSource>
  sourceFor(const MicroversionId &version) const;

  // -- gleditor::DocumentObserver -------------------------------------------
  //
  // Every edit the library applies becomes a hyperop against the version that
  // document is showing, and the document moves to the state the op produced.
  // Typing is therefore how hypertime is extended; there is no separate
  // "commit".
  void textInserted(Doc &doc, std::uint32_t at,
                    const std::string &utf8) override;
  void textErased(Doc &doc, std::uint32_t at,
                  const std::string &removed) override;

  // -- gleditor::SpanDecorator ----------------------------------------------
  //
  // Shades the passages this document shares with another open one, and the
  // passages a link is attached to. Both are computed from primedia addresses,
  // so they are what the store knows rather than a text search.
  void decorate(const Doc &doc, std::vector<gleditor::SpanStyle> &out) override;

private:
  /// Rebuild the cached version for @p docIndex after an edit moved it.
  void refresh(std::uint32_t docIndex, const MicroversionId &version);
  /// Note that something a decoration depends on has changed.
  void invalidate() { epoch++; }

  /// Where the bytes of torrent-backed scrolls come from.
  ///
  /// Declared before the store, and therefore destroyed after it: the store
  /// keeps a bare pointer to this, and members are torn down in reverse
  /// declaration order. Nothing dereferences it during destruction today,
  /// which is exactly why the order should be right now rather than after
  /// something does.
  DirectoryContentSource contentSource;
  /// Null unless useSwarm() was called. Declared before the store for the same
  /// reason as the directory source: the store points at whichever is active.
  std::unique_ptr<SwarmContentSource> swarmSource;
  Store docStore;
  /// Bumped whenever a view or a link changes, which is what a cached set of
  /// decorations is checked against.
  std::uint64_t epoch{1};
  std::string storePath;
  std::vector<OpenView> open;

public:
  /// Forget every open view. Used when the program replaces what is on screen
  /// wholesale, which is how travelling to another state is done.
  void clearViews() {
    open.clear();
    invalidate();
  }
};

/**
 * @brief The hypertime map: every state of the document, and how they connect.
 *
 * Drawn as a frame contributor over the documents, in window pixels. Nelson's
 * prototype had one too, and it is the part of OSMIC that cannot be explained
 * without a picture: a graph where nothing is ever lost, as against the single
 * line an undo stack offers.
 */
class HypertimeMap : public gleditor::FrameContributor {
public:
  HypertimeMap(std::string aFontName, const Session &aSession);

  /// The canvas is built here rather than in the constructor, because the
  /// device does not exist until the render thread has started.
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;

  /// Whether the map is shown at all.
  void setVisible(const bool shown) { visible = shown; }
  [[nodiscard]] bool isVisible() const { return visible; }
  void toggle() { visible = !visible; }

  /// Which state to mark as where the reader is.
  void setCurrent(MicroversionId id) { current = std::move(id); }

  void drawFrame(gleditor::FrameContext &ctx) override;

private:
  /// Pixel geometry of the map. A node is a labelled box; a generation is a
  /// column, so time runs left to right and branches stack downwards.
  static constexpr float nodeWidth   = 74.0F;
  static constexpr float nodeHeight  = 26.0F;
  static constexpr float columnGap   = 34.0F;
  static constexpr float rowGap      = 10.0F;
  static constexpr float mapMargin   = 16.0F;
  static constexpr float padding     = 10.0F;

  std::string fontName;
  std::unique_ptr<gleditor::Canvas> canvas;
  const Session &session;
  MicroversionId current;
  bool visible{};
  /// What the map showed when it was last built, so that a frame in which
  /// nothing changed does not rebuild and re-upload it.
  std::size_t builtForOps{static_cast<std::size_t>(-1)};
  MicroversionId builtForCurrent;
  bool builtForVisible{};
  int builtForHeight{};
};

} // namespace xudu

#endif // XUDU_SESSION_H
// vi: set sw=2 sts=2 ts=2 et:
