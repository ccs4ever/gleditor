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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/document_observer.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/grounding_modal.hpp>
#include <gleditor/image_cache.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/span_decorator.hpp>
#include <gleditor/svg_cache.hpp>
#include <gleditor/text_source.hpp>

#include "xudu/core/config.hpp"
#include "xudu/core/media_manager.hpp"
#include "xudu/core/microversion.hpp"
#include "xudu/core/publication.hpp"
#include "xudu/core/resolver.hpp"
#include "xudu/core/store.hpp"
#include "xudu/core/swarm.hpp"
#include "xudu/core/uncommitted_op_log.hpp"

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
  VersionTextSource(std::string aText, MicroversionId aVersion,
                    std::vector<std::uint32_t> aBreaks = {})
      : contents(std::move(aText)), id(std::move(aVersion)),
        breaks(std::move(aBreaks)) {}

  [[nodiscard]] std::string text() const override { return contents; }
  [[nodiscard]] std::string name() const override { return id.str(); }
  [[nodiscard]] const MicroversionId &version() const { return id; }
  [[nodiscard]] std::vector<std::uint32_t> forcedBreaks() const override {
    return breaks;
  }

private:
  std::string contents;
  MicroversionId id;
  std::vector<std::uint32_t> breaks;
};

/**
 * @brief One open document: which version it shows, which store it belongs to,
 * and where it sits.
 *
 * The library indexes open documents by position, and the picking tags carry
 * that index, so this is kept in the same order.
 */
struct OpenView {
  MicroversionId version;
  std::size_t storeIndex{0};
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
  /// Uncommitted interactive edits waiting to be compacted and flushed to the
  /// store.
  UncommittedOpLog uncommittedLog;
};

/**
 * @class Session
 * @brief Stores, the views onto them, and the hooks that keep them in step.
 */
class Session : public gleditor::DocumentObserver,
                public gleditor::SpanDecorator {
public:
  /// Colours the decorator paints with. Backgrounds behind text, so they are
  /// pale enough to read through.
  static constexpr std::uint32_t transclusionColour = 0xFFE9A8FFU;
  static constexpr std::uint32_t linkColour         = 0xB9E8C4FFU;
  /// Withheld holes, and any hole whose reason did not reach us. The rest of
  /// the hole palette lives beside HoleReason in scroll.hpp, so that the
  /// mapping from reason to colour is testable without a graphics device --
  /// see colourForHole() there.
  static constexpr std::uint32_t redactionColour              = kWithheldColour;
  static constexpr std::uint32_t transcopyrightLockedColour   = 0xF59E0BCCU;
  static constexpr std::uint32_t transcopyrightUnlockedColour = 0x10B981AAU;

  explicit Session(std::string aStorePath,
                   std::shared_ptr<UserPermascroll> scroll = nullptr);
  ~Session() override;

  [[nodiscard]] const UserPermascroll *userPermascroll() const;
  void dumpPermascroll(const std::string &filePath) const;
  void saveOsmicTextAll() const;

  MicroversionId importBranch(std::size_t storeIndex,
                              const std::string &filePath);
  MicroversionId insertText(std::uint32_t docIndex, std::uint32_t at,
                            std::string_view newText);
  /// Like insertText(), but for a whole media file's bytes: routes through
  /// Store::insertMedia() so the range is tagged with @p mimeType and a later
  /// transclusion of a fragment of it can still be classified. See
  /// Store::insertMedia() for why insertText() itself must not be used for
  /// this -- it would coalesce with adjacent locally-typed text into one
  /// piece libmagic cannot identify.
  MicroversionId insertMedia(std::uint32_t docIndex, std::uint32_t at,
                             std::string_view bytes, std::string mimeType);
  MicroversionId insertBreak(std::uint32_t docIndex, std::uint32_t at);
  MicroversionId insertSpan(std::uint32_t docIndex, std::uint32_t at,
                            const PrimediaSpan &span);
  MicroversionId transclude(std::uint32_t destDocIndex, std::uint32_t destPos,
                            std::uint32_t srcDocIndex, std::uint32_t srcStart,
                            std::uint32_t srcLength);
  MicroversionId transcludeText(std::uint32_t destDocIndex,
                                std::uint32_t destPos,
                                std::uint32_t srcDocIndex,
                                std::string_view queryText);

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

  /**
   * @brief Read a published document and take it into this store.
   *
   * After this it is a document like any other here: it can be read, quoted,
   * and linked to by documents written on this machine that have never been
   * published themselves. Nothing about it is copied -- its pieces point at
   * the publisher's scrolls, so this store and theirs are pointing at one copy
   * of the content, which is what makes a link between them a link about the
   * same passage.
   *
   * @param path A manifest as publishDocument() writes one.
   * @return The state showing it, ready to be opened.
   * @throws std::runtime_error when the file cannot be read, is not a
   *         publication, or is not signed by whoever it claims.
   */
  MicroversionId readPublication(const std::string &path);

  /**
   * @brief Who publishes from this store, and what signs for them.
   *
   * Three places say, and the nearest wins: the per-user configuration file,
   * which is who somebody is; `author.yaml` beside the spools, which is who
   * they are for this store -- a pen name, a work identity; and whatever a
   * caller passes to publishDocument(), which is who they are for one
   * publication. Layered rather than merged into one setting because each
   * answers a different question, and because being asked to state an identity
   * again per document is how it ends up spelled three ways.
   *
   * Separate from @ref identity, which is the ed25519 key: that says "the same
   * publisher as last time" and nothing about who that is.
   */
  [[nodiscard]] const Config &settings();

  /// Who this store publishes as: the configuration, with the store's own
  /// override applied.
  [[nodiscard]] Author author();

  /// Record who this store publishes as, overriding the configuration for
  /// documents kept here.
  void setAuthor(Author who);

  /**
   * @brief This machine's publishing identity, minted on first use.
   *
   * An ed25519 key pair kept beside the spools. It is the name this machine's
   * publications are known by, so it has to be the same one tomorrow -- and
   * the secret half is the whole of the authority to publish under that name,
   * so the file is written readable only by its owner.
   *
   * @throws std::runtime_error when this build has no ed25519.
   */
  [[nodiscard]] const MutableKeys &identity();

  /**
   * @brief Publish @p version so it can be read off this machine.
   *
   * Seals whatever has been typed here into the machine's scroll first, which
   * is what gives local content a global address; a document made entirely of
   * quotations needs no sealing but is sealed with everything else anyway,
   * since the spool is one scroll and sealing it again only covers what is
   * new.
   *
   * @param salt Which document under this machine's name. The same salt
   *        publishes a further state of the same document.
   * @return Where the manifest was written.
   * @throws std::runtime_error when the version points at content that cannot
   *         be given a global address, or when this build has no ed25519.
   */
  /**
   * @brief Everything a publication says about itself, before it is made.
   *
   * What the dialog collects and what the command line fills in. Held together
   * rather than passed as five arguments because it is one decision -- how
   * this document goes out -- and because a caller that got the order wrong
   * would publish under the wrong name without any type saying so.
   */
  struct PublishRequest {
    /// Which document under this machine's name. The same salt publishes a
    /// further state of the same document.
    std::string salt;
    std::string title;
    /// Who to publish as. Empty fields fall back to @ref author.
    Author author;
    /// Anything else to record, as `key: value`.
    std::vector<std::pair<std::string, std::string>> extra;
    /**
     * @brief The key's passphrase, for this signature only.
     *
     * Not kept and not recorded. Empty is the ordinary case: a running
     * gpg-agent has already been given it, or the key has none.
     */
    std::string passphrase;
  };

  std::string publishDocument(const MicroversionId &version,
                              const PublishRequest &request,
                              std::size_t storeIndex = 0);

  /// Where publishing writes manifests, torrents and the sealed spool.
  [[nodiscard]] std::string publishedDir(std::size_t storeIndex = 0) const;

  /**
   * @brief Record a link, and move @p docIndex's view to the state that made
   *        it.
   *
   * A link changes no text, so the document on screen is unaffected; what
   * moves is where in hypertime that view sits, which is what makes making a
   * link an act that can be gone back past like any other.
   */
  MicroversionId addLink(std::uint32_t docIndex, Link link);

  [[nodiscard]] std::size_t storeCount() const { return stores.size(); }
  [[nodiscard]] Store &store(std::size_t index = 0);
  [[nodiscard]] const Store &store(std::size_t index = 0) const;

  /// Where the store is kept, and writing it there.
  [[nodiscard]] const std::string &path(std::size_t index = 0) const;
  [[nodiscard]] bool isTemporaryStore(std::size_t index = 0) const;
  void setStorePath(std::size_t index, std::string newPath,
                    bool isTemporary = false);

  std::size_t addStore(std::unique_ptr<Store> aStore, std::string aPath,
                       bool aIsTemporary = false);
  std::pair<std::size_t, MicroversionId>
  importFileToTemporaryStore(const std::string &filePath);
  std::size_t loadAuxiliaryStore(const std::string &aPath);

  void save(std::size_t index = 0) const;
  void saveAll() const;

  /// The version each open document shows, in the library's document order.
  [[nodiscard]] const std::vector<OpenView> &views() const { return open; }
  [[nodiscard]] std::vector<OpenView> &views() { return open; }
  [[nodiscard]] MicroversionId versionOf(std::uint32_t docIndex) const;
  [[nodiscard]] std::size_t storeIndexOf(std::uint32_t docIndex) const;

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
  void viewOpened(const MicroversionId &version, std::size_t storeIndex = 0);

  /**
   * @brief A source for @p version, ready to hand to the render queue.
   *
   * @param fontName What the reader's page will actually be laid out with.
   *        A media placeholder's height is sized from the decoded image (or
   *        the default video aspect) and this font's line pitch, so it must
   *        match whatever font the caller's Doc will use -- a mismatch here
   *        does not corrupt anything, but the reserved space and the
   *        widget's own size would disagree by however far the two fonts'
   *        line heights differ.
   */
  [[nodiscard]] std::shared_ptr<VersionTextSource>
  sourceFor(const MicroversionId &version, std::size_t storeIndex = 0,
            std::string_view fontName = "Monospace 16") const;

  struct MediaSpanInfo {
    PrimediaSpan span;
    std::uint32_t docOffset{0};
    std::string mime;
    bool isAudio{false};
    bool isVideo{false};
    bool isImage{false};
    std::string label;
    /// Where @p span sits within the whole media file it was classified
    /// against -- the offset (and that file's own total length) a
    /// transclusion of only part of a file leaves for a temporal or spatial
    /// sub-range to be computed from. Zero and equal to span.length when
    /// @p span already covers the whole file, which is the common case and
    /// needs no such translation.
    std::uint64_t containerOffset{0};
    std::uint64_t containerLength{0};
    /// The byte length of the placeholderFor() run reserved for this span in
    /// the document's concatext, immediately following @p docOffset -- what
    /// lets a caller outside session.cpp (LinkBeams::resolveAnchors) answer
    /// "does offset X fall inside this span's reserved range" without
    /// recomputing placeholder sizing itself.
    std::uint32_t reservedLength{0};
  };

  /// Discovered media spans for @p version with their document byte offsets.
  /// @param fontName See sourceFor(): must be the same font passed there, so
  ///        the offsets line up with what that call actually reserved.
  [[nodiscard]] std::vector<MediaSpanInfo>
  mediaSpansFor(const MicroversionId &version, std::size_t storeIndex = 0,
                std::string_view fontName = "Monospace 16") const;

  // -- Hypertime History & Scrubbing ----------------------------------------

  /**
   * @brief Return the linear ancestral microversion history chain for @p
   * docIndex.
   */
  [[nodiscard]] std::vector<MicroversionId>
  historyOf(std::uint32_t docIndex) const;

  /**
   * @brief Scrub/travel the document view at @p docIndex to @p version.
   */
  void scrubToVersion(std::uint32_t docIndex, const MicroversionId &version,
                      Doc &doc);

  /**
   * @brief Scrub document view backward by @p steps in its hypertime history.
   */
  bool scrubBackward(std::uint32_t docIndex, Doc &doc, std::size_t steps = 1);

  /**
   * @brief Scrub document view forward by @p steps in its hypertime history.
   */
  bool scrubForward(std::uint32_t docIndex, Doc &doc, std::size_t steps = 1);

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

  /**
   * @brief AppState::onDecoratedInsert's target: --type named decorations
   *        for text it just inserted, so record them as format links.
   *
   * Called after textInserted() has already advanced this document to the
   * version the insertion produced, so @p at / @p length address that
   * version directly. One Format link per decoration named in @p mask, all
   * sharing the same left content, each its own point in hypertime the same
   * as any other link.
   */
  void markDecorated(Doc &doc, std::uint32_t at, std::uint32_t length,
                     gleditor::DecorationMask mask);

  // -- Uncommitted Replay Log & Macro-Epoch Flush --------------------------
  static constexpr auto idleFlushTimeout = std::chrono::seconds(5);

  /**
   * @brief Flush any uncommitted edits in the replay log for @p docIndex (or
   * all open documents when nullopt) to the store.
   */
  void flushUncommitted(std::optional<std::uint32_t> docIndex = std::nullopt);

  /**
   * @brief Periodic tick called from the frame loop to flush uncommitted logs
   *        that have been idle for >= idleFlushTimeout.
   */
  void tick(std::chrono::steady_clock::time_point now =
                std::chrono::steady_clock::now());

  /// Whether @p docIndex has uncommitted edits waiting in the replay log.
  [[nodiscard]] bool hasUncommitted(std::uint32_t docIndex) const;

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
  DirectoryContentSource contentSource;
  /// Null unless useSwarm() was called.
  std::unique_ptr<SwarmContentSource> swarmSource;

  struct StoreEntry {
    std::unique_ptr<Store> store;
    std::string path;
    bool isTemporary{false};
  };
  std::vector<StoreEntry> stores;

  /// Bumped whenever a view or a link changes, which is what a cached set of
  /// decorations is checked against.
  std::uint64_t epoch{1};
  std::vector<OpenView> open;
  /// This machine's key pair, read or minted the first time it is wanted.
  std::optional<MutableKeys> keys;
  /// Who publishes here, read from the store the first time it is wanted.
  std::optional<Author> who;
  /// The per-user configuration, read once.
  std::optional<Config> config;

  MediaManager mediaManager_;
  gleditor::GroundingModal groundingModal_;

public:
  [[nodiscard]] MediaManager &mediaManager() { return mediaManager_; }
  [[nodiscard]] const MediaManager &mediaManager() const {
    return mediaManager_;
  }
  [[nodiscard]] gleditor::GroundingModal &groundingModal() {
    return groundingModal_;
  }
  [[nodiscard]] const gleditor::GroundingModal &groundingModal() const {
    return groundingModal_;
  }

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
class HypertimeMap : public gleditor::FrameContributor,
                     public gleditor::PickObserver,
                     public gleditor::a11y::Source {
public:
  HypertimeMap(std::string aFontName, const Session &aSession);
  ~HypertimeMap() override;

  HypertimeMap(const HypertimeMap &)            = delete;
  HypertimeMap &operator=(const HypertimeMap &) = delete;
  HypertimeMap(HypertimeMap &&)                 = delete;
  HypertimeMap &operator=(HypertimeMap &&)      = delete;

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;

  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return builtAt;
  }

  /// What happens when somebody clicks a state: show it.
  void setGoer(std::function<void(const MicroversionId &)> aGoer) {
    goer = std::move(aGoer);
  }

  void setVisible(const bool show) { visible = show; }
  void toggle() { visible = !visible; }

  /// Where the reader currently is. Highlighted in the graph.
  void setCurrent(const MicroversionId &id) { current = id; }

private:
  struct Node {
    MicroversionId id;
    float x{}, y{};
    float width{}, height{};
    std::string label;
  };
  struct Edge {
    std::size_t from{};
    std::size_t to{};
    bool isBranch{};
  };

  std::string fontName;
  const Session &session;
  std::unique_ptr<gleditor::Canvas> canvas;
  bool visible{false};
  MicroversionId current;
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::function<void(const MicroversionId &)> goer;
  std::uint64_t builtAt{};

  void layout(RenderState &state);
};

/**
 * @brief Still images placed at document byte offsets, drawn as real pixels.
 *
 * A media span classified as an image has no business going through
 * MediaWidget -- there is no play, pause or seek for a picture -- and no
 * business owning a pipeline of its own either: a document with a dozen
 * figures would need a dozen MediaWidget-style Canvases, and Vulkan's
 * descriptor pool is only sized for DeviceVK::maxPipelines. So this holds
 * one shared Canvas and one shared ImageCache for every image span across
 * every open document, and draws each placement with its own clear/commit/
 * draw cycle -- one shared pipeline, one draw call per image, rather than
 * one pipeline per image.
 */
class ImageOverlay : public gleditor::FrameContributor {
public:
  explicit ImageOverlay(std::string aFontName);
  ~ImageOverlay() override;

  ImageOverlay(const ImageOverlay &)            = delete;
  ImageOverlay &operator=(const ImageOverlay &) = delete;
  ImageOverlay(ImageOverlay &&)                 = delete;
  ImageOverlay &operator=(ImageOverlay &&)      = delete;

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;

  /// Forget every placed image. Called before syncMediaWidgets rebuilds its
  /// list from scratch, the same way mediaWidgets itself is rebuilt.
  void clear() { placements.clear(); }

  /**
   * @brief Decode @p bytes once (cached by @p id) and place it at
   *        @p docOffset within @p doc.
   *
   * Silently does nothing if the bytes fail to decode or the device is not
   * ready yet: a span libmagic called an image but SDL_image cannot open is
   * an image span with nothing usable in it, not a program error.
   */
  void place(std::shared_ptr<Doc> doc, std::uint32_t docOffset,
             const std::string &id, std::span<const std::uint8_t> bytes,
             const gleditor::MimeType &mime);

  /**
   * @brief The placed image at @p docOffset within @p doc, as an anchor
   *        naming its own centre and height -- the same page-pixel-space
   *        convention Doc::anchorFor() returns, so a caller (LinkBeams) can
   *        feed it through exactly the code that already turns a text
   *        anchor into a beam endpoint.
   *
   * Refactored out of drawFrame()'s own position math rather than
   * duplicated: an image's rectangle is anchorFor(docOffset) offset by the
   * same (height + 20px caption gap) and centred over the same width/height
   * drawFrame() draws, so keeping one formula is what keeps them from
   * drifting apart the way MediaWidget's two position branches once did
   * (see Phase 3's own writeup).
   *
   * @return nullopt when no placement matches @p docOffset exactly (nothing
   *         placed there, or the page it anchors to is not built yet).
   */
  [[nodiscard]] std::optional<Doc::Anchor>
  rectFor(const Doc &doc, std::uint32_t docOffset) const;

private:
  struct Placement {
    std::shared_ptr<Doc> doc;
    std::uint32_t docOffset{};
    gleditor::ImageResource image;
    float width{};
    float height{};
  };

  /// A placement's bottom-left corner in its own page's pixel space, and
  /// which page -- the one formula drawFrame() (to build a world transform)
  /// and rectFor() (to build a beam anchor) both derive from, so the two
  /// cannot drift into disagreeing about where an image actually is.
  struct Corner {
    std::uint32_t pageIndex{};
    float x{};
    float y{};
  };
  [[nodiscard]] static std::optional<Corner> bottomLeftOf(const Placement &p);

  std::string fontName;
  std::unique_ptr<gleditor::Canvas> canvas;
  std::unique_ptr<gleditor::ImageCache> imageCache;
  std::unique_ptr<gleditor::SvgCache> svgCache;
  std::vector<Placement> placements;
};

} // namespace xudu

#endif // XUDU_SESSION_H
