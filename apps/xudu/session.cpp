#include "xudu/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

#include "xudu/core/link_layout.hpp"
#include "xudu/core/provenance.hpp"

namespace xudu {

Session::Session(std::string aStorePath) {
  auto primaryStore = std::make_unique<Store>();
  primaryStore->load(aStorePath);
  primaryStore->setContentSource(&contentSource);
  stores.push_back(
      StoreEntry{std::move(primaryStore), std::move(aStorePath), false});
}

Session::~Session() {
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

  const auto sealed =
      sealLocalSpool(st, mine, "primedia", into, provenance, priorState.scroll,
                     priorState.opsAlreadySealed);

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

  auto newStore = std::make_unique<Store>();
  const gleditor::FileTextSource source(filePath);
  const auto imported = newStore->insert(MicroversionId{}, 0, source.text());
  newStore->save(tempDir.string());

  const auto idx = addStore(std::move(newStore), tempDir.string(), true);
  return {idx, imported};
}

std::size_t Session::loadAuxiliaryStore(const std::string &aPath) {
  auto newStore = std::make_unique<Store>();
  newStore->load(aPath);
  return addStore(std::move(newStore), aPath, false);
}

void Session::save(const std::size_t index) const {
  if (index < stores.size() && stores[index].store &&
      !stores[index].path.empty()) {
    stores[index].store->save(stores[index].path);
  }
}

void Session::saveAll() const {
  for (std::size_t i = 0; i < stores.size(); ++i) {
    save(i);
  }
}

MicroversionId Session::versionOf(const std::uint32_t docIndex) const {
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
  open.push_back(OpenView{version, storeIndex, st.rebuild(version), {}, 0});
  invalidate();
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

std::shared_ptr<VersionTextSource>
Session::sourceFor(const MicroversionId &version,
                   const std::size_t storeIndex) const {
  const auto &st     = store(storeIndex);
  const auto rebuilt = st.rebuild(version);
  return std::make_shared<VersionTextSource>(rebuilt.materialize(st), version,
                                             rebuilt.forcedBreaks());
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
  if (docIndex >= open.size()) {
    return;
  }
  refresh(docIndex, version);
  doc.load(sourceFor(version, open[docIndex].storeIndex));
}

bool Session::scrubBackward(const std::uint32_t docIndex, Doc &doc,
                            const std::size_t steps) {
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

void Session::textInserted(Doc &doc, const std::uint32_t at,
                           const std::string &utf8) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
  }
  const auto sIdx     = open[which].storeIndex;
  auto &st            = store(sIdx);
  const auto produced = st.insert(open[which].version, at, utf8);
  refresh(which, produced);
  save(sIdx);
  std::cout << "xudu: " << produced.str() << " insert " << utf8.size()
            << " bytes at " << at << "\n";
}

void Session::textErased(Doc &doc, const std::uint32_t at,
                         const std::string &removed) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
  }
  const auto sIdx     = open[which].storeIndex;
  auto &st            = store(sIdx);
  const auto produced = st.erase(open[which].version, at,
                                 static_cast<std::uint32_t>(removed.size()));
  refresh(which, produced);
  save(sIdx);
  std::cout << "xudu: " << produced.str() << " delete " << removed.size()
            << " bytes at " << at << "\n";
}

void Session::markDecorated(Doc &doc, const std::uint32_t at,
                            const std::uint32_t length,
                            const gleditor::DecorationMask mask) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
  }
  const auto sIdx    = open[which].storeIndex;
  auto &st           = store(sIdx);
  const auto content = st.rebuild(open[which].version).spansFor(at, length);
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
              << xudu::formatAttributeName(*attribute) << " [" << at << ", "
              << (at + length) << ")\n";
  }
  save(sIdx);
  refresh(which, version);
}

void Session::decorate(const Doc &doc, std::vector<gleditor::SpanStyle> &out) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
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
      const auto colour = xudu::linkColour(link.type, link.tier);
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
}

HypertimeMap::HypertimeMap(std::string aFontName, const Session &aSession)
    : fontName(std::move(aFontName)), session(aSession) {}

HypertimeMap::~HypertimeMap() = default;

void HypertimeMap::deviceReady(render::RenderDevice &device,
                               const render::PipelineDesc &documentPipeline) {
  canvas = std::make_unique<gleditor::Canvas>(&device, fontName);
  canvas->createPipeline(documentPipeline, false);
}

void HypertimeMap::layout([[maybe_unused]] RenderState &state) {
  if (builtAt == session.generation() && !nodes.empty()) {
    return;
  }
  nodes.clear();
  edges.clear();
  builtAt = session.generation();

  const auto &st = session.store(0);
  const auto all = st.allVersions();
  if (all.empty()) {
    return;
  }

  // Linear layout for hypertime nodes in reverse chronological order
  float y           = 70.0F;
  constexpr float x = 20.0F;
  constexpr float w = 160.0F;
  constexpr float h = 28.0F;

  std::map<MicroversionId, std::size_t> nodeIndices;

  for (const auto &id : all) {
    Node node;
    node.id         = id;
    node.x          = x;
    node.y          = y;
    node.width      = w;
    node.height     = h;
    node.label      = "State " + id.str();
    nodeIndices[id] = nodes.size();
    nodes.push_back(node);
    y += h + 10.0F;
  }

  for (std::size_t i = 0; i < nodes.size(); i++) {
    const auto &parent = nodes[i].id.parent();
    if (!parent.isZero() && nodeIndices.contains(parent)) {
      Edge edge;
      edge.from        = nodeIndices[parent];
      edge.to          = i;
      const auto &segs = nodes[i].id.segments();
      edge.isBranch    = segs.size() > 1 && segs.back().number == 1;
      edges.push_back(edge);
    }
  }
}

void HypertimeMap::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible || nullptr == canvas) {
    return;
  }
  layout(ctx.state);
  if (nodes.empty()) {
    return;
  }

  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);
  const auto ortho  = glm::ortho(0.0F, width, 0.0F, height, -1.0F, 1.0F);

  canvas->clear();

  // Background map panel
  canvas->addRect(10.0F, 50.0F, 180.0F, std::min(height - 100.0F, 400.0F),
                  0x1A1C24E0U);

  for (const auto &edge : edges) {
    if (edge.from < nodes.size() && edge.to < nodes.size()) {
      const auto &n1 = nodes[edge.from];
      const auto &n2 = nodes[edge.to];
      canvas->addLine(n1.x + (n1.width / 2.0F), n1.y + (n1.height / 2.0F),
                      n2.x + (n2.width / 2.0F), n2.y + (n2.height / 2.0F), 2.0F,
                      edge.isBranch ? 0xFFC040FFU : 0x708090FFU);
    }
  }

  for (std::size_t i = 0; i < nodes.size(); i++) {
    const auto &n     = nodes[i];
    const bool isCur  = (n.id == current);
    const auto nodeBg = isCur ? 0x2A623DFFU : 0x282C34FFU;
    canvas->setTag(render::tagKindOverlay, static_cast<std::uint32_t>(i));
    canvas->addRect(n.x, n.y, n.width, n.height, nodeBg);
    canvas->addText(ctx.state, n.x + 8.0F, n.y + n.height - 5.0F, n.label,
                    isCur ? 0xFFFFFFFFU : 0xCCCCCCFFU, nodeBg);
  }

  canvas->commit();
  canvas->draw(ctx.state, ortho);
}

bool HypertimeMap::picked(const render::PickingResult &pick, RenderState &) {
  if (!visible || pick.tag.kind != render::tagKindOverlay) {
    return false;
  }
  const auto idx = pick.tag.clusterIndex;
  if (idx < nodes.size()) {
    if (goer) {
      goer(nodes[idx].id);
    }
    return true;
  }
  return false;
}

void HypertimeMap::describe(gleditor::a11y::Builder &into) {
  if (!visible || nodes.empty()) {
    return;
  }
  constexpr std::uint64_t mapId = 1;
  auto &mapNode                 = into.add(mapId, gleditor::a11y::Role::List);
  mapNode.label                 = "Hypertime Map";
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto &n     = nodes[i];
    const auto nodeId = 1000U + i;
    auto &node        = into.add(nodeId, gleditor::a11y::Role::ListItem);
    node.label        = n.label;
    node.value        = (n.id == current) ? "selected" : "";
    node.toggled      = (n.id == current);
    node.actions      = gleditor::a11y::bit(gleditor::a11y::Action::Click);
    mapNode.children.push_back(into.id(nodeId));
  }
  into.contribute(into.id(mapId));
}

} // namespace xudu
