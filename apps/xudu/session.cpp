#include "xudu/session.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/caret.hpp>
#include <gleditor/decode_index.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/svg_animator.hpp>
#include <gleditor/text_source.hpp>

#include "xudu/core/format.hpp"
#include "xudu/core/link_layout.hpp"
#include "xudu/core/provenance.hpp"

namespace xudu {

namespace {
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
} // namespace

Session::Session(std::string aStorePath,
                 std::shared_ptr<UserPermascroll> scroll) {
  auto primaryStore = std::make_unique<Store>(std::move(scroll));
  primaryStore->load(aStorePath);
  primaryStore->setContentSource(&contentSource);
  stores.push_back(
      StoreEntry{std::move(primaryStore), std::move(aStorePath), false});
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
        systemDocChangedCallback_(*kind, st.textOf(prod));
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

MicroversionId Session::insertMedia(const std::uint32_t docIndex,
                                    const std::uint32_t at,
                                    std::string_view bytes,
                                    std::string mimeType) {
  if (docIndex >= open.size()) {
    return MicroversionId{};
  }
  const auto sIdx = open[docIndex].storeIndex;
  auto &st        = store(sIdx);
  const auto prod =
      st.insertMedia(open[docIndex].version, at, bytes, std::move(mimeType))
          .version;
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

Session::~Session() {
  flushUncommitted();
  for (const auto &entry : stores) {
    if (entry.isTemporary && !entry.path.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(entry.path, ec);
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
  if (nullptr != content().metainfo(hash)) {
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
  if (nullptr != swarmSource) {
    return swarmSource->addTorrent(contents, root.empty() ? "." : root, false);
  }
  return contentSource.add(contents, root.empty() ? "." : root);
}

InfoHash Session::addMagnet(const std::string &uri) {
  const auto link = MagnetLink::parse(uri);
  if (nullptr != swarmSource) {
    return swarmSource->addMagnet(uri, path(0) + "-content");
  }
  if (nullptr == contentSource.metainfo(link.hash)) {
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
  const auto *const meta = content().metainfo(hash);
  if (nullptr == meta) {
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
  return (std::filesystem::path(storePath) / "author.yaml").string();
}

/// Where a store's SealState lives -- what makes the next publishDocument()
/// know what the last one already sealed, across separate runs of the
/// program. One file per store rather than per document: the local spool is
/// one scroll shared by every document the store holds, sealed under one
/// name ("primedia") regardless of which document's salt is being published.
std::string sealStatePath(const std::string &storePath) {
  return (std::filesystem::path(storePath) / "seal-state").string();
}

SealState loadSealState(const std::string &storePath) {
  if (std::ifstream in(sealStatePath(storePath), std::ios::binary); in) {
    const std::string text{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    if (auto state = decodeSealState(text); state) {
      return std::move(*state);
    }
  }
  return {};
}

void saveSealState(const std::string &storePath, const SealState &state) {
  std::filesystem::create_directories(storePath);
  std::ofstream out(sealStatePath(storePath),
                    std::ios::binary | std::ios::trunc);
  out << encodeSealState(state);
}

} // namespace

void Session::setAuthor(Author aWho) {
  who = std::move(aWho);
  Config record;
  record.author = *who;
  std::filesystem::create_directories(path(0));
  std::ofstream out(authorPath(path(0)), std::ios::trunc);
  out << record.toYaml();
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
      if (const auto record = Config::fromYaml(text); record) {
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
      keys = MutableKeys{PublicKey::fromHex(publicHex),
                         SecretKey::fromHex(secretHex)};
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

std::string Session::publishDocument(const MicroversionId &version,
                                     const PublishRequest &request,
                                     const std::size_t storeIndex) {
  flushUncommitted();
  auto &st         = store(storeIndex);
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
        "  author: \"Your Name\"\n"
        "  email: \"you@example.org\"\n"
        "or for this store alone with --author-name and --author-email.");
  }

  Provenance record;
  record.author        = who;
  record.salt          = request.salt;
  record.title         = request.title;
  record.extra         = request.extra;
  record.publisher     = mine.publicKey.hex();
  record.version       = version.str();
  record.published     = now;
  record.contentLength = st.primedia().bytes().size();
  record.contentDigest = sha256Hex(st.primedia().bytes());

  const auto priorState = loadSealState(path(storeIndex));
  // The history is sealed beside the content and vouched for beside it. This
  // has to be the same bytes sealLocalSpool() puts in the torrent, so it asks
  // the store the same way rather than describing them a second time -- only
  // what is new since the last seal, exactly as sealLocalSpool() will seal.
  const auto sealedOps = sealableOps(st, priorState.opsAlreadySealed);
  record.opsLength     = sealedOps.size();
  record.opsDigest     = sha256Hex(sealedOps);

  for (const auto &piece : st.rebuild(version).pieces()) {
    if (piece.isLocal()) {
      continue;
    }
    if (const auto *const scroll = st.scroll(piece.scroll); nullptr != scroll) {
      const auto key = scrollKey(*scroll);
      if (!key.empty() &&
          std::ranges::find(record.quotes, key) == record.quotes.end()) {
        record.quotes.push_back(key);
      }
    }
  }
  const auto provenance =
      signProvenance(record, settings().signing(request.passphrase));

  const auto withheldHoles = collectWithheldHoles();
  if (st.userPermascrollPtr()) {
    const auto newlySealed = st.userPermascrollPtr()->sealIncremental(
        into, provenance, withheldHoles);
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

  const auto sealed =
      sealLocalSpool(st, mine, "primedia", into, provenance, priorState.scroll,
                     priorState.opsAlreadySealed, withheldHoles);

  auto opsSegments = priorState.opsSegments;
  if (sealed.opsSegment.has_value()) {
    opsSegments.push_back(*sealed.opsSegment);
  }
  const auto pub =
      publish(st, version, mine, request.salt, request.title,
              static_cast<std::int64_t>(now), now, &sealed.scroll, opsSegments);

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
  return outPath;
}

MicroversionId Session::readPublication(const std::string &aPath) {
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
  auto &st         = store(0);
  const auto taken = adopt(st, *pub);
  invalidate();
  std::cout << "xudu: read " << pub->describe() << " as " << taken.version.str()
            << " (" << taken.scrolls << " scroll(s), " << taken.links
            << " link(s) new here)\n";
  return taken.version;
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

std::size_t Session::addStore(std::unique_ptr<Store> aStore, std::string aPath,
                              const bool aIsTemporary) {
  if (swarmSource) {
    aStore->setContentSource(swarmSource.get());
  } else {
    aStore->setContentSource(&contentSource);
  }
  stores.push_back(
      StoreEntry{std::move(aStore), std::move(aPath), aIsTemporary});
  return stores.size() - 1U;
}

std::pair<std::size_t, MicroversionId>
Session::importFileToTemporaryStore(const std::string &filePath) {
  namespace fs = std::filesystem;
  const auto nowNanos =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto tempDir =
      fs::temp_directory_path() / ("xudu_temp_" + std::to_string(nowNanos) +
                                   "_" + std::to_string(stores.size()));
  fs::create_directories(tempDir);

  auto perma    = (stores.empty() || !stores[0].store)
                      ? nullptr
                      : stores[0].store->userPermascrollPtr();
  auto newStore = std::make_unique<Store>(perma);
  const gleditor::FileTextSource source(filePath);
  // Piece by piece rather than one whole-file insert(), the same way and for
  // the same reason as the very first --import (see main.cpp): a plain file
  // is one plain-text piece and this changes nothing for it, but a PDF's
  // embedded figures only reach the store as classifiable primedia spans
  // through insertMedia(), which pieces() is what makes reachable here.
  MicroversionId imported;
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
      auto inserted =
          newStore->insertMedia(imported, at, piece.bytes, piece.mimeType);
      imported = inserted.version;
      span     = inserted.span;
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
    const auto nowNanos =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tempDir = fs::temp_directory_path() /
                         ("xudu_genesis_" + std::to_string(nowNanos) + "_" +
                          std::to_string(stores.size()));
    fs::create_directories(tempDir);
    targetDir   = tempDir.string();
    isTemporary = true;
  } else {
    fs::create_directories(targetDir);
  }

  auto perma    = (stores.empty() || !stores[0].store)
                      ? nullptr
                      : stores[0].store->userPermascrollPtr();
  auto newStore = std::make_unique<Store>(perma);
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
                         const std::size_t storeIndex) {
  const auto &st = store(storeIndex);
  open.push_back(OpenView{version, storeIndex, st.rebuild(version), {}, 0, {}});
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

  if (std::filesystem::exists(dir / "ops.nodes") ||
      std::filesystem::exists(dir / "ops.spool") ||
      std::filesystem::exists(dir / "current.yaml")) {
    sysStore->load(dir.string());
    if (sysStore->currentVersions().empty() && !sysStore->latest().isZero()) {
      sysStore->repointCurrentVersion(sysStore->latest());
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
    systemDocChangedCallback_(kind, st.textOf(version));
  }
}

MicroversionId Session::openSystemDoc(const SystemDocKind kind) {
  const auto sIdx = systemStoreIndex(kind);
  auto &st        = store(sIdx);
  auto ver        = st.primaryCurrentVersion();
  if (ver.isZero()) {
    ver = st.latest();
  }
  for (std::size_t i = 0; i < open.size(); ++i) {
    if (open[i].storeIndex == sIdx) {
      return open[i].version;
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
    for (std::uint32_t i = 0; i < opCount; ++i) {
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

  std::sort(holes.begin(), holes.end(),
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
  if (!st.advance(open[docIndex].pieces, open[docIndex].version, version)) {
    open[docIndex].pieces = st.rebuild(version);
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
  return {width, height};
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
  return {viewport.width,
          viewport.height + gleditor::MediaWidget::chromeHeightPx};
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
    const auto decoded =
        gleditor::decodeImageBuffer(bytes, gleditor::MimeType{mime});
    const float natW =
        decoded.valid() ? static_cast<float>(decoded.width) : 1.0F;
    const float natH =
        decoded.valid() ? static_cast<float>(decoded.height) : 1.0F;
    return imageFitSize(natW, natH);
  }
  if (gleditor::MagicMimeDetector::isVideoMime(mime)) {
    const auto [natW, natH] = videoNaturalSizeFor(bytes);
    return videoFitSize(natW, natH);
  }
  return {Session::audioCardWidthPx, Session::audioCardHeightPx};
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
    const auto bytes = st.read(PrimediaSpan{run.scroll, start, length});
    const auto mime  = magic.identifyBuffer(bytes.data(), bytes.size());
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
Session::sourceFor(const MicroversionId &version,
                   const std::size_t storeIndex) const {
  const auto &st     = store(storeIndex);
  const auto rebuilt = st.rebuild(version);
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
        const auto span =
            PrimediaSpan{run.scroll, stretch.start, stretch.length};
        const auto res = st.resolve(span);
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
      const auto containerBytes = st.read(PrimediaSpan{
          run.scroll, stretch.containerStart, stretch.containerLength});
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

  std::vector<gleditor::DecoratedRange> decoratedRanges;

  // Extract presentation formatting and paragraph alignment from Format links
  for (const auto &[linkId, link] : st.links()) {
    if (LinkType::Format != link.type) {
      continue;
    }
    const auto attrOpt = st.formatAttributeOf(link);
    if (!attrOpt) {
      continue;
    }
    if (const auto decoOpt = decorationFromFormatAttribute(*attrOpt)) {
      const auto mask = gleditor::decorationBit(*decoOpt);
      for (const auto &span : link.left) {
        for (const auto &extent : rebuilt.occurrencesOf(span)) {
          if (!extent.empty()) {
            decoratedRanges.push_back(gleditor::DecoratedRange{
                .start       = extent.start,
                .end         = extent.end,
                .decorations = mask,
            });
          }
        }
      }
    } else if (const auto alignOpt = textAlignFromFormatAttribute(*attrOpt)) {
      for (const auto &span : link.left) {
        for (const auto &extent : rebuilt.occurrencesOf(span)) {
          if (!extent.empty()) {
            blockStyles.push_back(gleditor::BlockStyleRange{
                .start = extent.start,
                .end   = extent.end,
                .align = *alignOpt,
            });
          }
        }
      }
    }
  }

  std::string title;
  if (st.isSystem()) {
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

  return std::make_shared<VersionTextSource>(
      concatext, version, breaks, boxes, blockStyles, title, decoratedRanges);
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
      const auto containerBytes = st.read(PrimediaSpan{
          run.scroll, stretch.containerStart, stretch.containerLength});
      const std::span<const std::uint8_t> containerSpan(
          reinterpret_cast<const std::uint8_t *>(containerBytes.data()),
          containerBytes.size());

      MediaSpanInfo info;
      info.span      = PrimediaSpan{run.scroll, stretch.start, stretch.length};
      info.docOffset = docOffset;
      info.mime      = stretch.mime;
      info.isAudio   = gleditor::MagicMimeDetector::isAudioMime(stretch.mime);
      info.isVideo   = gleditor::MagicMimeDetector::isVideoMime(stretch.mime);
      info.isImage   = gleditor::MagicMimeDetector::isImageMime(stretch.mime);
      if (info.mime == "image/gif" && gleditor::isAnimatedGif(containerSpan)) {
        info.isAnimation = true;
        info.isImage     = false;
      } else if (gleditor::MimeType{info.mime} ==
                     gleditor::MimeType::ImageSvg &&
                 gleditor::SvgAnimator::isAnimated(containerSpan)) {
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

bool Session::unlockTranscopyright(const std::size_t storeIdx,
                                   const PrimediaSpan &span) {
  if (storeIdx >= stores.size() || !stores[storeIdx].store) {
    return false;
  }
  auto &st       = *stores[storeIdx].store;
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
    if (open[d].storeIndex == storeIdx) {
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
    history.push_back(MicroversionId{});
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
        systemDocChangedCallback_(*kind, st.textOf(version));
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

    const auto sIdx = view.storeIndex;
    auto &st        = store(sIdx);
    auto curVersion = view.version;

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
          curVersion = st.insert(curVersion, op.at, op.text);
          std::cout << "xudu: " << curVersion.str() << " insert "
                    << op.text.size() << " bytes at " << op.at << "\n";
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
          curVersion = st.erase(curVersion, op.at, op.length);
          std::cout << "xudu: " << curVersion.str() << " delete " << op.length
                    << " bytes at " << op.at << "\n";
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
          systemDocChangedCallback_(*kind, st.textOf(curVersion));
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

void Session::markDecorated(const std::size_t which, const std::uint32_t at,
                            const std::uint32_t length,
                            const gleditor::DecorationMask mask) {
  if (which >= open.size()) {
    return;
  }
  flushUncommitted(which);
  const auto sIdx = open[which].storeIndex;
  auto &st        = store(sIdx);

  const auto textStr  = sourceFor(open[which].version, sIdx)->text();
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

  const auto effLen =
      (end > start) ? (end - start) : (length > 0 ? length : 1U);
  const auto content = st.rebuild(open[which].version).spansFor(start, effLen);
  if (content.empty()) {
    return;
  }
  auto version = open[which].version;
  for (const auto decoration :
       {gleditor::Decoration::Bold, gleditor::Decoration::Italic,
        gleditor::Decoration::Underline, gleditor::Decoration::Overline,
        gleditor::Decoration::Strikethrough, gleditor::Decoration::Superscript,
        gleditor::Decoration::Subscript}) {
    if (!gleditor::hasDecoration(mask, decoration)) {
      continue;
    }
    const auto attribute = xudu::formatAttributeFromDecoration(decoration);
    if (!attribute) {
      continue;
    }
    Link link;
    link.type  = LinkType::Format;
    link.owner = "--type";
    link.left  = content;
    link.right.push_back(xudu::vocabularySpanFor(*attribute));
    version = st.addLink(version, link);
    std::cout << "xudu: " << version.str() << " format "
              << xudu::formatAttributeName(*attribute) << " [" << start << ", "
              << (start + effLen) << ")\n";
  }
  save(sIdx);
  refresh(which, version);
}

void Session::setAlignment(Doc &doc, const std::uint32_t at,
                           const std::uint32_t length,
                           const gleditor::TextAlign align) {
  setAlignment(doc.documentIndex(), at, length, align);
}

void Session::setAlignment(const std::size_t which, const std::uint32_t at,
                           const std::uint32_t length,
                           const gleditor::TextAlign align) {
  if (which >= open.size()) {
    return;
  }
  flushUncommitted(which);
  const auto sIdx = open[which].storeIndex;
  auto &st        = store(sIdx);

  const auto textStr  = sourceFor(open[which].version, sIdx)->text();
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

  const auto effLen =
      (end > start) ? (end - start) : (length > 0 ? length : 1U);
  const auto content = st.rebuild(open[which].version).spansFor(start, effLen);
  if (content.empty()) {
    return;
  }
  const auto attribute = xudu::formatAttributeFromTextAlign(align);
  if (!attribute) {
    return;
  }
  Link link;
  link.type  = LinkType::Format;
  link.owner = "--type";
  link.left  = content;
  link.right.push_back(xudu::vocabularySpanFor(*attribute));
  auto version = st.addLink(open[which].version, link);
  std::cout << "xudu: " << version.str() << " align "
            << xudu::formatAttributeName(*attribute) << " [" << start << ", "
            << (start + effLen) << ")\n";
  save(sIdx);
  refresh(which, version);
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
        found.push_back(gleditor::SpanStyle{extent.start, extent.end,
                                            Session::transclusionColour});
      }
    }
  }

  // Passages links are attached to across all stores
  for (const auto &entry : stores) {
    if (!entry.store) {
      continue;
    }
    for (const auto &[id, link] : entry.store->links()) {
      if (xudu::LinkType::Format == link.type) {
        continue;
      }
      const auto colour =
          xudu::linkColourWithInstanceShift(id, link.type, link.tier);
      for (const auto *const ends : {&link.left, &link.right}) {
        for (const auto &span : *ends) {
          for (const auto &extent : mine.occurrencesOf(span)) {
            found.push_back(
                gleditor::SpanStyle{extent.start, extent.end, colour});
          }
        }
      }
    }
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
      if (res.status == xudu::ResolutionStatus::WithheldRedacted) {
        const auto colour = res.holeRecord
                                ? colourForHole(res.holeRecord->reason)
                                : Session::redactionColour;
        for (const auto &extent : mine.occurrencesOf(piece)) {
          found.push_back(
              gleditor::SpanStyle{extent.start, extent.end, colour});
        }
      } else if (res.status == xudu::ResolutionStatus::TranscopyrightLocked) {
        for (const auto &extent : mine.occurrencesOf(piece)) {
          found.push_back(gleditor::SpanStyle{
              extent.start, extent.end, Session::transcopyrightLockedColour});
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

  placements.push_back(
      Placement{std::move(doc), docOffset, *resource, width, height});
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
  return Corner{box->pageIndex, box->x, box->y};
}

void ImageOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas) {
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

    const auto *const pageObj = p.doc->page(pageIdx);
    // World-Y of this page's own origin, which callers use to convert a
    // page-pixel-space Y (already the same up-positive, centre-relative
    // convention as anchor->y) into world space: pageCenterY + Y*pixelsToWorld.
    const float pageCenterY = (nullptr != pageObj)
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

} // namespace xudu
