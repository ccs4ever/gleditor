#include "session.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

namespace xudu {

Session::Session(std::string aStorePath) : storePath(std::move(aStorePath)) {
  docStore.load(storePath);
  docStore.setContentSource(&contentSource);
}

void Session::useSwarm(const bool privateNetwork) {
  SwarmContentSource::Options options;
  options.listenInterfaces              = "0.0.0.0:0";
  options.restrictDhtToDistinctNetworks = !privateNetwork;
  swarmSource = std::make_unique<SwarmContentSource>(std::move(options));
  docStore.setContentSource(swarmSource.get());
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
  // From here it is an ordinary content reference, which is the point: what a
  // name adds is one indirection, and everything below it is unchanged.
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
  // A downloader leaves a torrent's data beside the torrent file more often
  // than not, so that is the default rather than something to be told.
  const auto root = dataRoot.empty()
                        ? std::filesystem::path(torrentPath).parent_path().string()
                        : dataRoot;
  if (nullptr != swarmSource) {
    // Not seeding: this machine is asking for the content, not offering it.
    // Anything it does end up holding it will serve, which is what makes a
    // reader of a quotation also a source of it.
    return swarmSource->addTorrent(contents, root.empty() ? "." : root, false);
  }
  return contentSource.add(contents, root.empty() ? "." : root);
}

InfoHash Session::addMagnet(const std::string &uri) {
  const auto link = MagnetLink::parse(uri);
  // The link names the content; it does not carry it, and it does not carry
  // the piece hashes either. Without those nothing can be verified, and
  // returning unverified bytes is the one thing this must not do.
  if (nullptr != swarmSource) {
    // A swarm can do what a magnet needs: join, and ask a peer for the info
    // dictionary. This is the case a magnet was designed for.
    return swarmSource->addMagnet(uri, storePath + "-content");
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

MicroversionId Session::quoteTorrent(const MicroversionId &parent,
                                     const std::uint32_t at,
                                     const InfoHash &hash,
                                     const std::uint32_t fileIndex,
                                     const std::uint64_t offset,
                                     const std::uint64_t length) {
  const auto *const meta = content().metainfo(hash);
  if (nullptr == meta) {
    throw std::runtime_error("no torrent " + hash.hex() +
                             " has been made available");
  }
  if (fileIndex >= meta->files().size()) {
    throw std::runtime_error("torrent " + hash.hex() + " has no file " +
                             std::to_string(fileIndex));
  }
  const auto &file = meta->files()[fileIndex];
  // A scroll of one segment: this content is a fixed torrent file, not a
  // published sequence somebody is still adding to. Its offsets are the file's
  // offsets, so the quotation means what it always meant -- and if the same
  // bytes are later republished under a name, that is a different scroll and
  // honestly so, because nothing binds the two packagings together.
  const auto scroll = Scroll::ofTorrentFile(hash, fileIndex, file.path,
                                            file.offset, file.length);
  // A length of zero means the rest of the file, which is what "quote this
  // document" ought to be spelled as.
  const auto count = 0 == length ? file.length - std::min(offset, file.length)
                                 : length;
  return docStore.transcludeExternal(parent, at, scroll, offset, count);
}

std::string Session::publishedDir() const {
  return (std::filesystem::path(storePath) / "published").string();
}

const MutableKeys &Session::identity() {
  if (keys.has_value()) {
    return *keys;
  }
  const auto path = std::filesystem::path(storePath) / "identity";
  if (std::ifstream in(path); in) {
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
  std::filesystem::create_directories(storePath);
  {
    std::ofstream out(path, std::ios::trunc);
    out << keys->publicKey.hex() << "\n" << keys->secretKey.hex() << "\n";
  }
  // The secret half is the whole of the authority to publish under this name.
  std::error_code ignored;
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, ignored);
  std::cout << "xudu: minted this machine's name " << keys->publicKey.hex()
            << "\n";
  return *keys;
}

std::string Session::publishDocument(const MicroversionId &version,
                                     const std::string &salt,
                                     const std::string &title) {
  const auto &mine = identity();
  const auto into  = publishedDir();

  // One scroll for everything typed on this machine, whatever document it
  // ended up in: the spool is one append-only sequence, so sealing it again
  // covers what is new and leaves every address already handed out alone.
  const auto sealed = sealLocalSpool(docStore, mine, "primedia", into);

  // BEP 44 orders two answers to one name by sequence, so it has to rise.
  // The clock is the one number available here that always has.
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto pub = publish(docStore, version, mine, salt, title,
                           static_cast<std::int64_t>(now), now, &sealed.scroll);

  std::filesystem::create_directories(into);
  const auto path =
      (std::filesystem::path(into) / (salt + ".xanadoc")).string();
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << encodePublication(pub);
  return path;
}

MicroversionId Session::readPublication(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot read publication: " + path);
  }
  const std::string encoded{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
  const auto pub = decodePublication(encoded);
  if (!pub) {
    throw std::runtime_error(
        path + " is not a publication, or is not signed by whoever it claims "
               "to be from. A manifest that does not verify is somebody's "
               "claim to have published what they did not.");
  }
  const auto taken = adopt(docStore, *pub);
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
  const auto produced = docStore.addLink(open[docIndex].version, std::move(link));
  // The text is unchanged -- a link changes none -- so the document on screen
  // stays as it is and only where it sits in hypertime moves.
  refresh(docIndex, produced);
  return produced;
}

MicroversionId Session::versionOf(const std::uint32_t docIndex) const {
  return docIndex < open.size() ? open[docIndex].version : MicroversionId{};
}

std::optional<MicroversionId>
Session::versionShowing(const std::vector<PrimediaSpan> &ends,
                        const std::vector<MicroversionId> &except) const {
  if (ends.empty()) {
    return std::nullopt;
  }
  auto candidates = docStore.allVersions();
  // Most recent first: content is quoted, so several states may show it, and
  // the one somebody is likely to mean is the one furthest along.
  for (const auto &id : std::ranges::reverse_view(candidates)) {
    if (std::ranges::find(except, id) != except.end()) {
      continue;
    }
    const auto pieces = docStore.rebuild(id);
    for (const auto &span : ends) {
      if (!pieces.occurrencesOf(span).empty()) {
        return id;
      }
    }
  }
  return std::nullopt;
}

void Session::viewOpened(const MicroversionId &version) {
  open.push_back(OpenView{version, docStore.rebuild(version), {}, 0});
  invalidate();
}

void Session::refresh(const std::uint32_t docIndex,
                      const MicroversionId &version) {
  if (docIndex >= open.size()) {
    return;
  }
  open[docIndex].version = version;
  open[docIndex].pieces  = docStore.rebuild(version);
  // Every document's decorations depend on every other's contents, so one
  // moving invalidates all of them rather than just its own.
  invalidate();
}

std::shared_ptr<VersionTextSource>
Session::sourceFor(const MicroversionId &version) const {
  return std::make_shared<VersionTextSource>(docStore.textOf(version), version);
}

void Session::textInserted(Doc &doc, const std::uint32_t at,
                           const std::string &utf8) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
  }
  // The library has already spliced the text; this records what it means. The
  // state the operation produces is where this document now is, so typing is
  // what moves it through hypertime.
  const auto produced = docStore.insert(open[which].version, at, utf8);
  refresh(which, produced);
  std::cout << "xudu: " << produced.str() << " insert " << utf8.size()
            << " bytes at " << at << "\n";
}

void Session::textErased(Doc &doc, const std::uint32_t at,
                         const std::string &removed) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
  }
  // Rearrange to limbo: the content stays in the primedia spool, so every
  // earlier state still resolves and this is not destructive.
  const auto produced = docStore.erase(
      open[which].version, at, static_cast<std::uint32_t>(removed.size()));
  refresh(which, produced);
  std::cout << "xudu: " << produced.str() << " delete " << removed.size()
            << " bytes at " << at << "\n";
}

void Session::decorate(const Doc &doc,
                       std::vector<gleditor::SpanStyle> &out) {
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

  // Passages this document shares with another open one. Found by comparing
  // primedia addresses, which is why two documents that merely read the same
  // are not reported and a quotation edited around still is.
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

  // Passages a link is attached to. A link names content, so it shows up here
  // for any document quoting that content, which is the whole of Nelson's
  // "present on all manifestations".
  for (const auto &[id, link] : docStore.links()) {
    for (const auto *const ends : {&link.left, &link.right}) {
      for (const auto &span : *ends) {
        for (const auto &extent : mine.occurrencesOf(span)) {
          found.push_back(
              gleditor::SpanStyle{extent.start, extent.end, Session::linkColour});
        }
      }
    }
  }

  out.insert(out.end(), found.begin(), found.end());
}

// -- the hypertime map --------------------------------------------------------

HypertimeMap::HypertimeMap(std::string aFontName, const Session &aSession)
    : fontName(std::move(aFontName)), session(aSession) {}

void HypertimeMap::deviceReady(render::RenderDevice &device,
                               const render::PipelineDesc &documentPipeline) {
  canvas = std::make_unique<gleditor::Canvas>(&device, fontName);
  canvas->createPipeline(documentPipeline);
}

void HypertimeMap::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible || nullptr == canvas) {
    return;
  }

  // Rebuilding means laying out a glyph per character of every label and
  // re-uploading, so it is done when something changed rather than per frame.
  const auto opCount = session.store().opCount();
  if (opCount != builtForOps || current != builtForCurrent ||
      !builtForVisible || ctx.screenHeight != builtForHeight) {
    builtForOps     = opCount;
    builtForCurrent = current;
    builtForVisible = true;
    builtForHeight  = ctx.screenHeight;

    canvas->clear();

    // A column per generation, so time runs left to right; branches off one
    // state stack downwards within their column.
    const auto versions = session.store().allVersions();
    std::map<std::string, std::pair<float, float>> placed;
    std::vector<int> rowsUsed;

    const auto depthOf = [](const MicroversionId &id) {
      return static_cast<int>(id.path().size());
    };

    int widest = 0;
    for (const auto &id : versions) {
      widest = std::max(widest, depthOf(id));
    }
    rowsUsed.assign(static_cast<std::size_t>(widest) + 1, 0);

    const auto top = static_cast<float>(ctx.screenHeight) - mapMargin;

    // The panel behind it all, sized to what will be drawn on it.
    const auto panelWidth =
        (static_cast<float>(widest) * (nodeWidth + columnGap)) + (2 * padding);
    float panelHeight = padding;
    {
      std::vector<int> counts(rowsUsed.size(), 0);
      for (const auto &id : versions) {
        counts[static_cast<std::size_t>(depthOf(id))]++;
      }
      const auto tallest = counts.empty()
                               ? 0
                               : *std::ranges::max_element(counts);
      panelHeight = (static_cast<float>(tallest) * (nodeHeight + rowGap)) +
                    (2 * padding) + nodeHeight;
    }

    canvas->setTag(render::tagKindOverlay);
    const auto heading = "hypertime: " + std::to_string(versions.size()) +
                         " states, none lost";
    const auto headingWidth = canvas->measureText(heading).width;
    canvas->addRect(mapMargin, top - panelHeight,
                   std::max(panelWidth, headingWidth + (2 * padding)),
                   panelHeight, Doc::VBORow::color3(24, 26, 34));
    canvas->addText(ctx.state, mapMargin + padding, top - padding, heading,
                   Doc::VBORow::color(226), Doc::VBORow::color3(24, 26, 34));

    for (const auto &id : versions) {
      const auto depth  = depthOf(id);
      auto &row         = rowsUsed[static_cast<std::size_t>(depth)];
      const auto left   = mapMargin + padding +
                        (static_cast<float>(depth - 1) * (nodeWidth + columnGap));
      const auto bottom = top - padding - nodeHeight -
                          (static_cast<float>(row + 1) * (nodeHeight + rowGap));
      row++;
      placed.emplace(id.str(), std::make_pair(left, bottom));

      // An edge back to the state this one followed, so a branch reads as a
      // fork rather than as two unrelated columns. Drawn as an elbow rather
      // than a diagonal: the only primitive here is an axis-aligned quad, so a
      // sloped line comes out as its bounding box -- a grey slab between the
      // two nodes. Two segments meeting at a right angle are exact, and are
      // how a graph of this shape is usually drawn anyway.
      if (const auto parent = placed.find(id.parent().str());
          parent != placed.end()) {
        constexpr float edge = 2.0F;
        const auto edgeColour = Doc::VBORow::color3(90, 96, 112);
        const auto fromX      = parent->second.first + nodeWidth;
        const auto fromY      = parent->second.second + (nodeHeight / 2.0F);
        const auto toY        = bottom + (nodeHeight / 2.0F);
        const auto turn       = fromX + (columnGap / 2.0F);
        canvas->addLine(fromX, fromY, turn, fromY, edge, edgeColour);
        canvas->addLine(turn, fromY, turn, toY, edge, edgeColour);
        canvas->addLine(turn, toY, left, toY, edge, edgeColour);
      }

      const bool here = id == current;
      const auto box  = here ? Doc::VBORow::color3(58, 96, 150)
                             : Doc::VBORow::color3(44, 48, 60);
      // Each node carries its own picking identity, so a click on the map can
      // be resolved to the state it landed on.
      canvas->setTag(render::tagKindOverlay,
                    static_cast<std::uint32_t>(placed.size()));
      canvas->addRect(left, bottom, nodeWidth, nodeHeight, box);
      canvas->addText(ctx.state, left + 6.0F, bottom + nodeHeight - 5.0F,
                     id.str(), Doc::VBORow::color(here ? 255 : 208), box);
    }

    canvas->commit();
  }

  // Window pixels with Y running up, matching what the canvas builds in.
  const glm::mat4 projection =
      glm::ortho(0.0F, static_cast<float>(ctx.screenWidth), 0.0F,
                 static_cast<float>(ctx.screenHeight));
  canvas->draw(ctx.state, projection);
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:
