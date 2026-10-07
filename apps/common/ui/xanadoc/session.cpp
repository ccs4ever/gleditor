#include "common/ui/xanadoc/session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/caret.hpp>
#include <gleditor/decode_index.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/logging.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/svg_animator.hpp>
#include <gleditor/text_source.hpp>

#include "common/xanadu/format.hpp"
#include "common/xanadu/format_resolver.hpp"
#include "common/xanadu/link_layout.hpp"
#include "common/xanadu/provenance.hpp"

namespace xanadu {

namespace {

/// The branch @p version is on, as its name spells it without the state
/// number: "3b2" is on "3b", "a1" on "a", and the main line on nothing.
std::string branchOf(const MicroversionId &version) {
  const auto segments = version.segments();
  std::string out;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto &segment = segments[i];
    if (MicroversionId::noBranch != segment.branch) {
      out += MicroversionId::branchLetters(segment.branch);
    }
    if (i + 1 < segments.size()) {
      out += std::to_string(segment.number);
    }
  }
  return out;
}
constexpr std::uint32_t kCollaboratorColors[] = {
    0x38BDF8FF, // Sky 400
    0xF43F5EFF, // Rose 500
    0xA855F7FF, // Purple 500
    0x22C55EFF, // Green 500
    0xEAB308FF, // Yellow 500
    0xEC4899FF, // Pink 500
    0x06B6D4FF, // Cyan 500
    0xF97316FF, // Orange 500
};
/// A fresh directory under xanadocsDirectory(), named @p stem plus the
/// local time, so a folder of them sorts by when each was started.
std::filesystem::path untitledStoreDir(const std::string_view stem) {
  namespace fs    = std::filesystem;
  const auto base = xanadocsDirectory();
  // UTC: no time-zone database to depend on, and the names still sort.
  const auto now = std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
  const auto name = std::format("{}-{:%Y%m%d-%H%M%S}", stem, now);
  auto dir        = base / name;
  for (int n = 2; fs::exists(dir); ++n) {
    dir = base / std::format("{}-{}", name, n);
  }
  fs::create_directories(dir);
  return dir;
}
} // namespace

std::filesystem::path xanadocsDirectory() {
  namespace fs = std::filesystem;
  if (const char *xdgData = std::getenv("XDG_DATA_HOME");
      nullptr != xdgData && '\0' != *xdgData) {
    return fs::path(xdgData) / "xudu" / "xanadocs";
  }
  if (const char *home = std::getenv("HOME");
      nullptr != home && '\0' != *home) {
    return fs::path(home) / ".local" / "share" / "xudu" / "xanadocs";
  }
  return fs::temp_directory_path() / "xudu" / "xanadocs";
}

Session::Session(std::string aStorePath,
                 std::shared_ptr<UserPermascroll> scroll) {
  auto primaryStore = std::make_unique<Store>(std::move(scroll));
  primaryStore->load(aStorePath);
  loadRetainedScrolls(*primaryStore, aStorePath);
  primaryStore->setContentSource(&contentSource);
  stores.push_back(StoreEntry{.store       = std::move(primaryStore),
                              .path        = std::move(aStorePath),
                              .isTemporary = false});
}

const UserPermascroll *Session::userPermascroll() const {
  if (stores.empty() || !stores[0].store) {
    return nullptr;
  }
  return &stores[0].store->userPermascroll();
}

void Session::dumpPermascroll(const std::string &filePath) const {
  std::filesystem::create_directories(
      std::filesystem::path(filePath).parent_path());
  std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
  if (out && !stores.empty() && stores[0].store) {
    const auto &bytes = stores[0].store->userPermascroll().bytes();
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
}

void Session::saveOsmicTextAll() const {
  for (const auto &entry : stores) {
    if (entry.store && !entry.path.empty()) {
      entry.store->saveOsmicText(entry.path);
    }
  }
}

MicroversionId Session::importBranch(const std::size_t storeIndex,
                                     const std::string &filePath) {
  if (storeIndex >= stores.size() || !stores[storeIndex].store) {
    return MicroversionId{};
  }
  auto &st = *stores[storeIndex].store;
  const gleditor::FileTextSource source(filePath);
  auto imported = st.insert(MicroversionId{}, 0, source.text());
  for (const auto breakAt : source.forcedBreaks()) {
    imported = st.insertBreak(imported, breakAt);
  }
  save(storeIndex);
  return imported;
}

MicroversionId Session::insertText(const std::uint32_t docIndex,
                                   const std::uint32_t at,
                                   std::string_view newText) {
  if (docIndex >= open.size()) {
    return MicroversionId{};
  }
  const auto sIdx        = open[docIndex].storeIndex;
  auto &st               = store(sIdx);
  const auto prod        = st.insert(open[docIndex].version, at, newText);
  open[docIndex].version = prod;
  open[docIndex].pieces  = st.rebuild(prod);
  invalidate();
  if (st.isSystem()) {
    st.repointCurrentVersion(prod);
    save(sIdx);
    if (systemDocChangedCallback_) {
      if (const auto kind = systemDocKindForStoreIndex(sIdx)) {
        systemDocChangedCallback_(*kind, st);
      }
    }
  } else if (swarmSource) {
    if (auto appliedOp = st.getOp(prod)) {
      broadcastLiveOp(docIndex, *appliedOp, prod, newText,
                      at + static_cast<std::uint32_t>(newText.size()), 0);
    }
  }
  return prod;
}

Scroll Session::retainMediaScroll(std::string_view bytes,
                                  const std::string &fileName,
                                  const std::string &mimeType,
                                  const std::filesystem::path &seedDirectory) {
  const std::vector<TorrentContent> files{
      {.path = fileName, .data = std::string(bytes)}};
  const auto made = makeTorrent(files, "media");
  const auto seed = writeTorrentSeed(seedDirectory, made, files);
  addTorrentMemory(made.file, seed.string());
  auto scroll = Scroll::ofTorrentFile(made.hash, 0, fileName, 0, bytes.size());
  if (!mimeType.empty()) {
    scroll.defaultMimeType      = mimeType;
    scroll.segments[0].mimeType = mimeType;
  }
  return scroll;
}

MicroversionId Session::insertMedia(const std::uint32_t docIndex,
                                    const std::uint32_t at,
                                    std::string_view bytes,
                                    const std::string &mimeType,
                                    std::string filePath) {
  if (docIndex >= open.size()) {
    return MicroversionId{};
  }
  const auto focus = focusTargetForView(docIndex);
  if (!focus.has_value() || !focus->isValid()) {
    return MicroversionId{};
  }
  const auto sIdx = open[docIndex].storeIndex;
  auto &st        = store(sIdx);

  const auto fileName =
      filePath.empty() ? "media.dat"
                       : std::filesystem::path(filePath).filename().string();
  const auto scroll =
      retainMediaScroll(bytes, fileName, mimeType, publishedDir(sIdx));

  const auto prod = st.transcludeExternal(open[docIndex].version, at, scroll, 0,
                                          bytes.size());
  open[docIndex].version = prod;
  open[docIndex].pieces  = st.rebuild(prod);
  invalidate();
  return prod;
}

MicroversionId Session::insertBreak(const std::uint32_t docIndex,
                                    const std::uint32_t at) {
  if (docIndex >= open.size()) {
    return MicroversionId{};
  }
  flushUncommitted(docIndex);
  const auto sIdx = open[docIndex].storeIndex;
  auto &st        = store(sIdx);
  const auto prod = st.insertBreak(open[docIndex].version, at);
  save(sIdx);
  refresh(docIndex, prod);
  return prod;
}

MicroversionId Session::insertSpan(const std::uint32_t docIndex,
                                   const std::uint32_t at,
                                   const PrimediaSpan &span) {
  if (docIndex >= open.size()) {
    return MicroversionId{};
  }
  const auto sIdx        = open[docIndex].storeIndex;
  auto &st               = store(sIdx);
  const auto prod        = st.insertSpan(open[docIndex].version, at, span);
  open[docIndex].version = prod;
  open[docIndex].pieces  = st.rebuild(prod);
  invalidate();
  return prod;
}

MicroversionId Session::transclude(const std::uint32_t destDocIndex,
                                   const std::uint32_t destPos,
                                   const std::uint32_t srcDocIndex,
                                   const std::uint32_t srcStart,
                                   const std::uint32_t srcLength) {
  if (destDocIndex >= open.size() || srcDocIndex >= open.size()) {
    return MicroversionId{};
  }
  const auto srcVer    = open[srcDocIndex].pieces;
  const auto spans     = srcVer.spansFor(srcStart, srcLength);
  const auto destSIdx  = open[destDocIndex].storeIndex;
  auto &st             = store(destSIdx);
  auto curVer          = open[destDocIndex].version;
  std::uint32_t curPos = destPos;
  for (const auto &span : spans) {
    curVer = st.insertSpan(curVer, curPos, span);
    curPos += span.length;
  }
  open[destDocIndex].version = curVer;
  open[destDocIndex].pieces  = st.rebuild(curVer);
  invalidate();
  return curVer;
}

MicroversionId Session::transcludeText(const std::uint32_t destDocIndex,
                                       const std::uint32_t destPos,
                                       const std::uint32_t srcDocIndex,
                                       std::string_view queryText) {
  if (destDocIndex >= open.size() || srcDocIndex >= open.size()) {
    return MicroversionId{};
  }
  const auto srcSIdx = open[srcDocIndex].storeIndex;
  const auto srcText = store(srcSIdx).textOf(open[srcDocIndex].version);
  const auto pos     = srcText.find(queryText);
  if (pos == std::string::npos) {
    throw std::runtime_error("query text not found in source document: " +
                             std::string(queryText));
  }
  return transclude(destDocIndex, destPos, srcDocIndex,
                    static_cast<std::uint32_t>(pos),
                    static_cast<std::uint32_t>(queryText.size()));
}

// Both catches below block every path out of flushUncommitted(); the only
// residual throw source is bad_alloc from formatting the diagnostic itself,
// the same unavoidable risk any other noexcept-adjacent code accepts.
// NOLINTNEXTLINE(bugprone-exception-escape)
Session::~Session() {
  publicationSubscriptions_.reset();
  linkPackageExchange_.reset();
  publicationDiscovery_.reset();
  publicationInbox_.reset();
  try {
    flushUncommitted();
  } catch (const std::exception &err) {
    // A throwing destructor risks std::terminate() if this runs during
    // unwinding from another exception, so the flush's own failure is
    // reported and swallowed rather than propagated.
    std::cerr << std::format(
        "xudu [warning]: failed to flush uncommitted edits on session "
        "teardown: {}\n",
        err.what());
  } catch (...) {
    // Same reasoning as above, for a throw that isn't std::exception-derived.
    std::cerr << "xudu [warning]: failed to flush uncommitted edits on "
                 "session teardown (non-standard exception)\n";
  }
  // An untitled store is kept once anything was written to it: typing is
  // saved as it happens, so deleting the directory here would throw away
  // work the reader never chose to discard. One opened and left alone is
  // clutter, and goes.
  for (const auto &entry : stores) {
    if (!entry.isTemporary || entry.path.empty() || !entry.store) {
      continue;
    }
    if (entry.store->opCount() == entry.opsWhenOpened) {
      std::error_code ec;
      std::filesystem::remove_all(entry.path, ec);
    } else {
      std::cout << "xudu: kept untitled xanadoc at " << entry.path << "\n";
    }
  }
}

void Session::useSwarm(const bool privateNetwork) {
  SwarmContentSource::Options options;
  options.listenInterfaces              = "0.0.0.0:0";
  options.restrictDhtToDistinctNetworks = !privateNetwork;
  swarmSource = std::make_unique<SwarmContentSource>(std::move(options));
  for (auto &entry : stores) {
    if (entry.store) {
      entry.store->setContentSource(swarmSource.get());
    }
  }
}

std::uint16_t Session::swarmPort() const {
  return nullptr == swarmSource ? 0 : swarmSource->listenPort();
}

const ContentSource &Session::content() const {
  if (nullptr != swarmSource) {
    return *swarmSource;
  }
  return contentSource;
}

void Session::connectPeer(const InfoHash &hash, const std::string &host,
                          const std::uint16_t port) {
  if (nullptr != swarmSource) {
    swarmSource->connectPeer(hash, host, port);
  }
}

bool Session::awaitMetadata(const InfoHash &hash,
                            const std::chrono::milliseconds timeout) {
  if (content().metainfo(hash).has_value()) {
    return true;
  }
  return nullptr != swarmSource && swarmSource->waitForMetadata(hash, timeout);
}

void Session::addDhtNode(const std::string &host, const std::uint16_t port) {
  if (nullptr != swarmSource) {
    swarmSource->addDhtNode(host, port);
  }
}

InfoHash Session::addName(const std::string &uri) {
  const auto link = MutableLink::parse(uri);
  if (nullptr == swarmSource) {
    throw std::runtime_error(
        "name " + link.key.hex() +
        " can only be resolved through the DHT, which needs --swarm. A name "
        "is not content: it says who is publishing, and the DHT says what "
        "they are currently pointing at.");
  }
  const auto pointer =
      swarmSource->resolveMutable(link, std::chrono::seconds{60});
  if (!pointer.has_value()) {
    throw std::runtime_error(
        "name " + link.key.hex() +
        " has no answer in the DHT. Either nothing has been published under "
        "it, or this machine is not in a DHT that has heard of it -- name a "
        "node with --dht-node.");
  }
  return addMagnet("magnet:?xt=urn:btih:" + pointer->hash.hex());
}

InfoHash Session::addTorrentMemory(const std::string_view torrentData,
                                   const std::string &dataRoot) {
  const auto root = dataRoot.empty() ? "." : dataRoot;
  if (nullptr != swarmSource) {
    return swarmSource->addTorrent(torrentData, root, false);
  }
  return contentSource.add(torrentData, root);
}

InfoHash Session::addTorrent(const std::string &torrentPath,
                             const std::string &dataRoot) {
  std::ifstream in(torrentPath, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot read torrent: " + torrentPath);
  }
  const std::string contents{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};
  const auto root =
      dataRoot.empty()
          ? std::filesystem::path(torrentPath).parent_path().string()
          : dataRoot;
  return addTorrentMemory(contents, root);
}

InfoHash Session::addMagnet(const std::string &uri) {
  const auto link = MagnetLink::parse(uri);
  if (nullptr != swarmSource) {
    return swarmSource->addMagnet(uri, path(0) + "-content");
  }
  if (!contentSource.metainfo(link.hash)) {
    throw std::runtime_error(
        "magnet " + link.hash.hex() +
        " names content whose metadata is not available here. A magnet link "
        "carries only the name; the piece hashes it needs to be verified "
        "against live in the torrent's info dictionary, which a client "
        "normally fetches from the swarm. Give the matching .torrent with "
        "--torrent.");
  }
  return link.hash;
}

MicroversionId
Session::quoteTorrent(const MicroversionId &parent, const std::uint32_t at,
                      const InfoHash &hash, const std::uint32_t fileIndex,
                      const std::uint64_t offset, const std::uint64_t length) {
  const auto meta = content().metainfo(hash);
  if (!meta) {
    throw std::runtime_error("no torrent " + hash.hex() +
                             " has been made available");
  }
  if (fileIndex >= meta->files().size()) {
    throw std::runtime_error("torrent " + hash.hex() + " has no file " +
                             std::to_string(fileIndex));
  }
  const auto &file  = meta->files()[fileIndex];
  const auto scroll = Scroll::ofTorrentFile(hash, fileIndex, file.path,
                                            file.offset, file.length);
  const auto count =
      0 == length ? file.length - std::min(offset, file.length) : length;
  return store(0).transcludeExternal(parent, at, scroll, offset, count);
}

std::string Session::publishedDir(const std::size_t storeIndex) const {
  const auto &p = path(storeIndex);
  return p.empty() ? "published"
                   : (std::filesystem::path(p) / "published").string();
}

namespace {

std::string authorPath(const std::string &storePath) {
  return (std::filesystem::path(storePath) / "author.tsv").string();
}

/// Where a store's SealState lives -- what makes the next publishDocument()
/// know which operations the last publication sealed across separate runs.
/// Primedia sealing belongs to the shared author's UserPermascroll; each store
/// retains its own operations segments here.
std::string sealStatePath(const std::string &storePath) {
  return (std::filesystem::path(storePath) / "seal-state").string();
}

SealState loadSealState(const std::string &storePath) {
  const auto path = sealStatePath(storePath);
  if (!std::filesystem::exists(path)) return {};
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read publication seal state: " + path);
  const std::string text{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  const auto state = decodeSealState(text);
  if (!state || in.bad())
    throw std::runtime_error("unreadable publication seal state: " + path);
  return *state;
}

void saveSealState(const std::string &storePath, const SealState &state) {
  std::filesystem::create_directories(storePath);
  const auto path = sealStatePath(storePath);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << encodeSealState(state);
  out.close();
  if (!out)
    throw std::runtime_error("cannot write publication seal state: " + path);
}

} // namespace

void Session::setAuthor(Author aWho) {
  who = std::move(aWho);
  Config record;
  record.author = *who;
  std::filesystem::create_directories(path(0));
  std::ofstream out(authorPath(path(0)), std::ios::trunc);
  out << record.toTsv();
}

const Config &Session::settings() {
  if (!config.has_value()) {
    config = loadConfig();
  }
  return *config;
}

Author Session::author() {
  auto chosen = settings().author;
  if (!who.has_value()) {
    if (std::ifstream in(authorPath(path(0))); in) {
      const std::string text{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};
      if (const auto record = Config::fromTsv(text); record) {
        who = record->author;
      }
    }
  }
  if (who.has_value()) {
    if (!who->name.empty()) {
      chosen.name = who->name;
    }
    if (!who->email.empty()) {
      chosen.email = who->email;
    }
    if (!who->gpgKey.empty()) {
      chosen.gpgKey = who->gpgKey;
    }
  }
  return chosen;
}

const MutableKeys &Session::identity() {
  if (keys.has_value()) {
    return *keys;
  }
  const auto p = std::filesystem::path(path(0)) / "identity";
  if (std::ifstream in(p); in) {
    std::string publicHex;
    std::string secretHex;
    in >> publicHex >> secretHex;
    if (!publicHex.empty() && !secretHex.empty()) {
      keys = MutableKeys{.publicKey = PublicKey::fromHex(publicHex),
                         .secretKey = SecretKey::fromHex(secretHex)};
      return *keys;
    }
  }

  keys = createMutableKeys();
  std::filesystem::create_directories(path(0));
  {
    std::ofstream out(p, std::ios::trunc);
    out << keys->publicKey.hex() << "\n" << keys->secretKey.hex() << "\n";
  }
  std::error_code ignored;
  std::filesystem::permissions(p,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, ignored);
  std::cout << "xudu: minted this machine's name " << keys->publicKey.hex()
            << "\n";
  return *keys;
}

void Session::configureTestPublicationSwarm(
    const std::string &listen,
    std::vector<std::pair<std::string, std::uint16_t>> nodes) {
  if (publicationOutbox_ || publicationInbox_ || publicationDiscovery_ ||
      publicationSubscriptions_ || linkPackageExchange_)
    throw std::logic_error("publication outbox is already running");
  testPublicationSwarm_ = true;
  publicationListen_    = listen;
  publicationNodes_     = std::move(nodes);
  (void)publicationOutbox();
}

PublicationOutbox &Session::publicationOutbox() {
  if (!publicationOutbox_) {
    PublicationOutbox::Options options;
    options.directory = std::filesystem::path(path(0)) / "publication-outbox";
    if (testPublicationSwarm_) {
      const auto keys        = identity();
      options.verifyIdentity = [key = keys.publicKey](const Publication &pub) {
        return pub.publisher == key ? PublicationIdentity::MockVerified
                                    : PublicationIdentity::Unknown;
      };
      SwarmContentSource::Options swarm;
      swarm.listenInterfaces               = publicationListen_;
      swarm.restrictDhtToDistinctNetworks  = false;
      swarm.allowManyConnectionsPerAddress = true;
      swarm.dhtPacketsPerSecond            = 100;
      options.makeTransport                = publicationSwarmTransport(
          keys, swarm, publicationNodes_,
          xanadocsDirectory().parent_path() / "author-catalog" /
              keys.publicKey.hex());
    }
    publicationOutbox_ =
        std::make_unique<PublicationOutbox>(std::move(options));
  }
  return *publicationOutbox_;
}

PublicationInbox &Session::publicationInbox() {
  if (!publicationInbox_) {
    PublicationInbox::Options options;
    options.directory = xanadocsDirectory().parent_path() / "publication-inbox";
    if (testPublicationSwarm_) {
      SwarmContentSource::Options swarm;
      const auto colon       = publicationListen_.rfind(':');
      swarm.listenInterfaces = publicationListen_.substr(0, colon + 1) + "0";
      swarm.restrictDhtToDistinctNetworks  = false;
      swarm.allowManyConnectionsPerAddress = true;
      swarm.dhtPacketsPerSecond            = 100;
      options.makeTransport =
          publicationDownloadSwarmTransport(swarm, publicationNodes_);
    }
    publicationInbox_ = std::make_unique<PublicationInbox>(std::move(options));
  }
  return *publicationInbox_;
}

ReaderLinkPackages &Session::readerLinkPackages() {
  if (!readerLinkPackages_) {
    auto reader = std::make_unique<ReaderLinkPackages>(
        xanadocsDirectory().parent_path() / "package-visibility");
    if (!reader->preferences().empty())
      for (const auto &status : linkPackageExchange().statuses())
        if (status.phase == LinkPackagePhase::Ready ||
            status.phase == LinkPackagePhase::Published ||
            (!status.received && !status.package.links.empty())) {
          if (auto result = reader->retain(status.package, status.hash);
              !result)
            throw ReaderLinkPackagesUnreadable(result.error());
        }
    for (const auto &entry : stores)
      if (entry.store && reader->containsAuthority(entry.store->documentId()))
        throw ReaderLinkPackagesUnreadable(
            "Package authority conflicts with an open store");
    readerLinkPackages_ = std::move(reader);
  }
  return *readerLinkPackages_;
}
std::expected<Session *, std::string>
Session::setLinkPackageEnabled(const LinkPackageStatus &package, bool on) try {
  if (on && package.phase != LinkPackagePhase::Ready &&
      package.phase != LinkPackagePhase::Published)
    throw std::invalid_argument("Verify the package before enabling it");
  auto &reader = readerLinkPackages();
  if (on)
    for (const auto &entry : stores)
      if (entry.store &&
          entry.store->documentId() ==
              ReaderLinkPackages::keyOf(package.hash, 0).authority)
        throw std::invalid_argument(
            "Package authority conflicts with an open store");
  if (on)
    if (auto result = reader.retain(package.package, package.hash); !result)
      throw std::invalid_argument(result.error());
  if (auto result = reader.setEnabled(package.hash, on); !result)
    throw ReaderLinkPackagesUnreadable(result.error());
  invalidate();
  return this;
} catch (const std::exception &error) {
  return std::unexpected(std::string(error.what()));
}
std::uint64_t Session::packageRenderId(const LinkKey &key) {
  const auto name = std::pair{key.authority.str(), key.id};
  if (const auto found = packageRenderIds_.find(name);
      found != packageRenderIds_.end())
    return found->second;
  // Native operation indices are 32 bits. This private presentation namespace
  // never leaves the renderer; activity uses the verified immutable authority.
  const auto id = (1ULL << 32U) + packageRenderKeys_.size();
  packageRenderKeys_.emplace(id, key);
  packageRenderIds_.emplace(name, id);
  return id;
}
LinkKey Session::presentationLinkKey(const std::uint64_t id) const {
  if (const auto found = packageRenderKeys_.find(id);
      found != packageRenderKeys_.end())
    return found->second;
  return {.authority = store().documentId(),
          .id        = static_cast<zigzag::CellRef>(id)};
}
LinkPackageExchange &Session::linkPackageExchange() {
  if (!linkPackageExchange_) {
    LinkPackageExchange::Options options;
    options.directory = xanadocsDirectory().parent_path() / "link-packages";
    if (testPublicationSwarm_) {
      const auto mine        = identity();
      options.verifyIdentity = [key = mine.publicKey](const LinkPackage &pkg) {
        return pkg.curator == key ? PublicationIdentity::MockVerified
                                  : PublicationIdentity::Unknown;
      };
      SwarmContentSource::Options swarm;
      const auto colon       = publicationListen_.rfind(':');
      swarm.listenInterfaces = publicationListen_.substr(0, colon + 1) + "0";
      swarm.restrictDhtToDistinctNetworks  = false;
      swarm.allowManyConnectionsPerAddress = true;
      swarm.dhtPacketsPerSecond            = 100;
      options.makePublisher                = publicationSwarmTransport(
          mine, swarm, publicationNodes_,
          xanadocsDirectory().parent_path() / "author-catalog" /
              mine.publicKey.hex());
      options.makeDownloader =
          publicationDownloadSwarmTransport(swarm, publicationNodes_);
    }
    linkPackageExchange_ =
        std::make_unique<LinkPackageExchange>(std::move(options));
  }
  return *linkPackageExchange_;
}
PublicationPin Session::pinPublication(const Publication &pub) const {
  const std::vector<TorrentContent> files{
      {.path = "publication.xanadoc", .data = encodePublication(pub)}};
  return {.publisher = pub.publisher,
          .salt      = pub.salt,
          .hash      = makeTorrent(files, "publication").hash,
          .sequence  = pub.sequence,
          .version   = pub.version,
          .title     = pub.title};
}
std::vector<Publication> Session::packagePublicationSources() {
  std::vector<std::filesystem::path> files;
  for (std::size_t i = 0; i < stores.size(); ++i) {
    const auto root = publishedDir(i);
    if (!std::filesystem::is_directory(root)) continue;
    for (const auto &entry : std::filesystem::directory_iterator(root))
      if (entry.is_regular_file() && entry.path().extension() == ".xanadoc")
        files.push_back(entry.path());
  }
  for (const auto &status : publicationInbox().statuses())
    if (status.phase == PublicationDownloadPhase::Ready)
      files.push_back(status.storePath.parent_path() / "publication.xanadoc");
  std::vector<Publication> result;
  std::set<std::string> hashes;
  for (const auto &file : files) {
    if (result.size() >= 128) break;
    if (!std::filesystem::is_regular_file(file) ||
        std::filesystem::file_size(file) > 16 * 1024 * 1024)
      continue;
    std::ifstream input(file, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    const auto pub = decodePublication(bytes);
    if (pub && verifyPublication(*pub) &&
        hashes.insert(pinPublication(*pub).hash.hex()).second)
      result.push_back(*pub);
  }
  std::ranges::sort(result, {}, [](const auto &pub) {
    return pub.title + pub.publisher.hex() + std::to_string(pub.sequence);
  });
  return result;
}
std::string Session::prepareLinkPackage(const Publication &source,
                                        const std::string &salt,
                                        const std::string &title,
                                        bool announce) {
  if (!verifyPublication(source))
    throw std::invalid_argument("Source publication signature failed");
  const auto mine    = identity();
  std::int64_t floor = 0;
  for (const auto &status : linkPackageExchange().statuses())
    if (status.package.curator == mine.publicKey && status.package.salt == salt)
      floor = std::max(floor, status.package.sequence);
  const auto sequence = reservePublicationSequence(
      std::filesystem::path(path(0)) / "publication-sequences", mine.publicKey,
      salt, floor);
  std::map<std::string, Scroll> scrolls;
  for (const auto &link : source.links)
    for (const auto *ends : {&link.left, &link.right})
      for (const auto &span : *ends)
        scrolls.emplace(span.scroll, source.scrolls.at(span.scroll));
  const auto now = static_cast<std::uint64_t>(
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
  const auto pkg =
      publishLinkPackage(mine, salt, title, sequence, now, source.links,
                         std::move(scrolls), {pinPublication(source)});
  return linkPackageExchange().submit(pkg, announce);
}

PublicationDiscovery &Session::publicationDiscovery() {
  if (!publicationDiscovery_) {
    PublicationDiscovery::Options options;
    options.directory =
        xanadocsDirectory().parent_path() / "publication-discovery";
    if (testPublicationSwarm_) {
      SwarmContentSource::Options swarm;
      const auto colon       = publicationListen_.rfind(':');
      swarm.listenInterfaces = publicationListen_.substr(0, colon + 1) + "0";
      swarm.restrictDhtToDistinctNetworks  = false;
      swarm.allowManyConnectionsPerAddress = true;
      swarm.dhtPacketsPerSecond            = 100;
      options.makeTransport                = publicationDiscoverySwarmTransport(
          swarm, publicationNodes_, options.directory / "scratch");
    }
    publicationDiscovery_ =
        std::make_unique<PublicationDiscovery>(std::move(options));
  }
  return *publicationDiscovery_;
}

PublicationSubscriptions &Session::publicationSubscriptions() {
  if (!publicationSubscriptions_) {
    PublicationSubscriptions::Options options;
    options.directory =
        xanadocsDirectory().parent_path() / "publication-subscriptions";
    options.inbox = &publicationInbox();
    const auto model =
        SystemStoreModel::fromStore(systemStore(SystemDocKind::Settings));
    const auto seconds =
        model.getInt64(xanadu::settings::kPublicationPollSeconds, 30);
    if (seconds < 1 ||
        seconds > std::chrono::milliseconds::max().count() / 1000)
      throw std::invalid_argument(
          "publicationPollSeconds must be positive and fit the polling clock");
    options.pollInterval = std::chrono::seconds{seconds};
    if (testPublicationSwarm_) {
      SwarmContentSource::Options swarm;
      const auto colon       = publicationListen_.rfind(':');
      swarm.listenInterfaces = publicationListen_.substr(0, colon + 1) + "0";
      swarm.restrictDhtToDistinctNetworks  = false;
      swarm.allowManyConnectionsPerAddress = true;
      swarm.dhtPacketsPerSecond            = 100;
      options.makeTransport                = publicationDownloadSwarmTransport(
          swarm, publicationNodes_, {}, std::chrono::seconds{30});
      options.pollInterval = std::chrono::seconds{2};
    }
    publicationSubscriptions_ =
        std::make_unique<PublicationSubscriptions>(std::move(options));
  }
  return *publicationSubscriptions_;
}

std::pair<std::size_t, MicroversionId>
Session::openDownloadedPublication(std::string_view id) {
  const auto downloaded = publicationInbox().status(id);
  if (downloaded.phase != PublicationDownloadPhase::Ready)
    throw std::runtime_error("Publication is not ready to open");
  const auto target = std::filesystem::weakly_canonical(downloaded.storePath);
  auto index        = stores.size();
  for (std::size_t candidate = 0; candidate < stores.size(); ++candidate) {
    if (!stores[candidate].path.empty() &&
        std::filesystem::weakly_canonical(stores[candidate].path) == target) {
      index = candidate;
      break;
    }
  }
  if (index == stores.size())
    index = loadAuxiliaryStore(downloaded.storePath.string());
  invalidate();
  std::cout << "xudu: opened downloaded publication " << downloaded.title
            << " (complete store " << index << ")\n";
  return {index, downloaded.version};
}

std::string Session::publishDocument(const MicroversionId &version,
                                     const PublishRequest &request,
                                     const std::size_t storeIndex) {
  if (request.salt.empty() || request.salt.size() > 64 || request.salt == "." ||
      request.salt == ".." ||
      request.salt.find_first_of("/\\") != std::string::npos ||
      request.salt.find('\0') != std::string::npos)
    throw std::invalid_argument(
        "publication name must be 1–64 bytes without path separators");
  if (request.announce && !testPublicationSwarm_)
    throw std::runtime_error(
        "test swarm publication requires mock verification configuration");
  flushUncommitted();
  auto &st = store(storeIndex);
  st.sealMetadata();
  if (request.editionToRepoint && !request.newEditionName.empty())
    throw std::invalid_argument("choose repointing or a new edition");
  auto metadataHead = st.structureHead();
  if (metadataHead.isZero()) metadataHead = st.latest();
  if (request.editionToRepoint)
    (void)st.repointEdition(metadataHead, *request.editionToRepoint, version);
  else if (!request.newEditionName.empty())
    (void)st.designateEdition(metadataHead, request.newEditionName, version,
                              /*allowDuplicateName=*/true);
  st.sealMetadata();
  const auto &mine = identity();
  const auto into  = publishedDir(storeIndex);
  const auto now   = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  auto who = author();
  if (!request.author.name.empty()) {
    who.name = request.author.name;
  }
  if (!request.author.email.empty()) {
    who.email = request.author.email;
  }
  if (!request.author.gpgKey.empty()) {
    who.gpgKey = request.author.gpgKey;
  }
  if (!who.named()) {
    throw std::runtime_error(
        "nobody has been named as the author. Publishing binds a person to "
        "what they published, so say who once, in " +
        configPath() +
        ":\n"
        "  author\\tYour Name\n"
        "  email\\tyou@example.org\n"
        "or for this store alone with --author-name and --author-email.");
  }

  std::int64_t observedFloor = 0;
  for (const auto &loaded : stores) {
    const auto priorPath = std::filesystem::path(loaded.path) / "published" /
                           (request.salt + ".xanadoc");
    if (std::ifstream in(priorPath, std::ios::binary); in) {
      const std::string bytes{std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>()};
      const auto prior = decodePublication(bytes);
      if (!prior || prior->publisher != mine.publicKey ||
          prior->salt != request.salt) {
        throw std::runtime_error(
            "cannot advance an invalid prior publication: " +
            priorPath.string());
      }
      observedFloor = std::max(observedFloor, prior->sequence);
    }
  }
  const auto sequence = reservePublicationSequence(
      std::filesystem::path(path(0)) / "publication-sequences", mine.publicKey,
      request.salt, observedFloor);

  Provenance record;
  record.author    = who;
  record.salt      = request.salt;
  record.title     = request.title;
  record.extra     = request.extra;
  record.publisher = mine.publicKey.hex();
  if (st.userPermascrollPtr()) {
    record.permascroll = st.userPermascroll().globalScrollKey();
  }
  record.version   = version.str();
  record.published = now;
  // Primedia travels in its author's separate incremental seal; this record
  // describes the history torrent, which contains no primedia payload.
  record.contentLength = 0;
  record.contentDigest = sha256Hex({});

  const auto priorState = loadSealState(path(storeIndex));
  // Sign exactly the incremental operations file sealLocalSpool() will write.
  const auto sealedOps = priorState.opsAlreadySealed < st.opCount()
                             ? sealableOps(st, priorState.opsAlreadySealed)
                             : std::string{};
  record.opsLength     = sealedOps.size();
  if (!sealedOps.empty()) record.opsDigest = sha256Hex(sealedOps);

  for (const auto &piece : st.rebuild(version).pieces()) {
    if (piece.isLocal()) {
      continue;
    }
    if (const auto scroll = st.scroll(piece.scroll); scroll.has_value()) {
      const auto key = scrollKey(*scroll);
      if (!key.empty() &&
          std::ranges::find(record.quotes, key) == record.quotes.end()) {
        record.quotes.push_back(key);
      }
    }
  }
  const auto withheldHoles = collectWithheldHoles();
  const auto signing       = settings().signing(request.passphrase);
  const auto provenance    = signProvenance(record, signing);
  if (st.userPermascrollPtr()) {
    const auto from     = st.userPermascroll().currentScroll().length();
    const auto allBytes = st.userPermascroll().bytes();
    if (from > allBytes.size())
      throw std::runtime_error("sealed permascroll exceeds local bytes");
    SignedProvenance primediaProvenance;
    if (from < allBytes.size()) {
      auto primediaRecord = record;
      const auto wireBytes =
          publicationPrimedia(allBytes.substr(from), from, withheldHoles);
      primediaRecord.contentLength = wireBytes.size();
      primediaRecord.contentDigest = sha256Hex(wireBytes);
      primediaRecord.opsLength     = 0;
      primediaRecord.opsDigest.clear();
      primediaRecord.extra.emplace_back("permascroll_at", std::to_string(from));
      primediaProvenance = signProvenance(primediaRecord, signing);
    }
    const auto newlySealed = st.userPermascrollPtr()->sealIncremental(
        into, primediaProvenance, withheldHoles);
    if (newlySealed.has_value() && swarmSource) {
      if (localAuthorScrollKey_.empty()) {
        localAuthorScrollKey_ = st.userPermascroll().globalScrollKey();
      }
      swarmSource->broadcastScrollSealed(
          SwarmContentSource::ScrollSealedBroadcast{
              .swarmHash       = collabRoomHash_,
              .authorScrollKey = localAuthorScrollKey_,
              .sealedUpTo      = newlySealed->end(),
              .pieceInfoHash   = newlySealed->torrent,
              .timestamp =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count(),
          });
    }
  }

  const auto sharedScroll = st.userPermascrollPtr()
                                ? st.userPermascroll().currentScroll()
                                : priorState.scroll;
  const auto &scrollKeys =
      st.userPermascrollPtr() ? st.userPermascroll().config().deviceKeys : mine;
  const auto sealed =
      sealLocalSpool(st, scrollKeys, sharedScroll.salt, into, provenance,
                     sharedScroll, priorState.opsAlreadySealed, withheldHoles);

  auto opsSegments = priorState.opsSegments;
  if (sealed.opsSegment.has_value()) {
    opsSegments.push_back(*sealed.opsSegment);
  }
  const auto pub =
      publish(st, version, mine, request.salt, request.title, sequence, now,
              &sealed.scroll, opsSegments, withheldHoles, request.topics);

  SealState nextState;
  nextState.scroll           = sealed.scroll;
  nextState.opsAlreadySealed = static_cast<std::uint32_t>(st.opCount());
  nextState.opsSegments      = opsSegments;
  saveSealState(path(storeIndex), nextState);

  std::filesystem::create_directories(into);
  const auto outPath =
      (std::filesystem::path(into) / (request.salt + ".xanadoc")).string();
  std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
  out << encodePublication(pub);
  out.close();
  if (!out) throw std::runtime_error("cannot write publication: " + outPath);
  std::vector<std::filesystem::path> roots;
  for (std::size_t i = 0; i < stores.size(); ++i)
    if (!stores[i].path.empty()) roots.emplace_back(publishedDir(i));
  (void)publicationOutbox().submit(pub, roots, request.announce);
  return outPath;
}

std::pair<std::size_t, MicroversionId>
Session::readPublication(const std::string &aPath) {
  std::ifstream in(aPath, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot read publication: " + aPath);
  }
  const std::string encoded{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
  const auto pub = decodePublication(encoded);
  if (!pub) {
    throw std::runtime_error(
        aPath + " is not a publication, or is not signed by whoever it claims "
                "to be from. A manifest that does not verify is somebody's "
                "claim to have published what they did not.");
  }
  std::vector<std::filesystem::path> roots{
      std::filesystem::path(aPath).parent_path()};
  for (std::size_t i = 0; i < stores.size(); ++i)
    roots.emplace_back(publishedDir(i));
  // A private snapshot per opening avoids overwriting an earlier publication
  // or the reader's local edits when the author announces a newer version.
  const auto destination = untitledStoreDir("publication");
  std::filesystem::remove(
      destination); // installer requires exclusive ownership
  auto restored = installPublication(
      *pub, roots, stores[0].store->userPermascrollPtr(), destination);
  const auto index = addStore(std::move(restored), destination.string(), false);
  invalidate();
  std::cout << "xudu: read " << pub->describe() << " as " << pub->version.str()
            << " (complete store " << index << ")\n";
  return {index, pub->version};
}

MicroversionId Session::addLink(const std::uint32_t docIndex, Link link) {
  flushUncommitted(docIndex);
  if (docIndex >= open.size()) {
    return MicroversionId{};
  }
  const auto sIdx     = open[docIndex].storeIndex;
  auto &st            = store(sIdx);
  const auto produced = st.addLink(open[docIndex].version, std::move(link));
  refresh(docIndex, produced);
  save(sIdx);
  return produced;
}

Store &Session::store(const std::size_t index) {
  if (index >= stores.size()) {
    throw std::out_of_range("store index out of range: " +
                            std::to_string(index));
  }
  return *stores[index].store;
}

const Store &Session::store(const std::size_t index) const {
  if (index >= stores.size()) {
    throw std::out_of_range("store index out of range: " +
                            std::to_string(index));
  }
  return *stores[index].store;
}

const std::string &Session::path(const std::size_t index) const {
  if (index >= stores.size()) {
    static const std::string empty;
    return empty;
  }
  return stores[index].path;
}

bool Session::isTemporaryStore(const std::size_t index) const {
  return index < stores.size() && stores[index].isTemporary;
}

void Session::setStorePath(const std::size_t index, std::string newPath,
                           const bool isTemporary) {
  if (index < stores.size()) {
    stores[index].path        = std::move(newPath);
    stores[index].isTemporary = isTemporary;
  }
}

void Session::loadRetainedScrolls(const Store &store,
                                  const std::string &storePath) {
  for (const auto &scroll : store.scrolls()) {
    for (const auto &segment : scroll.segments) {
      const auto root = std::filesystem::path(storePath) / "published" /
                        segment.torrent.hex();
      const auto metainfo = root / "metainfo.torrent";
      if (!std::filesystem::exists(metainfo)) continue;
      std::ifstream in(metainfo, std::ios::binary);
      if (!in) throw std::runtime_error("cannot read retained media metainfo");
      const std::string encoded{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
      if (Metainfo::parse(encoded).hash() != segment.torrent)
        throw std::runtime_error("retained media metainfo hash mismatch");
      contentSource.add(encoded, root.string());
    }
  }
}

std::size_t Session::addStore(std::unique_ptr<Store> aStore, std::string aPath,
                              const bool aIsTemporary) {
  if (readerLinkPackages().containsAuthority(aStore->documentId()))
    throw std::invalid_argument(
        "Store authority conflicts with a retained package");
  loadRetainedScrolls(*aStore, aPath);
  if (swarmSource) {
    aStore->setContentSource(swarmSource.get());
  } else {
    aStore->setContentSource(&contentSource);
  }
  const auto opsNow = aStore->opCount();
  stores.push_back(StoreEntry{.store         = std::move(aStore),
                              .path          = std::move(aPath),
                              .isTemporary   = aIsTemporary,
                              .opsWhenOpened = opsNow});
  return stores.size() - 1U;
}

std::pair<std::size_t, MicroversionId>
Session::importFileToTemporaryStore(const std::string &filePath) {
  namespace fs       = std::filesystem;
  const auto tempDir = untitledStoreDir(fs::path(filePath).stem().string());

  auto perma    = (stores.empty() || !stores[0].store)
                      ? nullptr
                      : stores[0].store->userPermascrollPtr();
  auto newStore = std::make_unique<Store>(perma);
  const gleditor::FileTextSource source(filePath);
  const auto docName      = std::filesystem::path(filePath).stem().string();
  MicroversionId imported = newStore->makeXanadoc(
      MicroversionId{}, docName.empty() ? "document" : docName);
  std::uint32_t at = 0;
  // Indexed by piece position, parallel to source.pieces(): the span each
  // piece landed at, so a later piece naming an earlier one via
  // duplicateOfPieceIndex (a PDF figure repeated across pages) can be
  // inserted via insertSpan() -- pointing at the bytes already stored --
  // instead of appending its own copy through insertMedia().
  std::vector<PrimediaSpan> insertedSpans;
  const auto pieces = source.pieces();
  insertedSpans.reserve(pieces.size());
  for (const auto &piece : pieces) {
    PrimediaSpan span;
    if (piece.duplicateOfPieceIndex.has_value() &&
        *piece.duplicateOfPieceIndex < insertedSpans.size()) {
      span     = insertedSpans[*piece.duplicateOfPieceIndex];
      imported = newStore->insertSpan(imported, at, span);
    } else if (piece.mimeType.empty()) {
      imported = newStore->insert(imported, at, piece.bytes);
    } else {
      const auto fileName =
          "fig_" + std::to_string(insertedSpans.size()) + ".dat";
      const auto scroll = retainMediaScroll(
          piece.bytes, fileName, piece.mimeType, tempDir / "published");
      const auto sId = newStore->addScroll(scroll);
      span =
          PrimediaSpan{.scroll = sId, .start = 0, .length = piece.bytes.size()};
      Op op;
      op.kind  = OpKind::Transclude;
      op.at    = at;
      op.span  = span;
      imported = newStore->apply(imported, op);
    }
    insertedSpans.push_back(span);
    at += static_cast<std::uint32_t>(piece.bytes.size());
    if (piece.pageBreakAfter) {
      imported = newStore->insertBreak(imported, at);
    }
  }
  newStore->save(tempDir.string());

  const auto idx = addStore(std::move(newStore), tempDir.string(), true);
  return {idx, imported};
}

std::size_t Session::loadAuxiliaryStore(const std::string &aPath) {
  auto perma    = (stores.empty() || !stores[0].store)
                      ? nullptr
                      : stores[0].store->userPermascrollPtr();
  auto newStore = std::make_unique<Store>(perma);
  newStore->load(aPath);
  return addStore(std::move(newStore), aPath, false);
}

std::size_t Session::createNewStore(const std::string &aPath) {
  namespace fs          = std::filesystem;
  std::string targetDir = aPath;
  bool isTemporary      = false;
  if (targetDir.empty()) {
    targetDir   = untitledStoreDir("untitled").string();
    isTemporary = true;
  } else {
    fs::create_directories(targetDir);
  }

  auto perma    = (stores.empty() || !stores[0].store)
                      ? nullptr
                      : stores[0].store->userPermascrollPtr();
  auto newStore = std::make_unique<Store>(perma);
  newStore->makeXanadoc(MicroversionId{}, "document");
  newStore->save(targetDir);
  return addStore(std::move(newStore), targetDir, isTemporary);
}

void Session::save(const std::size_t index) const {
  const_cast<Session *>(this)->flushUncommitted();
  if (index < stores.size() && stores[index].store &&
      !stores[index].path.empty()) {
    stores[index].store->save(stores[index].path);
  }
}

void Session::syncCurrentVersions(const std::size_t storeIndex) const {
  if (storeIndex >= stores.size() || !stores[storeIndex].store) {
    return;
  }

  std::vector<MicroversionId> visible;
  for (const auto &view : open) {
    if (view.storeIndex != storeIndex || view.version.isZero()) {
      continue;
    }
    if (std::ranges::find(visible, view.version) == visible.end()) {
      visible.push_back(view.version);
    }
  }
  if (visible.empty()) {
    const auto latest = stores[storeIndex].store->latest();
    if (!latest.isZero()) {
      visible.push_back(latest);
    }
  }
  if (!visible.empty()) {
    stores[storeIndex].store->setCurrentVersions(std::move(visible));
  }
}

Store *Session::activity() {
  if (activityStore || activityRefused) {
    return activityStore.get();
  }
  const auto dir = xanadu::activityDirectory();
  auto perma     = (stores.empty() || !stores[0].store)
                       ? nullptr
                       : stores[0].store->userPermascrollPtr();
  auto opened    = std::make_unique<Store>(perma);
  try {
    if (std::filesystem::exists(dir)) {
      opened->load(dir.string());
    }
    activityStore = std::move(opened);
  } catch (const std::exception &err) {
    activityRefused = true;
    std::cerr << "xudu: not resuming or recording where you were: the "
                 "activity store at "
              << dir.string() << " cannot be read (" << err.what()
              << "); it is left untouched\n";
  }
  return activityStore.get();
}

std::optional<xanadu::ReadingPlace> Session::lastPlace() {
  auto *const store = activity();
  if (nullptr == store) {
    return std::nullopt;
  }
  try {
    return xanadu::latestPlace(*store);
  } catch (const std::exception &err) {
    std::cerr << "xudu: cannot read where you were: " << err.what() << "\n";
    return std::nullopt;
  }
}

void Session::rememberPlace(const xanadu::ReadingPlace &place) {
  auto *const store = activity();
  if (nullptr == store) {
    return;
  }
  std::ignore    = xanadu::recordPlace(*store, place);
  const auto dir = xanadu::activityDirectory();
  std::filesystem::create_directories(dir);
  store->save(dir.string());
}

void Session::saveAll() const {
  const_cast<Session *>(this)->flushUncommitted();
  for (std::size_t i = 0; i < stores.size(); ++i) {
    save(i);
  }
}

MicroversionId Session::versionOf(const std::uint32_t docIndex) const {
  if (docIndex < open.size() && !open[docIndex].uncommittedLog.empty()) {
    const_cast<Session *>(this)->flushUncommitted(docIndex);
  }
  return docIndex < open.size() ? open[docIndex].version : MicroversionId{};
}

std::size_t Session::storeIndexOf(const std::uint32_t docIndex) const {
  return docIndex < open.size() ? open[docIndex].storeIndex : 0U;
}

std::optional<MicroversionId>
Session::versionShowing(const std::vector<PrimediaSpan> &ends,
                        const std::vector<MicroversionId> &except) const {
  if (ends.empty()) {
    return std::nullopt;
  }
  for (const auto &entry : stores) {
    if (!entry.store) {
      continue;
    }
    auto candidates = entry.store->allVersions();
    for (const auto &id : std::ranges::reverse_view(candidates)) {
      if (std::ranges::find(except, id) != except.end()) {
        continue;
      }
      const auto pieces = entry.store->rebuild(id);
      for (const auto &span : ends) {
        if (!pieces.occurrencesOf(span).empty()) {
          return id;
        }
      }
    }
  }
  return std::nullopt;
}

void Session::viewOpened(const MicroversionId &version,
                         const std::size_t storeIndex,
                         const std::uint32_t focusedBirth) {
  const auto &st = store(storeIndex);
  const auto targetBirth =
      (focusedBirth != 0) ? focusedBirth : st.activeXanadocOnBranch(version);
  std::vector<std::uint32_t> path;
  if (targetBirth != 0) {
    path = st.containmentPath(targetBirth);
  }
  open.push_back(OpenView{.version         = version,
                          .storeIndex      = storeIndex,
                          .focusedBirth    = targetBirth,
                          .containmentPath = std::move(path),
                          .pieces          = st.rebuild(version, targetBirth),
                          .decorations     = {},
                          .decoratedAt     = 0,
                          .uncommittedLog  = {}});
  invalidate();
}

void Session::viewClosed(const std::uint32_t docIndex) {
  if (docIndex >= open.size()) {
    return;
  }
  flushUncommitted(docIndex);
  open.erase(open.begin() + static_cast<std::ptrdiff_t>(docIndex));
  invalidate();
}

std::optional<FocusTarget>
Session::focusTargetForView(const std::size_t docIndex) const {
  if (docIndex >= open.size()) {
    return std::nullopt;
  }
  const auto &view = open[docIndex];
  const auto &st   = store(view.storeIndex);
  if (view.focusedBirth != 0) {
    if (!st.validateContainment(view.focusedBirth)) {
      return std::nullopt;
    }
    const auto kind = st.structureKindOfOp(view.focusedBirth);
    return FocusTarget{
        .kind            = kind,
        .birthOp         = view.focusedBirth,
        .containmentPath = view.containmentPath,
    };
  }
  const auto activeXanadoc = st.activeXanadocOnBranch(view.version);
  if (activeXanadoc != 0 && st.validateContainment(activeXanadoc)) {
    return FocusTarget{
        .kind            = StructureKind::Xanadoc,
        .birthOp         = activeXanadoc,
        .containmentPath = st.containmentPath(activeXanadoc),
    };
  }
  return FocusTarget{
      .kind            = StructureKind::Xanadoc,
      .birthOp         = 0,
      .containmentPath = {},
  };
}

void Session::setFocusTarget(const std::size_t docIndex,
                             const std::uint32_t birthOp) {
  if (docIndex >= open.size()) {
    return;
  }
  auto &view     = open[docIndex];
  const auto &st = store(view.storeIndex);
  if (birthOp == 0) {
    view.focusedBirth = 0;
    view.containmentPath.clear();
    return;
  }
  if (st.validateContainment(birthOp)) {
    view.focusedBirth    = birthOp;
    view.containmentPath = st.containmentPath(birthOp);
  }
}

std::size_t Session::systemStoreIndex(const SystemDocKind kind) {
  const auto it = systemStoreIndices_.find(kind);
  if (it != systemStoreIndices_.end()) {
    return it->second;
  }

  const auto dir = systemDocDirectory(kind);
  std::filesystem::create_directories(dir);

  auto perma    = (stores.empty() || !stores[0].store)
                      ? nullptr
                      : stores[0].store->userPermascrollPtr();
  auto sysStore = std::make_unique<Store>(perma);
  sysStore->setSystem(true);

  bool opened = false;
  if (std::filesystem::exists(dir / "ops.nodes") ||
      std::filesystem::exists(dir / "store.tables")) {
    // Every typed refusal a store shape can produce, because they all mean the
    // same thing here. Listed rather than caught as their common base: this
    // recovers from "the file is not what this build reads", not from a bug in
    // reading a file that is.
    std::string refusal;
    try {
      sysStore->load(dir.string());
      opened = true;
    } catch (const xanadu::OpsSegmentUnreadable &e) {
      refusal = e.what();
    } catch (const xanadu::StoreTablesUnreadable &e) {
      refusal = e.what();
    }
    if (!refusal.empty()) {
      // A system xanadoc is scaffolding this program writes for itself, and
      // the branch below already knows how to make one from nothing. So a
      // system store in a shape this build cannot read means the same thing
      // as one that is not there, and refusing to start over it would make
      // every earlier config a reason the program will not open at all.
      //
      // A document the *user* named is the opposite case and is left to fail
      // loudly, which is what R11 asks for: nobody can regenerate that one.
      //
      // Moved aside rather than written over. These hold whatever the user
      // changed through the UI -- their keymap, their settings -- and this
      // program did not write the bytes it is about to replace.
      auto aside = dir;
      aside += ".unreadable";
      for (int n = 1; std::filesystem::exists(aside); n++) {
        aside = dir;
        aside += ".unreadable." + std::to_string(n);
      }
      std::error_code moved;
      std::filesystem::rename(dir, aside, moved);
      std::cerr << std::format(
          "xudu [warning]: the system {} xanadoc could not be read ({}). It "
          "has been moved to {} and a default one written in its place.\n",
          systemDocName(kind), refusal, aside.string());
      std::filesystem::create_directories(dir);
      sysStore = std::make_unique<Store>(perma);
      sysStore->setSystem(true);
    }
  }
  if (opened) {
    if (sysStore->currentVersions().empty() && !sysStore->latest().isZero()) {
      sysStore->repointCurrentVersion(sysStore->latest());
    }
    if (kind == SystemDocKind::UI) {
      const auto before   = sysStore->primaryCurrentVersion();
      const auto manifold = sysStore->rebuildManifold(before);
      // An explicit document permascroll may differ from the one that wrote
      // these settings. Never mint replacements for names we cannot resolve.
      const auto groups = manifold.dimensionNamed(kDimGroups, *sysStore);
      const bool readable =
          groups &&
          manifold.linked(sysStore->homeCell(), *groups) != zigzag::noCell &&
          std::ranges::all_of(
              std::array{kDimVars, kDimValues, kDimSubgroups, kDimClone,
                         kDimNotes, kDimSchemas, kDimAlternates, kDimDefault},
              [&](std::string_view name) {
                return manifold.dimensionNamed(name, *sysStore).has_value();
              });
      if (readable) {
        const auto updated = ensureAllSettings(*sysStore, before, kind);
        if (updated != before) {
          sysStore->repointCurrentVersion(updated);
          sysStore->save(dir.string());
        }
      } else {
        GLEDITOR_LOG_WARN("xudu.settings",
                          "UI settings structure cannot be resolved with the "
                          "active permascroll; leaving the store unchanged");
      }
    }
  } else {
    // Fresh system store: initialize 3-page store with schema, notes, and
    // format links
    initializeSystemStore(*sysStore, kind);
    sysStore->save(dir.string());
  }

  const auto sIdx = addStore(std::move(sysStore), dir.string(), false);
  systemStoreIndices_[kind] = sIdx;
  return sIdx;
}

Store &Session::systemStore(const SystemDocKind kind) {
  return store(systemStoreIndex(kind));
}

const Store &Session::systemStore(const SystemDocKind kind) const {
  const auto it = systemStoreIndices_.find(kind);
  if (it != systemStoreIndices_.end()) {
    return store(it->second);
  }
  return const_cast<Session *>(this)->systemStore(kind);
}

std::optional<SystemDocKind>
Session::systemDocKindForStoreIndex(const std::size_t storeIndex) const {
  for (const auto &[kind, sIdx] : systemStoreIndices_) {
    if (sIdx == storeIndex) {
      return kind;
    }
  }
  if (storeIndex < stores.size() && stores[storeIndex].store &&
      stores[storeIndex].store->isSystem()) {
    for (std::uint8_t k = 0;
         k < static_cast<std::uint8_t>(SystemDocKind::Count); ++k) {
      const auto kind = static_cast<SystemDocKind>(k);
      if (stores[storeIndex].path == systemDocDirectory(kind).string() ||
          stores[storeIndex].path == systemDocUri(kind)) {
        return kind;
      }
    }
  }
  return std::nullopt;
}

void Session::repointSystemDoc(const SystemDocKind kind,
                               const MicroversionId &version) {
  const auto sIdx = systemStoreIndex(kind);
  auto &st        = store(sIdx);
  st.repointCurrentVersion(version);
  st.save(stores[sIdx].path);
  // Also update any open view showing this system store
  for (std::size_t i = 0; i < open.size(); ++i) {
    if (open[i].storeIndex == sIdx) {
      refresh(static_cast<std::uint32_t>(i), version);
    }
  }
  if (systemDocChangedCallback_) {
    systemDocChangedCallback_(kind, st);
  }
}

MicroversionId Session::openSystemDoc(const SystemDocKind kind) {
  const auto sIdx = systemStoreIndex(kind);
  auto &st        = store(sIdx);
  auto ver        = st.primaryCurrentVersion();
  if (ver.isZero()) {
    ver = st.latest();
  }
  for (auto &i : open) {
    if (i.storeIndex == sIdx) {
      return i.version;
    }
  }
  viewOpened(ver, sIdx);
  return ver;
}

void Session::setSystemDocPublished(const SystemDocKind kind,
                                    const bool published) {
  if (published) {
    publishedSystemDocs_.insert(kind);
  } else {
    publishedSystemDocs_.erase(kind);
  }
}

bool Session::isSystemDocPublished(const SystemDocKind kind) const {
  return publishedSystemDocs_.contains(kind);
}

std::vector<PublishedHoleRecord> Session::collectWithheldHoles() const {
  std::vector<PublishedHoleRecord> holes;
  for (std::size_t sIdx = 0; sIdx < stores.size(); ++sIdx) {
    const auto &entry = stores[sIdx];
    if (!entry.store || !entry.store->isSystem()) {
      continue;
    }
    const auto kindOpt = systemDocKindForStoreIndex(sIdx);
    if (kindOpt && publishedSystemDocs_.contains(*kindOpt)) {
      continue;
    }
    const auto opCount = entry.store->opCount();
    for (std::uint32_t i = 1; i <= opCount; ++i) {
      const auto *node = entry.store->getCompactOp(i);
      if (node && node->scrollId == localScroll && node->spanLength > 0) {
        holes.push_back(PublishedHoleRecord{
            .at     = node->spanStart,
            .length = node->spanLength,
            .reason = HoleReason::Withheld,
        });
      }
    }
  }

  if (holes.empty()) {
    return holes;
  }

  std::ranges::sort(holes,
                    [](const auto &a, const auto &b) { return a.at < b.at; });

  std::vector<PublishedHoleRecord> merged;
  merged.reserve(holes.size());
  for (const auto &hole : holes) {
    if (merged.empty()) {
      merged.push_back(hole);
      continue;
    }
    auto &last = merged.back();
    if (hole.at <= last.at + last.length) {
      last.length = std::max(last.length, (hole.at + hole.length) - last.at);
    } else {
      merged.push_back(hole);
    }
  }
  return merged;
}

void Session::refresh(const std::uint32_t docIndex,
                      const MicroversionId &version) {
  if (docIndex >= open.size()) {
    return;
  }
  const auto sIdx = open[docIndex].storeIndex;
  const auto &st  = store(sIdx);
  // The document this view is already showing is the one the edit was made
  // to, so the usual case is one operation away and the pieces in hand are
  // most of the answer. Only a move that is not one step on -- travelling in
  // hypertime, or several edits recorded before anything asked to see them --
  // has to replay the history from the null document.
  const auto birth = open[docIndex].focusedBirth;
  if (!st.advance(open[docIndex].pieces, open[docIndex].version, version,
                  birth)) {
    open[docIndex].pieces = st.rebuild(version, birth);
  }
  open[docIndex].version = version;
  invalidate();
}

namespace {

/// A box shown at its own @p naturalWidth x @p naturalHeight -- one image
/// pixel to one layout pixel, the same convention every raster format here
/// already uses (see design/svg-vector-primedia.md's own note that a static
/// SVG rasterizes once at its intrinsic size for this exact reason) -- unless
/// that would stand wider than @p maxWidth or taller than @p maxHeight, in
/// which case it is shrunk, preserving aspect, until it fits. Never enlarged
/// past its natural size: a 64x64 icon stays 64x64 layout pixels (tiny next
/// to an 1188-wide page) rather than being blown up to fill the page the way
/// stretching every image to @p maxWidth regardless of its own resolution
/// used to.
///
/// Shared by every media kind with real pixel dimensions (images directly;
/// video via videoFitSize(), which fits the viewport here before adding back
/// the chrome drawn outside it) so "fit the page, preserve aspect, never
/// upscale" is made exactly once rather than once per kind, each
/// independently reachable to drift from the others.
struct ImageFitSize {
  float width;
  float height;
};

ImageFitSize fitWithinBox(const float naturalWidth, const float naturalHeight,
                          const float maxWidth, const float maxHeight) {
  float width  = naturalWidth;
  float height = naturalHeight;
  if (width > maxWidth) {
    height *= maxWidth / width;
    width = maxWidth;
  }
  if (height > maxHeight) {
    width *= maxHeight / height;
    height = maxHeight;
  }
  return {.width = width, .height = height};
}

/// Both placeholderFor() (how much room to reserve) and ImageOverlay::place()
/// (how big to actually draw it) must call this and agree, for the same
/// reason they must agree on everything else here: a placeholder sized one
/// way and a widget sized another either overlaps the text that follows or
/// leaves a gap before it.
ImageFitSize imageFitSize(const float naturalWidth, const float naturalHeight) {
  return fitWithinBox(naturalWidth, naturalHeight, Doc::textWidthPx,
                      Doc::textHeightPx);
}

/// A modest stand-in natural size for a video whose real one could not be
/// read -- an unsupported container, or a build without libav
/// (GLEDITOR_HAVE_DECODE_INDEX_LIBAV) -- at MediaWidget::defaultAspect, the
/// same fallback MediaPlayer::aspectRatio() itself uses. Not the page's full
/// text width: with no real dimensions to go on, guessing a modest video
/// resolution is closer to what most video actually is than assuming it
/// wants to fill the entire page.
constexpr float kFallbackVideoNaturalWidthPx = 480.0F;

/// The real pixel dimensions of a video file's own bytes, from its
/// container's stream metadata (gleditor::peekVideoSize() -- no frame
/// decode, so this is cheap enough to call from both placeholderFor() and
/// mediaSpansFor() without either caching or sharing the result between
/// them) -- or the fallback above, when the container is not one FFmpeg
/// recognises or carries no video stream this build was compiled to read.
std::pair<float, float>
videoNaturalSizeFor(const std::span<const std::uint8_t> bytes) {
  const auto size = gleditor::peekVideoSize(bytes);
  if (size && size->first > 0 && size->second > 0) {
    return {static_cast<float>(size->first), static_cast<float>(size->second)};
  }
  return {kFallbackVideoNaturalWidthPx,
          kFallbackVideoNaturalWidthPx / gleditor::MediaWidget::defaultAspect};
}

/// The whole video card's size at @p naturalWidth x @p naturalHeight: the
/// viewport shown at its own resolution (never enlarged, same "1 pixel = 1
/// layout pixel" rule imageFitSize() follows) unless that would not fit the
/// page's text width or remaining height once the chrome MediaWidget draws
/// outside the viewport (title bar, transport, seek bar) is set aside, then
/// chromeHeightPx added back for the card as a whole. Without reserving
/// chromeHeightPx *before* fitting, a video whose aspect makes it want the
/// full remaining page height would leave no room for the chrome drawn
/// below it, pushing the card's true height past what was reserved.
ImageFitSize videoFitSize(const float naturalWidth, const float naturalHeight) {
  const auto viewport =
      fitWithinBox(naturalWidth, naturalHeight, Doc::textWidthPx,
                   Doc::textHeightPx - gleditor::MediaWidget::chromeHeightPx);
  return {.width  = viewport.width,
          .height = viewport.height + gleditor::MediaWidget::chromeHeightPx};
}

/// The widget size for one media span, by MIME type: an image or SVG fit at
/// its own resolution (imageFitSize()), a video card fit at its own
/// resolution (videoFitSize()), or the fixed audio card size. The one place
/// this decision is made -- placeholderFor() (how much room to reserve),
/// mediaSpansFor() (what to construct the widget at), and ImageOverlay::
/// place() (images only, via imageFitSize() directly since it already has a
/// decoded ImageResource in hand and would otherwise decode the same bytes
/// twice) all end up at the same answer because they all either call this or
/// its own imageFitSize()/videoFitSize() halves, rather than each keeping an
/// independent copy of "how big is this."
ImageFitSize mediaFitFor(const std::span<const std::uint8_t> bytes,
                         const std::string_view mime) {
  if (gleditor::MimeType{mime} == gleditor::MimeType::ImageSvg) {
    // No rasterization needed just to size this -- SvgCache's own GPU
    // texture and GL/SwCanvas rendering are for ImageOverlay::place() to
    // set up once the span is actually drawn.
    const auto size         = gleditor::SvgCache::peekSize(bytes);
    const auto [natW, natH] = size.value_or(std::make_pair(1.0F, 1.0F));
    if (gleditor::SvgAnimator::isAnimated(bytes)) {
      return videoFitSize(natW, natH);
    }
    return imageFitSize(natW, natH);
  }
  if (mime == "image/gif" && gleditor::isAnimatedGif(bytes)) {
    const auto size  = gleditor::peekGifSize(bytes);
    const float natW = size ? static_cast<float>(size->first) : 1.0F;
    const float natH = size ? static_cast<float>(size->second) : 1.0F;
    return videoFitSize(natW, natH);
  }
  if (gleditor::MagicMimeDetector::isImageMime(mime)) {
    // An image that will not decode still gets a placeholder, square.
    const auto natural =
        gleditor::decodeImageBuffer(bytes, gleditor::MimeType{mime})
            .transform([](const gleditor::DecodedImage &decoded) {
              return std::pair{static_cast<float>(decoded.width),
                               static_cast<float>(decoded.height)};
            })
            .value_or(std::pair{1.0F, 1.0F});
    return imageFitSize(natural.first, natural.second);
  }
  if (gleditor::MagicMimeDetector::isVideoMime(mime)) {
    const auto [natW, natH] = videoNaturalSizeFor(bytes);
    return videoFitSize(natW, natH);
  }
  return {.width  = Session::audioCardWidthPx,
          .height = Session::audioCardHeightPx};
}

/// What sourceFor() anchors one media span's LayoutBox to in a document's
/// concatext: a single OBJECT REPLACEMENT CHARACTER, 3 bytes in UTF-8, so a
/// widget's own pixel size no longer decides how many bytes of the document
/// it occupies -- only LayoutBox::widthPx/heightPx do, which the layout
/// engine turns into reserved space once it knows the font it is flowing
/// into. sourceFor() and mediaSpansFor() both anchor at this same fixed
/// width, which is what lets mediaSpansFor()'s docOffset bookkeeping stay in
/// step with sourceFor()'s concatext without either recomputing anything
/// about the other.
constexpr std::string_view kMediaAnchor = "\xEF\xBF\xBC";

/// One stretch of a piece, classified as plain text or media. See
/// classifyRun() for why a piece can hold more than one of these.
struct ClassifiedStretch {
  bool isMedia{false};
  std::uint64_t start{}; ///< Scroll-relative, same coordinates as the piece.
  std::uint64_t length{};
  std::string mime;                ///< Set only when isMedia.
  std::uint64_t containerStart{};  ///< Scroll-relative start of the whole
                                   ///< file this stretch belongs to. Set only
                                   ///< when isMedia.
  std::uint64_t containerLength{}; ///< That file's own total length.
};

/// Splits [@p run.start, @p run.start + @p run.length) into stretches of
/// plain text and media, resolving each media stretch to the whole file it
/// was cut from -- via Store::segmentsOverlapping(), which is what
/// Store::insertMedia() populated -- rather than MIME-sniffing the piece's
/// own bytes outright.
///
/// This is what makes a fragment transcluded out of the middle of a media
/// file still classify correctly: cut loose from its container it carries no
/// header of its own for libmagic to recognise (a PNG's IDAT bytes, a WAV's
/// PCM samples with no RIFF chunk in front of them), so sniffing the
/// fragment's own bytes is exactly the failure Gap E named. Resolving by
/// address instead means the fragment's *origin* is what gets classified,
/// which still has its header, regardless of what got cut out of it.
///
/// A run can also straddle a segment boundary -- typed text immediately
/// following a media file in the same scroll coalesces into one piece (see
/// Store::insertMedia()'s own comment) -- so this walks every segment
/// overlapping the run rather than assuming one answer for the whole thing.
/// Any stretch no segment covers falls back to sniffing its own bytes
/// directly, which is what makes this correct for content from before
/// segments existed as well as for genuinely plain typed text.
std::vector<ClassifiedStretch> classifyRun(const Store &st,
                                           const PrimediaSpan &run,
                                           gleditor::MagicMimeDetector &magic) {
  std::vector<ClassifiedStretch> out;
  const auto runEnd = run.start + run.length;

  const auto classifyPlain = [&](const std::uint64_t start,
                                 const std::uint64_t length) {
    if (0 == length) {
      return;
    }
    const auto bytes = st.read(
        PrimediaSpan{.scroll = run.scroll, .start = start, .length = length});
    const auto mime = magic.identifyBuffer(bytes.data(), bytes.size());
    if (gleditor::MagicMimeDetector::isMediaMime(mime)) {
      out.push_back(ClassifiedStretch{.isMedia         = true,
                                      .start           = start,
                                      .length          = length,
                                      .mime            = mime,
                                      .containerStart  = start,
                                      .containerLength = length});
    } else {
      out.push_back(ClassifiedStretch{.isMedia         = false,
                                      .start           = start,
                                      .length          = length,
                                      .mime            = "text/plain",
                                      .containerStart  = start,
                                      .containerLength = length});
    }
  };

  std::uint64_t cursor = run.start;
  for (const auto &segment :
       st.segmentsOverlapping(run.scroll, run.start, run.length)) {
    if (segment.at > cursor) {
      classifyPlain(cursor, segment.at - cursor);
      cursor = segment.at;
    }
    const auto mediaEnd = std::min(runEnd, segment.end());
    if (mediaEnd > cursor) {
      const bool isMedia =
          gleditor::MagicMimeDetector::isMediaMime(segment.mimeType);
      if (isMedia) {
        out.push_back(ClassifiedStretch{.isMedia         = true,
                                        .start           = cursor,
                                        .length          = mediaEnd - cursor,
                                        .mime            = segment.mimeType,
                                        .containerStart  = segment.at,
                                        .containerLength = segment.length});
      } else {
        out.push_back(ClassifiedStretch{.isMedia         = false,
                                        .start           = cursor,
                                        .length          = mediaEnd - cursor,
                                        .mime            = segment.mimeType,
                                        .containerStart  = segment.at,
                                        .containerLength = segment.length});
      }
      cursor = mediaEnd;
    }
  }
  if (cursor < runEnd) {
    classifyPlain(cursor, runEnd - cursor);
  }
  return out;
}

} // namespace

std::shared_ptr<VersionTextSource>
Session::sourceFor(const MicroversionId &version, const std::size_t storeIndex,
                   const std::uint32_t scopedBirth) const {
  const auto &st     = store(storeIndex);
  const auto rebuilt = st.rebuild(version, scopedBirth);
  gleditor::MagicMimeDetector magic;

  std::string concatext;
  const auto breaks = rebuilt.forcedBreaks();
  std::vector<gleditor::LayoutBox> boxes;
  std::vector<gleditor::BlockStyleRange> blockStyles;
  std::uint32_t nextBoxId = 0;

  for (const auto &run : rebuilt.pieces()) {
    if (breakMarkerScroll == run.scroll) {
      continue;
    }
    for (const auto &stretch : classifyRun(st, run, magic)) {
      if (!stretch.isMedia) {
        const auto span = PrimediaSpan{.scroll = run.scroll,
                                       .start  = stretch.start,
                                       .length = stretch.length};
        const auto res  = st.resolve(span);
        if (res.status == ResolutionStatus::VerifiedBytes) {
          concatext += res.text;
        } else if (res.status == ResolutionStatus::TranscopyrightLocked ||
                   res.status == ResolutionStatus::WithheldRedacted) {
          concatext.append(stretch.length > 0 ? stretch.length : 1U, ' ');
        } else {
          concatext += st.read(span);
        }
        continue;
      }
      // The whole container's bytes, not just this stretch: a fragment of a
      // compressed image or media file cannot be sized (or, for images,
      // meaningfully shown at all -- see ImageOverlay's own container
      // fallback) from a slice of it alone.
      const auto containerBytes =
          st.read(PrimediaSpan{.scroll = run.scroll,
                               .start  = stretch.containerStart,
                               .length = stretch.containerLength});
      const std::span<const std::uint8_t> containerSpan(
          reinterpret_cast<const std::uint8_t *>(containerBytes.data()),
          containerBytes.size());
      const auto fit = mediaFitFor(containerSpan, stretch.mime);

      // An image narrow enough to leave a usable column beside it floats, so
      // text wraps there instead of stepping over it; audio, video and
      // animations stay Block regardless of width, since their transport chrome
      // (play/pause, seek bar, title) wants the full column to itself, not a
      // half-width sliver squeezed beside text. kFloatWidthFraction is "at most
      // half the page" -- narrower than that and there is nothing left worth
      // wrapping text into.
      const bool isAnim =
          (stretch.mime == "image/gif" &&
           gleditor::isAnimatedGif(containerSpan)) ||
          (gleditor::MimeType{stretch.mime} == gleditor::MimeType::ImageSvg &&
           gleditor::SvgAnimator::isAnimated(containerSpan));
      const bool isImage =
          !isAnim && gleditor::MagicMimeDetector::isImageMime(stretch.mime);
      constexpr float kFloatWidthFraction = 0.5F;
      const bool floats =
          isImage && fit.width <= Doc::textWidthPx * kFloatWidthFraction;

      const auto anchorOffset = static_cast<std::uint32_t>(concatext.size());
      boxes.push_back(gleditor::LayoutBox{
          .anchor    = anchorOffset,
          .widthPx   = fit.width,
          .heightPx  = fit.height,
          .marginPx  = gleditor::MediaWidget::anchorGapPx,
          .placement = floats ? gleditor::BoxPlacement::FloatLeft
                              : gleditor::BoxPlacement::Block,
          .id        = nextBoxId++,
      });
      // Centre alignment only matters for a Block box -- a Float box's left
      // edge is fixed by placeFloat() against whichever side it floats to,
      // not by BlockStyleRange::align, so a floated figure gets no entry
      // here at all.
      if (!floats) {
        blockStyles.push_back(gleditor::BlockStyleRange{
            .start = anchorOffset,
            .end =
                anchorOffset + static_cast<std::uint32_t>(kMediaAnchor.size()),
            .align = gleditor::TextAlign::Centre,
        });
      }
      concatext += kMediaAnchor;
    }
  }

  // Extract presentation formatting and paragraph alignment from Format links
  const FormatResolver formatResolver(st);
  auto formattingResult = formatResolver.resolveVersion(rebuilt);
  std::vector<gleditor::DecoratedRange> decoratedRanges =
      std::move(formattingResult.decoratedRanges);
  blockStyles.insert(
      blockStyles.end(),
      std::make_move_iterator(formattingResult.blockStyles.begin()),
      std::make_move_iterator(formattingResult.blockStyles.end()));

  std::string title;
  if (scopedBirth != 0) {
    title = st.resolveStructureName(version, scopedBirth);
  }
  if (title.empty() && st.isSystem()) {
    if (const auto kind = systemDocKindForStoreIndex(storeIndex)) {
      title = std::string(systemDocUri(*kind));
    }
  }
  if (title.empty() && !version.isZero()) {
    if (const auto ann = st.versionAnnotation(version);
        ann && !ann->alias.empty()) {
      title = ann->alias;
    }
  }
  if (title.empty() && scopedBirth != 0) {
    title =
        (st.structureKindOfOp(scopedBirth) == StructureKind::Slice ? "Slice "
                                                                   : "Doc ") +
        std::to_string(scopedBirth);
  }
  // The store's own name, which does not change as it is edited. The version
  // name used to stand in, so a tab read "1", then "3" once a transclusion
  // reloaded it. An untitled store has no name of its own yet; the tab bar
  // numbers those.
  if (title.empty() && !isTemporaryStore(storeIndex)) {
    const auto named =
        std::filesystem::path(path(storeIndex)).lexically_normal();
    auto base = named.filename().string();
    if (base.empty()) {
      base = named.parent_path().filename().string();
    }
    if (!base.starts_with("untitled-")) {
      title = std::move(base);
    }
  }
  // Two views of one store on different branches would read alike; the
  // branch -- the version name without its last number -- tells them apart
  // and, unlike the version, stays put while either is edited.
  if (const auto branch = branchOf(version); !branch.empty()) {
    title = (title.empty() ? std::string{"Untitled"} : title) + " · " + branch;
  }

  auto target =
      std::make_shared<render::PickSemanticTarget>(render::PickSemanticTarget{
          .documentId = st.documentId().str(), .microversion = version.str()});
  return std::make_shared<VersionTextSource>(
      concatext, version, breaks, boxes, blockStyles, title, decoratedRanges,
      std::move(target));
}

std::vector<Session::MediaSpanInfo>
Session::mediaSpansFor(const MicroversionId &version,
                       const std::size_t storeIndex) const {
  if (storeIndex >= stores.size() || !stores[storeIndex].store) {
    return {};
  }
  const auto &st     = store(storeIndex);
  const auto rebuilt = st.rebuild(version);
  gleditor::MagicMimeDetector magic;

  std::vector<MediaSpanInfo> list;
  std::uint32_t docOffset = 0;

  for (const auto &run : rebuilt.pieces()) {
    if (breakMarkerScroll == run.scroll) {
      continue;
    }
    for (const auto &stretch : classifyRun(st, run, magic)) {
      if (!stretch.isMedia) {
        docOffset += static_cast<std::uint32_t>(stretch.length);
        continue;
      }
      const auto containerBytes =
          st.read(PrimediaSpan{.scroll = run.scroll,
                               .start  = stretch.containerStart,
                               .length = stretch.containerLength});
      const std::span<const std::uint8_t> containerSpan(
          reinterpret_cast<const std::uint8_t *>(containerBytes.data()),
          containerBytes.size());

      MediaSpanInfo info;
      info.span      = PrimediaSpan{.scroll = run.scroll,
                                    .start  = stretch.start,
                                    .length = stretch.length};
      info.docOffset = docOffset;
      info.mime      = stretch.mime;
      info.isAudio   = gleditor::MagicMimeDetector::isAudioMime(stretch.mime);
      info.isVideo   = gleditor::MagicMimeDetector::isVideoMime(stretch.mime);
      info.isImage   = gleditor::MagicMimeDetector::isImageMime(stretch.mime);
      const bool isAnimatedGif =
          info.mime == "image/gif" && gleditor::isAnimatedGif(containerSpan);
      const bool isAnimatedSvg =
          gleditor::MimeType{info.mime} == gleditor::MimeType::ImageSvg &&
          gleditor::SvgAnimator::isAnimated(containerSpan);
      if (isAnimatedGif || isAnimatedSvg) {
        info.isAnimation = true;
        info.isImage     = false;
      }
      info.containerOffset = stretch.start - stretch.containerStart;
      info.containerLength = stretch.containerLength;
      if (info.isAudio) {
        info.label = "Audio Stream";
      } else if (info.isVideo) {
        info.label = "Video Stream";
      } else if (info.isAnimation) {
        info.label = "Animation";
      } else if (info.isImage) {
        info.label = "Image Graphic";
      }

      if (info.isAudio || info.isVideo || info.isAnimation) {
        const auto fit    = mediaFitFor(containerSpan, stretch.mime);
        info.widgetWidth  = fit.width;
        info.widgetHeight = fit.height;
      }
      list.push_back(info);
      docOffset += static_cast<std::uint32_t>(kMediaAnchor.size());
    }
  }
  return list;
}

bool Session::unlockTranscopyright(const std::size_t storeIndex,
                                   const PrimediaSpan &span) {
  if (storeIndex >= stores.size() || !stores[storeIndex].store) {
    return false;
  }
  auto &st       = *stores[storeIndex].store;
  const auto res = st.resolve(span);
  if (res.status != ResolutionStatus::TranscopyrightLocked ||
      !res.lockInfo.has_value()) {
    return false;
  }
  const auto &tc   = *res.lockInfo;
  const auto cek   = TranscopyrightLogic::deriveDeterministicTestCek(tc.keyId);
  const auto count = span.length > 0 ? span.length : 1U;
  const auto cost  = tc.computeCost(count);
  if (!st.contentResolver().unlockTranscopyright(tc.keyId, cek, cost,
                                                 tc.currencySymbol)) {
    return false;
  }

  // Invalidate cached decorations and notify observers
  invalidate();
  for (std::size_t d = 0; d < open.size(); ++d) {
    if (open[d].storeIndex == storeIndex) {
      open[d].decoratedAt = 0;
      open[d].decorations.clear();
      if (tcUnlockedHandler_) {
        tcUnlockedHandler_(d, span, cost);
      }
    }
  }
  return true;
}

bool Session::unlockTranscopyrightAt(const std::uint32_t docIndex,
                                     const std::uint32_t charOffset) {
  if (docIndex >= open.size()) {
    return false;
  }
  const auto sIdx     = open[docIndex].storeIndex;
  const auto &st      = store(sIdx);
  const auto &rebuilt = st.rebuild(open[docIndex].version);
  const auto spans    = rebuilt.spansFor(charOffset, 1);
  if (spans.empty()) {
    return false;
  }
  return unlockTranscopyright(sIdx, spans.front());
}

std::vector<HoleSpanInfo>
Session::holesForView(const std::uint32_t docIndex) const {
  if (docIndex >= open.size()) {
    return {};
  }
  const auto sIdx = open[docIndex].storeIndex;
  const auto &st  = store(sIdx);
  const auto &ver = st.rebuild(open[docIndex].version);
  return TranscopyrightLogic::inspectHoles(st, ver, docIndex, sIdx);
}

std::vector<MicroversionId>
Session::historyOf(const std::uint32_t docIndex) const {
  if (docIndex >= open.size()) {
    return {};
  }
  const auto sIdx       = open[docIndex].storeIndex;
  const auto curVersion = open[docIndex].version;
  const auto &st        = store(sIdx);

  // Ancestral path leading to current version
  std::vector<MicroversionId> history = curVersion.path();
  if (history.empty()) {
    history.emplace_back();
  }

  // Follow forward descendants along main sequential branch
  auto head = curVersion;
  while (true) {
    const auto children = st.children(head);
    if (children.empty()) {
      break;
    }
    head = children.front();
    history.push_back(head);
  }
  return history;
}

void Session::scrubToVersion(const std::uint32_t docIndex,
                             const MicroversionId &version, Doc &doc) {
  flushUncommitted(docIndex);
  if (docIndex >= open.size()) {
    return;
  }
  refresh(docIndex, version);
  const auto sIdx = open[docIndex].storeIndex;
  auto &st        = store(sIdx);
  if (st.isSystem()) {
    st.repointCurrentVersion(version);
    st.save(stores[sIdx].path);
    if (systemDocChangedCallback_) {
      if (const auto kind = systemDocKindForStoreIndex(sIdx)) {
        systemDocChangedCallback_(*kind, st);
      }
    }
  }
  if (const auto src = sourceFor(version, sIdx)) {
    doc.load(*src);
  }
}

bool Session::scrubBackward(const std::uint32_t docIndex, Doc &doc,
                            const std::size_t steps) {
  flushUncommitted(docIndex);
  if (docIndex >= open.size()) {
    return false;
  }
  const auto cur  = open[docIndex].version;
  const auto hist = historyOf(docIndex);
  const auto it   = std::ranges::find(hist, cur);
  if (it == hist.end() || it == hist.begin()) {
    return false;
  }
  const auto curIdx = static_cast<std::size_t>(std::distance(hist.begin(), it));
  const auto targetIdx = (curIdx >= steps) ? (curIdx - steps) : 0U;
  if (targetIdx == curIdx) {
    return false;
  }
  scrubToVersion(docIndex, hist[targetIdx], doc);
  return true;
}

bool Session::scrubForward(const std::uint32_t docIndex, Doc &doc,
                           const std::size_t steps) {
  flushUncommitted(docIndex);
  if (docIndex >= open.size()) {
    return false;
  }
  const auto cur  = open[docIndex].version;
  const auto hist = historyOf(docIndex);
  const auto it   = std::ranges::find(hist, cur);
  if (it == hist.end()) {
    return false;
  }
  const auto curIdx = static_cast<std::size_t>(std::distance(hist.begin(), it));
  if (curIdx + 1 >= hist.size()) {
    return false;
  }
  const auto targetIdx = std::min(hist.size() - 1, curIdx + steps);
  if (targetIdx == curIdx) {
    return false;
  }
  scrubToVersion(docIndex, hist[targetIdx], doc);
  return true;
}

void Session::flushUncommitted(const std::optional<std::uint32_t> docIndex) {
  const auto flushOne = [this](const std::size_t which) {
    if (which >= open.size()) {
      return;
    }
    auto &view = open[which];
    if (view.uncommittedLog.empty()) {
      return;
    }

    const auto compacted = view.uncommittedLog.compact();
    view.uncommittedLog.clear();
    if (compacted.empty()) {
      return;
    }

    const auto focus = focusTargetForView(which);
    if (!focus.has_value() || !focus->isValid()) {
      view.uncommittedLog.clear();
      return;
    }

    const auto sIdx        = view.storeIndex;
    auto &st               = store(sIdx);
    auto curVersion        = view.version;
    const auto targetBirth = focus->birthOp;
    MicroversionId ctxId{};
    if (targetBirth != 0) {
      const auto lastOp = st.lastOpOnStructure(curVersion, targetBirth);
      ctxId             = (lastOp != 0) ? st.segmentedOps().idOf(lastOp)
                                        : st.segmentedOps().idOf(targetBirth);
    }

    for (const auto &op : compacted) {
      if (op.kind == OpKind::Insert) {
        if (!op.text.empty()) {
          // Always a fresh append. This used to search the whole permascroll
          // for a matching run of 24 bytes or more and reuse that span
          // instead -- which meant two documents that happened to contain the
          // same boilerplate line ended up at the same primedia coordinates,
          // and shared coordinates are what transclusion *is* here. It drew
          // gold prisms between documents nobody had quoted from each other,
          // and under transcopyright it would have routed royalties to
          // whoever typed the line first. Storage economy is a real goal, but
          // it belongs below the address layer, not at it.
          curVersion = st.insert(curVersion, op.at, op.text, ctxId);
          GLEDITOR_LOG_DEBUG("xudu.edit", "{} insert {} bytes at {}",
                             curVersion.str(), op.text.size(), op.at);
          if (swarmSource && !st.isSystem()) {
            if (auto appliedOp = st.getOp(curVersion)) {
              broadcastLiveOp(
                  static_cast<std::uint32_t>(which), *appliedOp, curVersion,
                  op.text, op.at + static_cast<std::uint32_t>(op.text.size()),
                  0);
            }
          }
        }
      } else if (op.kind == OpKind::Delete) {
        if (op.length > 0) {
          curVersion = st.erase(curVersion, op.at, op.length, ctxId);
          GLEDITOR_LOG_DEBUG("xudu.edit", "{} delete {} bytes at {}",
                             curVersion.str(), op.length, op.at);
          if (swarmSource && !st.isSystem()) {
            if (auto appliedOp = st.getOp(curVersion)) {
              broadcastLiveOp(static_cast<std::uint32_t>(which), *appliedOp,
                              curVersion, "", op.at, 0);
            }
          }
        }
      }
    }

    refresh(static_cast<std::uint32_t>(which), curVersion);
    if (st.isSystem()) {
      st.repointCurrentVersion(curVersion);
      if (systemDocChangedCallback_) {
        if (const auto kind = systemDocKindForStoreIndex(sIdx)) {
          systemDocChangedCallback_(*kind, st);
        }
      }
    }
    save(sIdx);
  };

  if (docIndex) {
    flushOne(*docIndex);
  } else {
    for (std::size_t i = 0; i < open.size(); ++i) {
      flushOne(i);
    }
  }
}

void Session::tick(const std::chrono::steady_clock::time_point now) {
  for (std::size_t i = 0; i < open.size(); ++i) {
    auto &view = open[i];
    if (!view.uncommittedLog.empty()) {
      const auto elapsed = now - view.uncommittedLog.lastActivity();
      if (elapsed >= idleFlushTimeout) {
        flushUncommitted(static_cast<std::uint32_t>(i));
      }
    }
  }
}

bool Session::hasUncommitted(const std::uint32_t docIndex) const {
  return docIndex < open.size() && !open[docIndex].uncommittedLog.empty();
}

void Session::textInserted(Doc &doc, const std::uint32_t at,
                           const std::string &utf8) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
  }
  const auto focus = focusTargetForView(which);
  if (!focus.has_value() || !focus->isValid()) {
    return;
  }
  open[which].uncommittedLog.recordInsert(at, utf8);
  if (swarmSource) {
    flushUncommitted(static_cast<std::uint32_t>(which));
  }
}

void Session::textErased(Doc &doc, const std::uint32_t at,
                         const std::string &removed) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
  }
  const auto focus = focusTargetForView(which);
  if (!focus.has_value() || !focus->isValid()) {
    return;
  }
  open[which].uncommittedLog.recordErase(at, removed);
  if (swarmSource) {
    flushUncommitted(static_cast<std::uint32_t>(which));
  }
}

void Session::markDecorated(Doc &doc, const std::uint32_t at,
                            const std::uint32_t length,
                            const gleditor::DecorationMask mask) {
  markDecorated(doc.documentIndex(), at, length, mask);
}

void Session::markDecorated(const std::size_t docIndex, const std::uint32_t at,
                            const std::uint32_t length,
                            const gleditor::DecorationMask mask) {
  if (docIndex >= open.size()) {
    return;
  }
  flushUncommitted(docIndex);
  const auto sIdx = open[docIndex].storeIndex;
  auto &st        = store(sIdx);

  const auto textStr  = sourceFor(open[docIndex].version, sIdx)->text();
  std::uint32_t start = at;
  std::uint32_t end   = at + length;
  if (length == 0 && !textStr.empty() && start < textStr.size()) {
    while (start > 0 &&
           !std::isspace(static_cast<unsigned char>(textStr[start - 1]))) {
      --start;
    }
    while (end < textStr.size() &&
           !std::isspace(static_cast<unsigned char>(textStr[end]))) {
      ++end;
    }
  }

  std::uint32_t effLen = length > 0 ? length : 1U;
  if (end > start) {
    effLen = end - start;
  }
  const auto content =
      st.rebuild(open[docIndex].version).spansFor(start, effLen);
  if (content.empty()) {
    return;
  }
  auto version = open[docIndex].version;
  for (const auto decoration :
       {gleditor::Decoration::Bold, gleditor::Decoration::Italic,
        gleditor::Decoration::Underline, gleditor::Decoration::Overline,
        gleditor::Decoration::Strikethrough, gleditor::Decoration::Superscript,
        gleditor::Decoration::Subscript}) {
    if (!gleditor::hasDecoration(mask, decoration)) {
      continue;
    }
    const auto attribute = xanadu::formatAttributeFromDecoration(decoration);
    if (!attribute) {
      continue;
    }
    Link link;
    link.type  = LinkType::Format;
    link.owner = "--type";
    link.left  = content;
    link.right.push_back(xanadu::vocabularySpanFor(*attribute));
    version = st.addLink(version, link);
    GLEDITOR_LOG_DEBUG("xudu.edit", "{} format {} [{}, {})", version.str(),
                       xanadu::formatAttributeName(*attribute), start,
                       start + effLen);
  }
  save(sIdx);
  refresh(docIndex, version);
}

void Session::setAlignment(Doc &doc, const std::uint32_t at,
                           const std::uint32_t length,
                           const gleditor::TextAlign align) {
  setAlignment(doc.documentIndex(), at, length, align);
}

void Session::setAlignment(const std::size_t docIndex, const std::uint32_t at,
                           const std::uint32_t length,
                           const gleditor::TextAlign align) {
  if (docIndex >= open.size()) {
    return;
  }
  flushUncommitted(docIndex);
  const auto sIdx = open[docIndex].storeIndex;
  auto &st        = store(sIdx);

  const auto textStr  = sourceFor(open[docIndex].version, sIdx)->text();
  std::uint32_t start = at;
  std::uint32_t end   = at + length;
  if (length == 0 && !textStr.empty()) {
    while (start > 0 && textStr[start - 1] != '\n') {
      --start;
    }
    while (end < textStr.size() && textStr[end] != '\n') {
      ++end;
    }
    if (end < textStr.size() && textStr[end] == '\n') {
      ++end;
    }
  }

  std::uint32_t effLen = length > 0 ? length : 1U;
  if (end > start) {
    effLen = end - start;
  }
  const auto content =
      st.rebuild(open[docIndex].version).spansFor(start, effLen);
  if (content.empty()) {
    return;
  }
  const auto attribute = xanadu::formatAttributeFromTextAlign(align);
  if (!attribute) {
    return;
  }
  Link link;
  link.type  = LinkType::Format;
  link.owner = "--type";
  link.left  = content;
  link.right.push_back(xanadu::vocabularySpanFor(*attribute));
  auto version = st.addLink(open[docIndex].version, link);
  GLEDITOR_LOG_DEBUG("xudu.edit", "{} align {} [{}, {})", version.str(),
                     xanadu::formatAttributeName(*attribute), start,
                     start + effLen);
  save(sIdx);
  refresh(docIndex, version);
}

void Session::setLocalCollaboratorInfo(std::string name,
                                       std::string fingerprint,
                                       std::string authorScrollKey) {
  localAuthorName_        = std::move(name);
  localAuthorFingerprint_ = std::move(fingerprint);
  localAuthorScrollKey_   = std::move(authorScrollKey);
}

void Session::broadcastLiveOp(const std::uint32_t /*docIndex*/, const Op &op,
                              const MicroversionId &version,
                              const std::string_view primediaText,
                              const std::uint32_t caretOffset,
                              const std::uint32_t selectionLength) {
  if (!swarmSource) {
    return;
  }
  if (localAuthorScrollKey_.empty() && userPermascroll()) {
    localAuthorScrollKey_ = userPermascroll()->globalScrollKey();
  }
  if (localAuthorFingerprint_.empty() && userPermascroll()) {
    localAuthorFingerprint_ =
        userPermascroll()->config().masterIdentity.toString();
  }
  SwarmContentSource::LiveOpBroadcast broadcast;
  broadcast.swarmHash         = collabRoomHash_;
  broadcast.version           = version;
  broadcast.op                = op;
  broadcast.primediaText      = std::string(primediaText);
  broadcast.authorScrollKey   = localAuthorScrollKey_;
  broadcast.authorName        = localAuthorName_;
  broadcast.authorFingerprint = localAuthorFingerprint_;
  broadcast.caretOffset       = caretOffset;
  broadcast.selectionLength   = selectionLength;
  broadcast.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
  swarmSource->broadcastLiveOp(broadcast);
}

void Session::broadcastLocalCaret(const std::uint32_t docIndex,
                                  const std::uint32_t caretOffset,
                                  const std::uint32_t selectionLength) {
  if (!swarmSource) {
    return;
  }
  if (docIndex >= open.size()) {
    return;
  }
  if (localAuthorScrollKey_.empty() && userPermascroll()) {
    localAuthorScrollKey_ = userPermascroll()->globalScrollKey();
  }
  if (localAuthorFingerprint_.empty() && userPermascroll()) {
    localAuthorFingerprint_ =
        userPermascroll()->config().masterIdentity.toString();
  }
  SwarmContentSource::LiveOpBroadcast broadcast;
  broadcast.swarmHash         = collabRoomHash_;
  broadcast.version           = open[docIndex].version;
  broadcast.op.kind           = OpKind::Insert;
  broadcast.op.parent         = open[docIndex].version;
  broadcast.op.at             = caretOffset;
  broadcast.op.length         = 0;
  broadcast.authorScrollKey   = localAuthorScrollKey_;
  broadcast.authorName        = localAuthorName_;
  broadcast.authorFingerprint = localAuthorFingerprint_;
  broadcast.caretOffset       = caretOffset;
  broadcast.selectionLength   = selectionLength;
  broadcast.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
  swarmSource->broadcastLiveOp(broadcast);
}

bool Session::applyRemoteLiveOp(
    const SwarmContentSource::LiveOpBroadcast &broadcast,
    std::vector<std::shared_ptr<Doc>> &docs, Caret *localCaret) {
  const std::string authorKey = !broadcast.authorFingerprint.empty()
                                    ? broadcast.authorFingerprint
                                    : broadcast.authorScrollKey;
  if (authorKey.empty()) {
    return false;
  }
  // Ignore echo of local operations
  if (authorKey == localAuthorFingerprint_ ||
      (!localAuthorScrollKey_.empty() && authorKey == localAuthorScrollKey_)) {
    return false;
  }

  // Update collaborator metadata and presence
  auto &collab           = collaborators_[authorKey];
  collab.authorScrollKey = broadcast.authorScrollKey;
  collab.name = broadcast.authorName.empty() ? "Peer" : broadcast.authorName;
  collab.fingerprint     = broadcast.authorFingerprint;
  collab.caretOffset     = broadcast.caretOffset;
  collab.selectionLength = broadcast.selectionLength;
  collab.lastSeen        = std::chrono::steady_clock::now();
  if (collab.colorRgba == 0) {
    std::size_t h    = std::hash<std::string>{}(authorKey);
    collab.colorRgba = kCollaboratorColors[h % std::size(kCollaboratorColors)];
  }

  // Determine if this broadcast is merely a caret ping
  const bool isCaretOnly =
      (broadcast.op.kind == OpKind::Insert && broadcast.op.length == 0 &&
       broadcast.primediaText.empty());
  if (isCaretOnly) {
    return true;
  }

  if (open.empty()) {
    return false;
  }

  // Match target document view
  std::size_t targetDocIndex = 0;
  for (std::size_t i = 0; i < open.size(); ++i) {
    if (open[i].version == broadcast.op.parent) {
      targetDocIndex = i;
      break;
    }
  }
  collab.docIndex = static_cast<std::uint32_t>(targetDocIndex);

  const auto sIdx = open[targetDocIndex].storeIndex;
  auto &st        = store(sIdx);

  // Apply op to the store
  const auto newVersion = st.applyRemoteLiveOp(
      broadcast.op, broadcast.primediaText, broadcast.authorScrollKey);
  open[targetDocIndex].version = newVersion;
  open[targetDocIndex].pieces  = st.rebuild(newVersion);
  invalidate();
  refresh(static_cast<std::uint32_t>(targetDocIndex), newVersion);

  // Reload Doc text layout if loaded
  if (targetDocIndex < docs.size() && docs[targetDocIndex]) {
    if (const auto src = sourceFor(newVersion, sIdx)) {
      docs[targetDocIndex]->load(*src);
    }
  }

  // Displace local caret if active on this document
  if (localCaret && localCaret->active() &&
      localCaret->documentIndex() == targetDocIndex) {
    if (broadcast.op.kind == OpKind::Insert) {
      const std::uint32_t insertedBytes =
          broadcast.op.length > 0
              ? broadcast.op.length
              : static_cast<std::uint32_t>(broadcast.primediaText.size());
      localCaret->shiftForInsertion(broadcast.op.at, insertedBytes);
    } else if (broadcast.op.kind == OpKind::Delete && broadcast.op.length > 0) {
      localCaret->shiftForErasure(broadcast.op.at, broadcast.op.length);
    }
  }

  // Displace other remote collaborator carets
  for (auto &[k, other] : collaborators_) {
    if (k == authorKey || other.docIndex != targetDocIndex) {
      continue;
    }
    if (broadcast.op.kind == OpKind::Insert) {
      const std::uint32_t insertedBytes =
          broadcast.op.length > 0
              ? broadcast.op.length
              : static_cast<std::uint32_t>(broadcast.primediaText.size());
      if (other.caretOffset >= broadcast.op.at) {
        other.caretOffset += insertedBytes;
      }
    } else if (broadcast.op.kind == OpKind::Delete && broadcast.op.length > 0) {
      if (other.caretOffset > broadcast.op.at + broadcast.op.length) {
        other.caretOffset -= broadcast.op.length;
      } else if (other.caretOffset > broadcast.op.at) {
        other.caretOffset = broadcast.op.at;
      }
    }
  }

  return true;
}

void Session::applyRemoteScrollSealed(
    const SwarmContentSource::ScrollSealedBroadcast &sealed) {
  for (auto &entry : stores) {
    if (entry.store) {
      entry.store->trimRemoteAuthorBuffer(sealed.authorScrollKey,
                                          sealed.sealedUpTo);
    }
  }
}

void Session::decorate(const Doc &doc, std::vector<gleditor::SpanStyle> &out) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
  }
  if (!open[which].uncommittedLog.empty()) {
    flushUncommitted(which);
  }
  auto &view = open[which];
  if (view.decoratedAt == epoch) {
    out.insert(out.end(), view.decorations.begin(), view.decorations.end());
    return;
  }
  view.decorations.clear();
  view.decoratedAt = epoch;
  auto &found      = view.decorations;
  const auto &mine = view.pieces;

  // Passages this document shares with another open one.
  for (std::size_t other = 0; other < open.size(); other++) {
    if (other == which) {
      continue;
    }
    for (const auto &piece : open[other].pieces.pieces()) {
      for (const auto &extent : mine.occurrencesOf(piece)) {
        found.push_back(
            gleditor::SpanStyle{.start  = extent.start,
                                .end    = extent.end,
                                .colour = Session::transclusionColour});
      }
    }
  }

  // Passages links are attached to across all stores
  for (const auto &entry : stores) {
    if (!entry.store) {
      continue;
    }
    for (const auto &[id, link] : entry.store->links()) {
      if (xanadu::LinkType::Format == link.type) {
        continue;
      }
      const auto colour =
          xanadu::linkColourWithInstanceShift(id, link.type, link.tier);
      for (const auto *const ends : {&link.left, &link.right}) {
        for (const auto &span : *ends) {
          for (const auto &extent : mine.occurrencesOf(span)) {
            found.push_back(gleditor::SpanStyle{
                .start = extent.start, .end = extent.end, .colour = colour});
          }
        }
      }
    }
  }

  const auto packageKeys = readerLinkPackages().links();
  const std::vector<PackageDocumentView> packageDocuments{
      {store(view.storeIndex), view.version, mine}};
  const auto packageIndex =
      packageKeys.empty()
          ? nullptr
          : std::make_unique<PackageOccurrenceIndex>(
                packageDocuments, std::span<const PackageCellView>{});
  for (const auto &key : packageKeys) {
    const auto *link = readerLinkPackages().find(key);
    if (!link || link->type == LinkType::Format) continue;
    const auto colour = linkColourWithInstanceShift(
        packageRenderId(key), link->type, ProminenceTier::Curated);
    for (const auto *ends : {&link->left, &link->right})
      for (const auto &span : *ends)
        for (const auto &match : packageIndex->occurrences(span))
          found.push_back(
              {.start  = std::get<DocumentSite>(match.site).range.start,
               .end    = std::get<DocumentSite>(match.site).range.end,
               .colour = colour});
  }

  // Passages that are withheld or transcopyright-locked in this document's
  // store
  if (view.storeIndex < stores.size() && stores[view.storeIndex].store) {
    const auto &st = *stores[view.storeIndex].store;
    for (const auto &piece : mine.pieces()) {
      if (piece.isLocal() || piece.empty() ||
          breakMarkerScroll == piece.scroll ||
          vocabularyScroll == piece.scroll) {
        continue;
      }
      const auto res = st.resolve(piece);
      if (res.status == xanadu::ResolutionStatus::WithheldRedacted) {
        const auto colour = res.holeRecord
                                ? colourForHole(res.holeRecord->reason)
                                : Session::redactionColour;
        for (const auto &extent : mine.occurrencesOf(piece)) {
          found.push_back(gleditor::SpanStyle{
              .start = extent.start, .end = extent.end, .colour = colour});
        }
      } else if (res.status == xanadu::ResolutionStatus::TranscopyrightLocked) {
        for (const auto &extent : mine.occurrencesOf(piece)) {
          found.push_back(gleditor::SpanStyle{
              .start  = extent.start,
              .end    = extent.end,
              .colour = Session::transcopyrightLockedColour});
        }
      }
    }
  }
}

ImageOverlay::ImageOverlay(std::string aFontName)
    : fontName(std::move(aFontName)) {}

ImageOverlay::~ImageOverlay() = default;

void ImageOverlay::deviceReady(render::RenderDevice &device,
                               const render::PipelineDesc &documentPipeline) {
  canvas = std::make_unique<gleditor::Canvas>(&device, fontName);
  // Embedded in the document's own world space, so depth-tested the same way
  // a page's own text is: an image behind the page it sits on should stay
  // behind it.
  canvas->createPipeline(documentPipeline, true);
  imageCache = std::make_unique<gleditor::ImageCache>(&device);
  svgCache   = std::make_unique<gleditor::SvgCache>(&device);
}

void ImageOverlay::place(std::shared_ptr<Doc> doc,
                         const std::uint32_t docOffset, const std::string &id,
                         const std::span<const std::uint8_t> bytes,
                         const gleditor::MimeType &mime) {
  if (!imageCache || !svgCache) {
    return;
  }
  const auto resource = (gleditor::MimeType::ImageSvg == mime)
                            ? svgCache->loadBuffer(id, bytes)
                            : imageCache->loadBuffer(id, bytes, mime);
  if (!resource || !resource->valid()) {
    return;
  }

  // Fit within one page -- matching what Session::placeholderFor()
  // (session.cpp) reserved for it when the document's text was built. The
  // two must agree: this is drawn at the same byte offset that placeholder's
  // blank lines start at, and a differently-sized image here would either
  // leave a gap or overlap the text that follows.
  const auto [width, height] =
      imageFitSize(static_cast<float>(resource->width),
                   static_cast<float>(resource->height));

  placements.push_back(Placement{.doc       = std::move(doc),
                                 .docOffset = docOffset,
                                 .image     = *resource,
                                 .width     = width,
                                 .height    = height});
}

std::optional<ImageOverlay::Corner>
ImageOverlay::bottomLeftOf(const Placement &p) {
  if (!p.doc) {
    return std::nullopt;
  }
  // The layout engine already decided where this image's LayoutBox landed --
  // Doc::boxFor() hands back that box's own bottom-left corner directly, in
  // the same page-pixel space Corner is in, so there is no anchorGapPx
  // arithmetic left to redo here (it is baked into the box's own reserved
  // space via LayoutBox::marginPx, set once in Session::sourceFor()).
  const auto box = p.doc->boxFor(p.docOffset);
  if (!box.has_value()) {
    return std::nullopt;
  }
  return Corner{.pageIndex = box->pageIndex, .x = box->x, .y = box->y};
}

void ImageOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas || !ctx.state.documentsVisible) {
    return;
  }
  for (const auto &p : placements) {
    if (!p.doc || p.doc->isClosing()) {
      continue;
    }
    const auto corner = bottomLeftOf(p);
    if (!corner.has_value()) {
      continue;
    }
    const auto pageIdx  = corner->pageIndex;
    const float anchorX = corner->x;
    const float anchorY = corner->y;

    const auto pageObj = p.doc->page(pageIdx);
    // World-Y of this page's own origin, which callers use to convert a
    // page-pixel-space Y (already the same up-positive, centre-relative
    // convention as anchor->y) into world space: pageCenterY + Y*pixelsToWorld.
    const float pageCenterY = (pageObj.has_value())
                                  ? pageObj->getModel()[3][1]
                                  : (-100.0F * static_cast<float>(pageIdx));

    const auto docModel = p.doc->modelMatrix();
    const auto widgetModel =
        glm::translate(docModel,
                       glm::vec3{anchorX * Doc::pixelsToWorld,
                                 pageCenterY + (anchorY * Doc::pixelsToWorld),
                                 0.05F}) *
        glm::scale(glm::mat4(1.0F),
                   glm::vec3{Doc::pixelsToWorld, Doc::pixelsToWorld, 1.0F});
    const auto transform = ctx.viewProjection * widgetModel;

    canvas->setIdentity(p.doc->documentIndex(), pageIdx);
    canvas->setTag(render::tagKindOverlay, 0);
    canvas->clear();
    canvas->addImage(0.0F, 0.0F, p.width, p.height, p.image, 0xFFFFFFFFU);
    canvas->commit();
    canvas->draw(ctx.state, transform, 1.0F);
  }
}

std::optional<Doc::Anchor>
ImageOverlay::rectFor(const Doc &doc, const std::uint32_t docOffset) const {
  for (const auto &p : placements) {
    if (p.doc.get() != &doc || p.docOffset != docOffset) {
      continue;
    }
    const auto corner = bottomLeftOf(p);
    if (!corner.has_value()) {
      return std::nullopt;
    }
    Doc::Anchor rect;
    rect.pageIndex = corner->pageIndex;
    rect.x         = corner->x + (p.width * 0.5F);
    rect.y         = corner->y + (p.height * 0.5F);
    rect.height    = p.height;
    return rect;
  }
  return std::nullopt;
}

} // namespace xanadu
