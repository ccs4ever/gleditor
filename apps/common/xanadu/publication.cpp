/**
 * @file publication.cpp
 * @brief Implementation of the published, signed document manifest.
 */
#include "publication.hpp" // IWYU pragma: associated

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <memory>
#include <mutex>

#include <lmdb.h>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <filesystem>
#include <fstream>
#include <gleditor/mimetype.hpp>

#include "bencode.hpp"
#include "binary_ops.hpp"
#include "scroll_codec.hpp"
#include "store.hpp"
#include "swarm.hpp"

namespace xanadu {

namespace {

/// Keys of the manifest dictionary. Short, and written once here so that a
/// reader and a writer cannot disagree about them.
constexpr auto keyHoles     = "holes";
constexpr auto keyLinks     = "links";
constexpr auto keyOpsSegs   = "ops";
constexpr auto keyPieces    = "pieces";
constexpr auto keyPublisher = "publisher";
constexpr auto keySalt      = "salt";
constexpr auto keyScrolls   = "scrolls";
constexpr auto keySequence  = "seq";
constexpr auto keySignature = "sig";
constexpr auto keyTime      = "time";
constexpr auto keyTitle     = "title";
constexpr auto keyVersion   = "version";

std::string rawBytes(const PublicKey &key) {
  return std::string{reinterpret_cast<const char *>(key.bytes.data()),
                     key.bytes.size()};
}

bencode::Value encodeSpan(const GlobalSpan &span) {
  return bencode::Value::dict({
      {"len", bencode::Value::integer(static_cast<std::int64_t>(span.length))},
      {"scroll", bencode::Value::string(span.scroll)},
      {"start", bencode::Value::integer(static_cast<std::int64_t>(span.start))},
  });
}

std::optional<GlobalSpan> decodeSpan(const bencode::Value &value) {
  const auto scroll = value.find("scroll");
  const auto start  = value.find("start");
  const auto length = value.find("len");
  if (!scroll || !scroll->isString() || !start || !start->isInteger() ||
      !length || !length->isInteger()) {
    return std::nullopt;
  }
  if (start->asInteger() < 0 || length->asInteger() < 0) {
    return std::nullopt;
  }
  return GlobalSpan{.scroll = scroll->asString(),
                    .start  = static_cast<std::uint64_t>(start->asInteger()),
                    .length = static_cast<std::uint64_t>(length->asInteger())};
}

bencode::Value encodeSpans(const std::vector<GlobalSpan> &spans) {
  bencode::List out;
  out.reserve(spans.size());
  for (const auto &span : spans) {
    out.push_back(encodeSpan(span));
  }
  return bencode::Value::list(std::move(out));
}

std::optional<std::vector<GlobalSpan>>
decodeSpans(const bencode::Value &value) {
  if (!value.isList()) {
    return std::nullopt;
  }
  std::vector<GlobalSpan> out;
  out.reserve(value.asList().size());
  for (const auto &item : value.asList()) {
    auto span = decodeSpan(item);
    if (!span) {
      return std::nullopt;
    }
    out.push_back(*span);
  }
  return out;
}

bencode::Value encodeScroll(const Scroll &scroll) {
  bencode::List segments;
  segments.reserve(scroll.segments.size());
  for (const auto &segment : scroll.segments) {
    segments.push_back(encodeSegment(segment));
  }
  return bencode::Value::dict({
      {"key", bencode::Value::string(rawBytes(scroll.publisher))},
      {"salt", bencode::Value::string(scroll.salt)},
      {"segments", bencode::Value::list(std::move(segments))},
  });
}

std::optional<Scroll> decodeScroll(const bencode::Value &value) {
  const auto key      = value.find("key");
  const auto salt     = value.find("salt");
  const auto segments = value.find("segments");
  if (!key || !key->isString() || key->asString().size() != 32 || !salt ||
      !salt->isString() || !segments || !segments->isList()) {
    return std::nullopt;
  }
  Scroll scroll;
  std::copy(key->asString().begin(), key->asString().end(),
            scroll.publisher.bytes.begin());
  scroll.salt = salt->asString();
  for (const auto &item : segments->asList()) {
    auto segment = decodeSegment(item);
    if (!segment) {
      return std::nullopt;
    }
    scroll.segments.push_back(*segment);
  }
  return scroll;
}

bencode::Value encodeLink(const GlobalLink &link) {
  bencode::Dict dict = {
      {"left", encodeSpans(link.left)},
      {"owner", bencode::Value::string(link.owner)},
      {"right", encodeSpans(link.right)},
      {"tier", bencode::Value::string(prominenceTierName(link.tier))},
      {"type", bencode::Value::string(linkTypeName(link.type))},
  };
  if (!link.curator.empty()) {
    dict.emplace("curator", bencode::Value::string(link.curator));
  }
  return bencode::Value::dict(std::move(dict));
}

std::optional<GlobalLink> decodeLink(const bencode::Value &value) {
  const auto type    = value.find("type");
  const auto owner   = value.find("owner");
  const auto left    = value.find("left");
  const auto right   = value.find("right");
  const auto tier    = value.find("tier");
  const auto curator = value.find("curator");
  if (!type || !type->isString() || !owner || !owner->isString() || !left ||
      !right) {
    return std::nullopt;
  }
  auto lefts  = decodeSpans(*left);
  auto rights = decodeSpans(*right);
  if (!lefts || !rights) {
    return std::nullopt;
  }
  GlobalLink link;
  link.type  = linkTypeFromName(type->asString());
  link.owner = owner->asString();
  link.left  = std::move(*lefts);
  link.right = std::move(*rights);
  if (tier.has_value() && tier->isString()) {
    if ("curated" == tier->asString()) {
      link.tier = ProminenceTier::Curated;
    } else if ("public" == tier->asString()) {
      link.tier = ProminenceTier::Public;
    } else {
      link.tier = ProminenceTier::Author;
    }
  } else {
    link.tier = ProminenceTier::Author;
  }
  if (curator.has_value() && curator->isString()) {
    link.curator = curator->asString();
  }
  return link;
}

std::vector<MicroversionId> historyHeads(const Store &store) {
  std::vector<MicroversionId> heads;
  for (std::uint32_t i = 1; i <= store.opCount(); ++i)
    if (store.segmentedOps().childrenOf(i).empty())
      heads.push_back(store.segmentedOps().idOf(i));
  std::ranges::sort(heads);
  return heads;
}

std::vector<PublishedStructure>
structureInventory(const Store &store,
                   const std::vector<MicroversionId> &heads) {
  std::vector<PublishedStructure> inventory;
  for (const auto &birth : store.discoverStructureBirths()) {
    PublishedStructure item;
    item.kind  = birth.kind;
    item.birth = store.segmentedOps().idOf(birth.opIndex);
    for (const auto &head : heads) {
      const auto path = store.opsFor(head);
      if (std::ranges::find(path, item.birth) == path.end()) continue;
      auto name = store.resolveStructureName(head, birth.opIndex);
      if (name.empty())
        name =
            std::string(item.kind == StructureKind::Slice ? "Slice " : "Doc ") +
            item.birth.str();
      item.views.push_back({head, std::move(name)});
    }
    inventory.push_back(std::move(item));
  }
  PublishedStructure implicit;
  for (const auto &head : heads) {
    for (const auto &id : store.opsFor(head)) {
      const auto *node = store.getCompactOp(id);
      if (node->kind != OpKind::Structure &&
          store.activeXanadocOnBranch(id) == 0) {
        implicit.views.push_back({head, "Document"});
        break;
      }
    }
  }
  if (!implicit.views.empty()) inventory.push_back(std::move(implicit));
  std::ranges::sort(inventory, {}, &PublishedStructure::birth);
  return inventory;
}

bencode::Value encodeInventory(const Publication &pub) {
  bencode::List inventory;
  for (const auto &item : pub.inventory) {
    bencode::List views;
    for (const auto &view : item.views)
      views.push_back(bencode::Value::dict(
          {{"head", bencode::Value::string(view.head.str())},
           {"name", bencode::Value::string(view.name)}}));
    inventory.push_back(bencode::Value::dict(
        {{"birth", bencode::Value::string(item.birth.str())},
         {"kind",
          bencode::Value::integer(static_cast<std::int64_t>(item.kind))},
         {"views", bencode::Value::list(std::move(views))}}));
  }
  return bencode::Value::list(std::move(inventory));
}

/// The manifest as a dictionary, with or without the signature. The signing
/// buffer is this without it, so the two cannot drift.
bencode::Dict manifestOf(const Publication &pub, const bool withSignature) {
  bencode::List links;
  links.reserve(pub.links.size());
  for (const auto &link : pub.links) {
    links.push_back(encodeLink(link));
  }

  bencode::Dict scrolls;
  for (const auto &[key, scroll] : pub.scrolls) {
    scrolls.emplace(key, encodeScroll(scroll));
  }

  bencode::List opsSegs;
  opsSegs.reserve(pub.opsSegments.size());
  for (const auto &segment : pub.opsSegments) {
    opsSegs.push_back(encodeSegment(segment));
  }

  bencode::List holesList;
  holesList.reserve(pub.holes.size());
  for (const auto &hole : pub.holes) {
    holesList.push_back(encodeHole(hole));
  }

  bencode::List topics;
  for (const auto &topic : pub.topics)
    topics.push_back(bencode::Value::string(topic));
  bencode::List heads;
  for (const auto &head : pub.heads)
    heads.push_back(bencode::Value::string(head.str()));
  const auto &idBytes = pub.storeId.bytes();
  bencode::Dict manifest{
      {"store",
       bencode::Value::string(std::string(
           reinterpret_cast<const char *>(idBytes.data()), idBytes.size()))},
      {"history_scroll", bencode::Value::string(pub.historyScroll)},
      {"heads", bencode::Value::list(std::move(heads))},
      {"inventory", encodeInventory(pub)},
      {"selected_birth", bencode::Value::string(pub.selectedBirth.str())},
      {"format", bencode::Value::integer(publicationFormatVersion)},
      {"topics", bencode::Value::list(std::move(topics))},
      {keyHoles, bencode::Value::list(std::move(holesList))},
      {keyLinks, bencode::Value::list(std::move(links))},
      {keyOpsSegs, bencode::Value::list(std::move(opsSegs))},
      {keyPieces, encodeSpans(pub.pieces)},
      {keyPublisher, bencode::Value::string(rawBytes(pub.publisher))},
      {keySalt, bencode::Value::string(pub.salt)},
      {keyScrolls, bencode::Value::dict(std::move(scrolls))},
      {keySequence, bencode::Value::integer(pub.sequence)},
      {keyTime,
       bencode::Value::integer(static_cast<std::int64_t>(pub.published))},
      {keyTitle, bencode::Value::string(pub.title)},
      {keyVersion, bencode::Value::string(pub.version.str())},
  };
  if (withSignature) {
    manifest.emplace(
        keySignature,
        bencode::Value::string(std::string{
            reinterpret_cast<const char *>(pub.signature.bytes.data()),
            pub.signature.bytes.size()}));
  }
  return manifest;
}

} // namespace

GlobalSpan GlobalSpan::intersect(const GlobalSpan &other) const {
  if (scroll != other.scroll) {
    return {};
  }
  const auto from = std::max(start, other.start);
  const auto to   = std::min(end(), other.end());
  if (to <= from) {
    return {};
  }
  return GlobalSpan{.scroll = scroll, .start = from, .length = to - from};
}

bool GlobalSpan::operator<(const GlobalSpan &other) const {
  if (scroll != other.scroll) {
    return scroll < other.scroll;
  }
  if (start != other.start) {
    return start < other.start;
  }
  return length < other.length;
}

bool GlobalLink::touches(const GlobalSpan &span) const {
  const auto meets = [&span](const std::vector<GlobalSpan> &side) {
    return std::ranges::any_of(side, [&span](const GlobalSpan &one) {
      return !one.intersect(span).empty();
    });
  };
  return meets(left) || meets(right);
}

DhtTarget Publication::name() const {
  return MutableLink{.key = publisher, .salt = salt, .displayName = {}}
      .target();
}

std::string Publication::uri() const {
  return MutableLink{.key = publisher, .salt = salt, .displayName = {}}.uri();
}

std::uint64_t Publication::length() const {
  std::uint64_t total = 0;
  for (const auto &piece : pieces) {
    total += piece.length;
  }
  return total;
}

std::string Publication::describe() const {
  return std::format("\"{}\" by {}… seq {}, {} bytes in {} pieces, {} links",
                     title, publisher.hex().substr(0, 8), sequence, length(),
                     pieces.size(), links.size());
}

std::vector<std::string> publicationTopics(const std::string_view input) {
  std::vector<std::string> topics;
  std::size_t at = 0;
  while (at < input.size()) {
    const auto comma = input.find(',', at);
    auto token       = input.substr(
        at, comma == std::string_view::npos ? input.size() - at : comma - at);
    const auto first = token.find_first_not_of(" \t\r\n");
    const auto last  = token.find_last_not_of(" \t\r\n");
    if (first != std::string_view::npos) {
      std::string topic{token.substr(first, last - first + 1)};
      for (auto &byte : topic)
        if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
      if (std::ranges::find(topics, topic) == topics.end())
        topics.push_back(std::move(topic));
    }
    if (comma == std::string_view::npos) break;
    at = comma + 1;
  }
  return topics;
}

std::string publicationSigningBuffer(const Publication &pub) {
  return bencode::Value::dict(manifestOf(pub, false)).encode();
}

std::string encodePublication(const Publication &pub) {
  return bencode::Value::dict(manifestOf(pub, true)).encode();
}

bool verifyPublication(const Publication &pub) {
  return verifyMutableItem(publicationSigningBuffer(pub), pub.signature,
                           pub.publisher);
}

std::optional<Publication> decodePublication(const std::string_view encoded) {
  bencode::Value root;
  try {
    root = bencode::decode(encoded);
  } catch (const std::exception &) {
    return std::nullopt;
  }
  if (!root.isDict()) {
    return std::nullopt;
  }

  const auto format = root.find("format");
  if (!format || !format->isInteger() ||
      format->asInteger() != publicationFormatVersion)
    throw PublicationUnreadable("publication format version 2 expected, got " +
                                (format && format->isInteger()
                                     ? std::to_string(format->asInteger())
                                     : std::string{"0 (unversioned)"}));
  const auto topics   = root.find("topics");
  const auto opsSegs  = root.find(keyOpsSegs);
  const auto holesVal = root.find(keyHoles);
  if (!topics || !topics->isList() || !opsSegs || !opsSegs->isList() ||
      !holesVal || !holesVal->isList())
    throw PublicationUnreadable(
        "publication format 2: required topics/history/holes table missing");
  const auto publisher = root.find(keyPublisher);
  const auto salt      = root.find(keySalt);
  const auto title     = root.find(keyTitle);
  const auto version   = root.find(keyVersion);
  const auto sequence  = root.find(keySequence);
  const auto time      = root.find(keyTime);
  const auto pieces    = root.find(keyPieces);
  const auto links     = root.find(keyLinks);
  const auto scrolls   = root.find(keyScrolls);
  const auto signature = root.find(keySignature);
  if (!publisher || !publisher->isString() ||
      publisher->asString().size() != 32 || !salt || !salt->isString() ||
      !title || !title->isString() || !version || !version->isString() ||
      !sequence || !sequence->isInteger() || !time || !time->isInteger() ||
      !pieces || !links || !links->isList() || !scrolls || !scrolls->isDict() ||
      !signature || !signature->isString() ||
      signature->asString().size() != 64) {
    return std::nullopt;
  }

  const auto storeId       = root.find("store");
  const auto historyScroll = root.find("history_scroll");
  const auto heads         = root.find("heads");
  const auto inventory     = root.find("inventory");
  const auto selectedBirth = root.find("selected_birth");
  if (!storeId || !storeId->isString() || !historyScroll ||
      !historyScroll->isString() || !heads || !heads->isList() || !inventory ||
      !inventory->isList() || !selectedBirth || !selectedBirth->isString())
    throw PublicationUnreadable(
        "publication format 2: required store inventory missing");
  Publication pub;
  if (!DocumentId::fromBytes(storeId->asString(), pub.storeId))
    return std::nullopt;
  pub.historyScroll = historyScroll->asString();
  try {
    pub.selectedBirth = MicroversionId::parse(selectedBirth->asString());
    for (const auto &head : heads->asList()) {
      if (!head.isString()) return std::nullopt;
      pub.heads.push_back(MicroversionId::parse(head.asString()));
    }
    for (const auto &item : inventory->asList()) {
      const auto birth = item.find("birth");
      const auto kind  = item.find("kind");
      const auto views = item.find("views");
      if (!birth || !birth->isString() || !kind || !kind->isInteger() ||
          (kind->asInteger() != static_cast<int>(StructureKind::Slice) &&
           kind->asInteger() != static_cast<int>(StructureKind::Xanadoc)) ||
          !views || !views->isList())
        return std::nullopt;
      PublishedStructure structure;
      structure.birth = MicroversionId::parse(birth->asString());
      structure.kind  = static_cast<StructureKind>(kind->asInteger());
      for (const auto &view : views->asList()) {
        const auto head = view.find("head");
        const auto name = view.find("name");
        if (!head || !head->isString() || !name || !name->isString())
          return std::nullopt;
        structure.views.push_back(
            {MicroversionId::parse(head->asString()), name->asString()});
      }
      pub.inventory.push_back(std::move(structure));
    }
  } catch (const std::exception &) {
    return std::nullopt;
  }
  std::copy(publisher->asString().begin(), publisher->asString().end(),
            pub.publisher.bytes.begin());
  pub.salt  = salt->asString();
  pub.title = title->asString();
  for (const auto &topic : topics->asList()) {
    if (!topic.isString() || topic.asString().empty()) return std::nullopt;
    pub.topics.push_back(topic.asString());
  }
  pub.sequence  = sequence->asInteger();
  pub.published = static_cast<std::uint64_t>(time->asInteger());
  std::copy(signature->asString().begin(), signature->asString().end(),
            pub.signature.bytes.begin());
  try {
    pub.version = MicroversionId::parse(version->asString());
  } catch (const std::exception &) {
    return std::nullopt;
  }

  auto decodedPieces = decodeSpans(*pieces);
  if (!decodedPieces) {
    return std::nullopt;
  }
  pub.pieces = std::move(*decodedPieces);

  for (const auto &item : links->asList()) {
    auto link = decodeLink(item);
    if (!link) {
      return std::nullopt;
    }
    pub.links.push_back(std::move(*link));
  }
  for (const auto &[key, value] : scrolls->asDict()) {
    auto scroll = decodeScroll(value);
    if (!scroll) {
      return std::nullopt;
    }
    pub.scrolls.emplace(key, std::move(*scroll));
  }

  for (const auto &item : opsSegs->asList()) {
    auto segment = decodeSegment(item);
    if (!segment) return std::nullopt;
    pub.opsSegments.push_back(std::move(*segment));
  }
  for (const auto &item : holesVal->asList()) {
    auto hole = decodeHole(item);
    if (!hole) return std::nullopt;
    pub.holes.push_back(std::move(*hole));
  }

  // Checked last, over everything just read: a manifest that does not verify
  // is somebody's claim to have published what they did not, and the only
  // thing to do with it is to fail to read it.
  if (!verifyPublication(pub)) {
    return std::nullopt;
  }
  return pub;
}

std::string publicationPrimedia(const std::string_view bytes,
                                const std::uint64_t at,
                                const std::vector<PublishedHoleRecord> &holes) {
  if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - at)
    throw std::invalid_argument("publication primedia range overflows");
  const auto end = at + bytes.size();
  std::string payload{bytes};
  for (const auto &hole : holes) {
    if (hole.length > std::numeric_limits<std::uint64_t>::max() - hole.at)
      throw std::invalid_argument("publication hole range overflows");
    if (hole.reason != HoleReason::Withheld &&
        hole.reason != HoleReason::Revoked &&
        hole.reason != HoleReason::Takedown)
      continue;
    const auto first = std::max(at, hole.at);
    const auto last  = std::min(end, hole.end());
    if (first >= last) continue;
    std::fill(payload.begin() + static_cast<std::ptrdiff_t>(first - at),
              payload.begin() + static_cast<std::ptrdiff_t>(last - at), '\0');
  }
  return payload;
}

SealedScroll sealLocalSpool(const Store &store, const MutableKeys &keys,
                            const std::string &salt, const std::string &into,
                            const SignedProvenance &provenance,
                            const Scroll &priorScroll,
                            const std::uint32_t opsAlreadySealed,
                            const std::vector<PublishedHoleRecord> &holes) {
  if (provenance.tsv.empty() || provenance.signature.empty()) {
    throw std::runtime_error(
        "cannot seal without a signed authorship record. The record is signed "
        "before the seal and sealed in with the content, so that the info hash "
        "covers both; sealing without one would publish content nobody has "
        "put their name to.");
  }

  const auto allBytes = store.primedia().bytes();
  const auto name     = salt.empty() ? std::string{"primedia"} : salt;

  const auto primediaAlreadySealed = priorScroll.length();
  if (primediaAlreadySealed > allBytes.size()) {
    throw std::runtime_error(
        "cannot seal: the prior scroll already covers more bytes than this "
        "store's local spool holds -- it belongs to a different store.");
  }
  const auto newPrimedia    = allBytes.substr(primediaAlreadySealed);
  const bool hasNewPrimedia = !newPrimedia.empty();

  const auto opsTotal = static_cast<std::uint32_t>(store.opCount());
  if (opsAlreadySealed > opsTotal)
    throw std::runtime_error(
        "cannot seal: prior operations exceed this store's history");
  const bool hasNewOps = opsAlreadySealed < opsTotal;
  const auto newOps =
      hasNewOps ? sealableOps(store, opsAlreadySealed) : std::string{};

  auto wirePayload =
      publicationPrimedia(newPrimedia, primediaAlreadySealed, holes);

  // The content first, so a fresh segment's bytes begin at offset zero of its
  // own piece stream -- what keeps every address already handed out pointing
  // where it did. New operations, when there are any, follow it; the record
  // and its signature always come last, since they describe this seal rather
  // than being seal-specific content of their own.
  std::vector<TorrentContent> files;
  if (hasNewPrimedia) {
    files.push_back(TorrentContent{.path = sealedContentName,
                                   .data = std::move(wirePayload)});
  }
  if (hasNewOps) {
    files.push_back(TorrentContent{.path = sealedOpsName, .data = newOps});
  }
  files.push_back(
      TorrentContent{.path = provenanceFileName, .data = provenance.tsv});
  files.push_back(
      TorrentContent{.path = provenanceSigName, .data = provenance.signature});
  auto made = makeTorrent(files, name);

  SealedScroll sealed;
  sealed.hash        = made.hash;
  sealed.torrentFile = std::move(made.file);
  sealed.provenance  = provenance;

  sealed.scroll           = priorScroll;
  sealed.scroll.publisher = keys.publicKey;
  sealed.scroll.salt      = salt;

  std::uint64_t streamOffset = 0;
  std::uint32_t fileIndex    = 0;
  if (hasNewPrimedia) {
    ScrollSegment segment;
    segment.at           = primediaAlreadySealed;
    segment.length       = newPrimedia.size();
    segment.torrent      = sealed.hash;
    segment.streamOffset = streamOffset;
    segment.fileIndex    = fileIndex;
    segment.path         = sealedContentName;
    sealed.scroll.segments.push_back(segment);
    streamOffset += newPrimedia.size();
    fileIndex++;
  }
  if (hasNewOps) {
    ScrollSegment segment;
    segment.at           = opsAlreadySealed;
    segment.length       = opsTotal - opsAlreadySealed;
    segment.torrent      = sealed.hash;
    segment.streamOffset = streamOffset;
    segment.fileIndex    = fileIndex;
    segment.path         = sealedOpsName;
    sealed.opsSegment    = segment;
  }

  if (!into.empty()) {
    (void)writeTorrentSeed(into, MadeTorrent{sealed.torrentFile, sealed.hash},
                           files);
  }
  return sealed;
}

CompoundPublication
sealCompound(const Store &store, const MutableKeys &keys,
             const std::string &salt, const std::string &into,
             const SignedProvenance &provenance,
             const std::vector<std::filesystem::path> &stagedMediaFiles,
             const Scroll &priorScroll, const std::uint32_t opsAlreadySealed) {
  auto mainSeal = sealLocalSpool(store, keys, salt, into, provenance,
                                 priorScroll, opsAlreadySealed);

  const auto parentProv = parseProvenance(provenance.tsv);

  std::vector<StagedMediaTorrent> mediaTorrents;
  for (const auto &mediaFile : stagedMediaFiles) {
    if (!std::filesystem::exists(mediaFile)) {
      continue;
    }

    std::ifstream file(mediaFile, std::ios::binary);
    if (!file.is_open()) {
      continue;
    }

    std::string data((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    const auto rawName = mediaFile.filename().string();
    std::string name   = rawName;
    if (rawName.size() > 17 && rawName[16] == '_') {
      bool isHex = true;
      for (std::size_t i = 0; i < 16; ++i) {
        if (0 == std::isxdigit(static_cast<unsigned char>(rawName[i]))) {
          isHex = false;
          break;
        }
      }
      if (isHex) {
        name = rawName.substr(17);
      }
    }

    const auto detectedMime =
        gleditor::MimeDetector::detectFile(mediaFile.string());
    const auto fileSha256 = sha256Hex(data);

    // Build inextricably linked provenance record for copyright &
    // transcopyright
    Provenance mediaProv;
    if (parentProv) {
      mediaProv.author    = parentProv->author;
      mediaProv.published = parentProv->published;
    }
    mediaProv.title         = name;
    mediaProv.salt          = name;
    mediaProv.publisher     = keys.publicKey.hex();
    mediaProv.contentLength = data.size();
    mediaProv.contentDigest = fileSha256;
    mediaProv.extra.emplace_back("mime_type", detectedMime.essence());
    mediaProv.extra.emplace_back("transcopyright",
                                 "perpetual-permission-to-quote-on-demand");
    mediaProv.extra.emplace_back(
        "rights",
        "Transcopyright granted: perpetual permission to quote on demand in "
        "xanadocs with attribution.");

    SignedProvenance signedMediaProv;
    signedMediaProv.tsv       = mediaProv.toTsv();
    signedMediaProv.signature = provenance.signature;

    std::vector<TorrentContent> files;
    files.push_back(TorrentContent{.path = name, .data = data});
    files.push_back(TorrentContent{.path = provenanceFileName,
                                   .data = signedMediaProv.tsv});
    files.push_back(TorrentContent{.path = provenanceSigName,
                                   .data = signedMediaProv.signature});

    auto made = makeTorrent(files, name);

    if (!into.empty()) {
      const std::filesystem::path dir = std::filesystem::path(into) / name;
      std::filesystem::create_directories(dir);
      {
        std::ofstream out(std::filesystem::path(into) / (name + ".torrent"),
                          std::ios::binary);
        out << made.file;
      }
      {
        std::ofstream out(dir / name, std::ios::binary);
        out << data;
      }
      {
        std::ofstream out(dir / provenanceFileName, std::ios::binary);
        out << signedMediaProv.tsv;
      }
      {
        std::ofstream out(dir / provenanceSigName, std::ios::binary);
        out << signedMediaProv.signature;
      }
    }

    mediaTorrents.push_back(StagedMediaTorrent{
        .hash        = made.hash,
        .torrentFile = std::move(made.file),
        .mediaPath   = mediaFile.string(),
        .mimeType    = detectedMime.essence(),
        .length      = data.size(),
        .provenance  = std::move(signedMediaProv),
    });
  }

  return CompoundPublication{
      .mainSeal      = std::move(mainSeal),
      .mediaTorrents = std::move(mediaTorrents),
  };
}

namespace {
constexpr auto keySealScroll    = "scroll";
constexpr auto keySealOpsSealed = "opsSealed";
constexpr auto keySealOpsSegs   = "opsSegs";
} // namespace

std::string encodeSealState(const SealState &state) {
  bencode::List opsSegs;
  opsSegs.reserve(state.opsSegments.size());
  for (const auto &segment : state.opsSegments) {
    opsSegs.push_back(encodeSegment(segment));
  }
  return bencode::Value::dict(
             {
                 {keySealScroll, encodeScroll(state.scroll)},
                 {keySealOpsSealed,
                  bencode::Value::integer(
                      static_cast<std::int64_t>(state.opsAlreadySealed))},
                 {keySealOpsSegs, bencode::Value::list(std::move(opsSegs))},
             })
      .encode();
}

std::optional<SealState> decodeSealState(const std::string_view encoded) {
  bencode::Value root;
  try {
    root = bencode::decode(encoded);
  } catch (const std::exception &) {
    return std::nullopt;
  }
  if (!root.isDict()) {
    return std::nullopt;
  }
  const auto scroll    = root.find(keySealScroll);
  const auto opsSealed = root.find(keySealOpsSealed);
  const auto opsSegs   = root.find(keySealOpsSegs);
  if (!scroll || !opsSealed || !opsSealed->isInteger() ||
      opsSealed->asInteger() < 0 || !opsSegs || !opsSegs->isList()) {
    return std::nullopt;
  }
  auto decodedScroll = decodeScroll(*scroll);
  if (!decodedScroll) {
    return std::nullopt;
  }
  SealState state;
  state.scroll           = std::move(*decodedScroll);
  state.opsAlreadySealed = static_cast<std::uint32_t>(opsSealed->asInteger());
  for (const auto &item : opsSegs->asList()) {
    auto segment = decodeSegment(item);
    if (!segment) {
      return std::nullopt;
    }
    state.opsSegments.push_back(std::move(*segment));
  }
  return state;
}

std::string globalKeyOf(const Store &store, const PrimediaSpan &span,
                        const Scroll *const localSealedAs) {
  // A break is not an address, so asking the store for its scroll gets nullptr
  // and reads as "content this machine has not published" -- which is how
  // publishing any document with a page break in it used to throw. It has a
  // name of its own instead.
  if (breakMarkerScroll == span.scroll) {
    return std::string{breakMarkerKey};
  }
  // A piece of the local spool has a global name exactly when the local spool
  // has been sealed: the offsets are the same bytes, so the sealed scroll's
  // name is the address it always had, said globally.
  const auto scroll = span.isLocal() ? gleditor::refOf(localSealedAs)
                                     : store.scroll(span.scroll);
  return scroll ? scrollKey(*scroll) : std::string{};
}

std::optional<GlobalSpan> globalise(const Store &store,
                                    const PrimediaSpan &span,
                                    const Scroll *const localSealedAs) {
  auto key = globalKeyOf(store, span, localSealedAs);
  if (key.empty()) {
    return std::nullopt;
  }
  return GlobalSpan{
      .scroll = std::move(key), .start = span.start, .length = span.length};
}

bool canCarry(const Store &from, const Store &into, const PrimediaSpan &span) {
  if (&from == &into) {
    return true;
  }
  if (span.isLocal()) {
    return &from.userPermascroll() == &into.userPermascroll();
  }
  return span.scroll <= from.scrolls().size();
}

std::optional<PrimediaSpan> carrySpan(const Store &from, Store &into,
                                      const PrimediaSpan &span) {
  if (!canCarry(from, into, span)) {
    return std::nullopt;
  }
  if (&from == &into || span.isLocal()) {
    return span;
  }
  return PrimediaSpan{.scroll = into.addScroll(from.scrolls()[span.scroll - 1]),
                      .start  = span.start,
                      .length = span.length};
}

std::optional<PrimediaSpan>
localise(Store &store, const GlobalSpan &span,
         const std::map<std::string, Scroll> &scrolls) {
  // Already known here, under whatever id this store handed out. Found by the
  // name rather than by asking for the scroll again, so that a store which has
  // learned of a re-seal keeps the identity it already had.
  const auto &known = store.scrolls();
  for (std::size_t i = 0; i < known.size(); i++) {
    if (scrollKey(known[i]) == span.scroll) {
      return PrimediaSpan{.scroll = static_cast<ScrollId>(i + 1),
                          .start  = span.start,
                          .length = span.length};
    }
  }
  const auto found = scrolls.find(span.scroll);
  if (scrolls.end() == found) {
    return std::nullopt;
  }
  return PrimediaSpan{.scroll = store.addScroll(found->second),
                      .start  = span.start,
                      .length = span.length};
}

GlobalOpRef opRefOf(const Store &store, const std::uint32_t opIndex,
                    const Scroll &sealedAs) {
  auto produces = store.segmentedOps().idOf(opIndex);
  if (produces.isZero()) {
    // Index zero is state zero and every other index past the end reads as it
    // too. Neither is an operation, so neither gets a name.
    return {};
  }
  return GlobalOpRef{.scroll   = scrollKey(sealedAs),
                     .produces = std::move(produces)};
}

std::optional<std::uint32_t> localiseOpRef(const Store &store,
                                           const GlobalOpRef &ref,
                                           const Scroll &sealedAs) {
  if (ref.empty() || scrollKey(sealedAs) != ref.scroll) {
    return std::nullopt;
  }
  const auto index = store.segmentedOps().indexOf(ref.produces);
  return 0 == index ? std::nullopt : std::optional{index};
}

namespace {

/// Magic and version for the operations file inside a seal. Its own, rather
/// than the ops spool's: what follows the table below is an ops spool, but the
/// file as a whole is not one and must not be read as one by mistake.
constexpr std::string_view sealedOpsMagic = "\x7fXSO\x01";

} // namespace

std::string sealableOps(const Store &store,
                        const std::uint32_t sinceExclusive) {
  // Which scrolls the operations being sealed actually name. Only these
  // operations, not every one the store holds: a segment carries its own
  // table because it stands alone until historyFromSeal() folds it in beside
  // whatever segments came before it.
  std::map<ScrollId, std::string> named;
  for (std::uint32_t index = sinceExclusive + 1;
       index <= store.segmentedOps().size(); index++) {
    const auto *const node = store.segmentedOps().get(index);
    if (nullptr == node || node->scrollId == localScroll ||
        node->scrollId == breakMarkerScroll) {
      continue; // zero is the scroll being sealed; see sealableOps()'s comment
    }
    if (named.contains(node->scrollId)) {
      continue;
    }
    const auto scroll = store.scroll(node->scrollId);
    if (!scroll) {
      throw std::runtime_error(std::format(
          "cannot seal these operations: one of them quotes scroll {}, which "
          "this store does not hold. An operation naming content nobody can "
          "resolve is an operation with a hole in it.",
          node->scrollId));
    }
    auto key = scrollKey(*scroll);
    if (key.empty()) {
      throw std::runtime_error(
          "cannot seal these operations: one of them quotes a scroll with no "
          "global name. Content has to be published before a history that "
          "points at it can be.");
    }
    named.emplace(node->scrollId, std::move(key));
  }

  std::string out{sealedOpsMagic};
  std::ostringstream table(std::ios::binary);
  writeVarint(table, named.size());
  for (const auto &[id, key] : named) {
    writeVarint(table, id);
    writeVarint(table, key.size());
    table << key;
  }
  out += table.str();
  out += store.exportBinaryOps(sinceExclusive);
  return out;
}

namespace {

/// One sealableOps() segment, applied into @p history. remap's entry for
/// localScroll (the primedia scroll this history was sealed beside) is
/// carried in by the caller and reused across every segment; every other
/// entry is local to this one segment's own table, since a fresh table is
/// where each segment's numbering starts over.
void applyOpsSegment(const std::string_view sealed, Store &history,
                     const ScrollId selfScrollInHistory,
                     const std::map<std::string, Scroll> &scrolls) {
  if (!sealed.starts_with(sealedOpsMagic)) {
    throw std::runtime_error(
        "these are not a seal's operations: the file does not begin the way "
        "one does.");
  }
  std::istringstream in(std::string{sealed.substr(sealedOpsMagic.size())},
                        std::ios::binary);

  std::uint64_t count = 0;
  if (!readVarint(in, count)) {
    throw std::runtime_error("a seal's operations end before their scrolls do");
  }
  // Zero is the scroll this segment was sealed beside, and is the one entry
  // that is never written down: its bytes begin at offset zero of the same
  // stream.
  std::map<ScrollId, ScrollId> remap;
  remap.emplace(localScroll, selfScrollInHistory);
  remap.emplace(breakMarkerScroll, breakMarkerScroll);
  for (std::uint64_t i = 0; i < count; i++) {
    std::uint64_t id     = 0;
    std::uint64_t keyLen = 0;
    if (!readVarint(in, id) || !readVarint(in, keyLen)) {
      throw std::runtime_error("a seal's scroll table ends part way through");
    }
    std::string key(keyLen, '\0');
    in.read(key.data(), static_cast<std::streamsize>(keyLen));
    if (!in) {
      throw std::runtime_error("a seal's scroll table ends part way through");
    }
    const auto found = scrolls.find(key);
    if (scrolls.end() == found) {
      throw std::runtime_error(std::format(
          "a seal's operations quote \"{}\" and the seal does not say where "
          "that is.",
          key));
    }
    remap.emplace(static_cast<ScrollId>(id), history.addScroll(found->second));
  }

  std::vector<OpRecord> records;
  readOpsSpool(in, records);
  for (auto &record : records) {
    // The publisher's ScrollIds meant something in their store; these mean the
    // same content in this one. Everything else about the operation -- where
    // it applies, what it produced, which state it followed -- travels as it
    // was written.
    const auto at = remap.find(record.op.span.scroll);
    if (remap.end() == at) {
      throw std::runtime_error(
          std::format("a seal's operations name scroll {} and its table does "
                      "not say what that was.",
                      record.op.span.scroll));
    }
    record.op.span.scroll = at->second;
  }
  history.adoptOpRecords(records);
}

} // namespace

std::unique_ptr<Store>
historyFromSeal(const std::span<const std::string_view> segments,
                const Scroll &from,
                const std::map<std::string, Scroll> &scrolls) {
  auto history                   = std::make_unique<Store>();
  const auto selfScrollInHistory = history->addScroll(from);
  for (const auto &segment : segments) {
    applyOpsSegment(segment, *history, selfScrollInHistory, scrolls);
  }
  return history;
}

std::unique_ptr<Store>
historyFromSeal(const std::string_view sealed, const Scroll &from,
                const std::map<std::string, Scroll> &scrolls) {
  const std::array<std::string_view, 1> one{sealed};
  return historyFromSeal(std::span<const std::string_view>{one}, from, scrolls);
}

std::unique_ptr<Store>
restorePublication(const Publication &pub, const ContentSource &source,
                   std::shared_ptr<UserPermascroll> readerPermascroll) {
  const auto refuse = [](const std::string_view reason) {
    throw PublicationUnreadable("publication history: " + std::string(reason));
  };
  if (!verifyPublication(pub)) refuse("signature failed");
  if (!readerPermascroll) refuse("reader permascroll must be supplied");
  if (pub.opsSegments.empty()) refuse("no complete history supplied");
  const auto local = pub.scrolls.find(pub.historyScroll);
  if (local == pub.scrolls.end()) refuse("global permascroll missing");
  for (const auto &[key, scroll] : pub.scrolls)
    if (key != scrollKey(scroll)) refuse("scroll identity mismatch");
  auto history =
      std::make_unique<Store>(std::move(readerPermascroll), pub.storeId);
  history->setContentSource(&source);
  const auto self = history->addScroll(local->second);
  history->bindPublishedLocalScroll(self);
  // Install descriptors before folding metadata: rank names are primedia too.
  for (const auto &[key, scroll] : pub.scrolls) {
    (void)key;
    (void)history->addScroll(scroll);
  }
  Resolver resolver(&source);
  std::uint64_t count = 0;
  for (const auto &segment : pub.opsSegments) {
    if (segment.at != count || segment.length == 0)
      refuse("operation segments have a gap or overlap");
    const auto meta = source.metainfo(segment.torrent);
    if (!meta || segment.fileIndex >= meta->files().size())
      refuse("operation torrent metadata unavailable");
    const auto &file = meta->files()[segment.fileIndex];
    if (file.path != segment.path || file.path != sealedOpsName ||
        file.offset != segment.streamOffset)
      refuse("operation file coordinates mismatch");
    const auto scroll =
        Scroll::ofTorrentFile(segment.torrent, segment.fileIndex, file.path,
                              file.offset, file.length);
    const auto bytes =
        resolver.resolve(scroll, {.start = 0, .length = file.length});
    if (!bytes.isVerified() || bytes.text.size() != file.length)
      refuse("operation payload missing or corrupt");
    try {
      applyOpsSegment(bytes.text, *history, self, pub.scrolls);
    } catch (const std::exception &error) {
      refuse(error.what());
    }
    if (history->opCount() - count != segment.length)
      refuse(std::format(
          "operation count disagrees with signed segment: expected {}, got {}",
          segment.length, history->opCount() - count));
    count = history->opCount();
  }
  for (std::uint32_t i = 1; i <= history->opCount(); ++i) {
    const auto span = history->getCompactOp(i)->span();
    if (span.empty() || isReservedScroll(span.scroll)) continue;
    const auto scroll = history->scroll(span.scroll);
    if (!scroll) refuse("operation scroll descriptor missing");
    const auto resolved = resolver.resolve(*scroll, span);
    if (resolved.status == ResolutionStatus::MissingPieces ||
        resolved.status == ResolutionStatus::UnverifiedHash)
      refuse("primedia dependency missing or corrupt");
  }
  if (pub.heads != historyHeads(*history) ||
      pub.inventory != structureInventory(*history, pub.heads))
    refuse("signed inventory disagrees with history");
  if (!history->getOp(pub.version)) refuse("selected version is absent");
  const auto selected = history->activeXanadocOnBranch(pub.version);
  if (history->segmentedOps().idOf(selected) != pub.selectedBirth)
    refuse("selected document birth disagrees with history");
  std::vector<GlobalSpan> pieces;
  for (const auto &span : history->rebuild(pub.version, selected).pieces()) {
    const auto global = globalise(*history, span);
    if (!global) refuse("selected content is not globally addressable");
    pieces.push_back(*global);
  }
  if (pieces != pub.pieces) refuse("selected document disagrees with history");
  return history;
}

Adopted adopt(Store &store, const Publication &pub) {
  if (!verifyPublication(pub)) {
    throw std::runtime_error(
        "cannot read this publication: its signature is not " +
        pub.publisher.hex() +
        "'s. An unsigned or wrongly signed manifest is a claim to have "
        "published what somebody did not, and reading it anyway is what "
        "signing exists to prevent.");
  }

  Adopted taken;
  const auto before = store.scrolls().size();

  // The pieces, in order, each a quotation of published content -- which is
  // what reading somebody else's document is. Nothing is copied: the spans
  // name the publisher's scrolls, so this store now points at the same bytes
  // the publisher's own document points at, and a comparison of addresses
  // finds the two showing the same passage.
  std::uint32_t at = 0;
  for (const auto &piece : pub.pieces) {
    // A break is a place in the publisher's own text rather than a quotation
    // of anything, so it is re-recorded as a break here rather than resolved
    // through the scroll table, and it takes up no room in what follows it.
    if (breakMarkerKey == piece.scroll) {
      taken.version = store.insertBreak(taken.version, at);
      continue;
    }
    const auto found = pub.scrolls.find(piece.scroll);
    if (pub.scrolls.end() == found) {
      throw std::runtime_error(std::format(
          "cannot read this publication: it points at \"{}\" and does not say "
          "where that is. A document with an address nobody can resolve is a "
          "document with a hole in it.",
          piece.scroll));
    }
    taken.version = store.transcludeExternal(taken.version, at, found->second,
                                             piece.start, piece.length);
    at += static_cast<std::uint32_t>(piece.length);
  }

  // The links it asserts. They attach to content rather than to this document,
  // so once they are here they show up on everything this store holds that
  // quotes the same passages -- including documents written here that the
  // publisher has never seen.
  for (const auto &carried : pub.links) {
    Link link;
    link.type        = carried.type;
    link.owner       = carried.owner;
    bool addressable = true;
    const auto bring = [&](const std::vector<GlobalSpan> &side,
                           std::vector<PrimediaSpan> &into) {
      for (const auto &span : side) {
        const auto local = localise(store, span, pub.scrolls);
        if (!local) {
          // An end this store could not resolve even after taking the
          // manifest's scrolls in. Half a link is a claim about a passage
          // that cannot be checked, so the whole of it is left out.
          addressable = false;
          return;
        }
        into.push_back(*local);
      }
    };
    bring(carried.left, link.left);
    bring(carried.right, link.right);
    if (!addressable) {
      continue;
    }
    // A link this store already holds is the same link arriving again --
    // through a second publication that carries it, or through this one being
    // read twice -- and it is one link either way.
    const auto same = [&link](const Link &held) {
      return held.type == link.type && held.owner == link.owner &&
             held.left == link.left && held.right == link.right;
    };
    if (std::ranges::any_of(store.linkView(), same)) {
      continue;
    }
    store.addLink(taken.version, std::move(link));
    taken.links++;
  }

  taken.scrolls = store.scrolls().size() - before;
  return taken;
}

Publication publish(const Store &store, const MicroversionId &version,
                    const MutableKeys &keys, std::string salt,
                    std::string title, const std::int64_t sequence,
                    const std::uint64_t published,
                    const Scroll *const localSealedAs,
                    const std::vector<ScrollSegment> &opsSegments,
                    const std::vector<PublishedHoleRecord> &holes,
                    const std::vector<std::string> &topics) {
  if (!opsSegments.empty() && store.hasPendingMetadata())
    throw PublicationUnreadable(
        "prepare pending author metadata before signing publication history");
  Publication pub;
  pub.storeId = store.documentId();
  pub.selectedBirth =
      store.segmentedOps().idOf(store.activeXanadocOnBranch(version));
  if (!opsSegments.empty()) {
    if (!localSealedAs || scrollKey(*localSealedAs).empty())
      throw PublicationUnreadable(
          "publication history has no global permascroll");
    pub.historyScroll = scrollKey(*localSealedAs);
    pub.scrolls.emplace(pub.historyScroll, *localSealedAs);
    pub.heads     = historyHeads(store);
    pub.inventory = structureInventory(store, pub.heads);
  }
  pub.publisher   = keys.publicKey;
  pub.salt        = std::move(salt);
  pub.title       = std::move(title);
  pub.version     = version;
  pub.sequence    = sequence;
  pub.opsSegments = opsSegments;
  pub.holes       = holes;
  pub.topics      = topics;
  pub.published   = published;

  const auto document  = store.rebuild(version);
  const auto scrollFor = [&store, localSealedAs](const PrimediaSpan &span) {
    return span.isLocal() ? gleditor::refOf(localSealedAs)
                          : store.scroll(span.scroll);
  };

  for (const auto &piece : document.pieces()) {
    const auto global = globalise(store, piece, localSealedAs);
    if (!global) {
      throw std::runtime_error(std::format(
          "cannot publish: {} bytes at {} are content this machine has not "
          "published; seal it into a scroll first",
          piece.length, piece.start));
    }
    if (global->scroll.empty()) {
      throw std::runtime_error(
          "cannot publish: a scroll with no name and no segments has no "
          "address a reader could resolve");
    }
    // A break carries no scroll to put in the table -- there is no content
    // behind it to resolve -- and scrollFor() would hand back nothing.
    if (breakMarkerScroll != piece.scroll) {
      pub.scrolls.insert_or_assign(global->scroll, *scrollFor(piece));
    }
    pub.pieces.push_back(*global);
  }

  // The links whose ends this document actually shows. A store may hold links
  // about anything; what a publication carries are the ones that say something
  // about what it published.
  for (const auto &piece : document.pieces()) {
    for (const auto &link : store.linksTouching(piece)) {
      GlobalLink out;
      out.type               = link.type;
      out.owner              = link.owner;
      bool addressable       = true;
      const auto sayGlobally = [&](const std::vector<PrimediaSpan> &side,
                                   std::vector<GlobalSpan> &into) {
        for (const auto &span : side) {
          const auto global = globalise(store, span, localSealedAs);
          if (!global) {
            // An end nobody else could resolve -- content typed here that has
            // not been sealed. The link is dropped rather than published
            // half-addressed: half a link is a claim about a passage that
            // cannot be checked.
            addressable = false;
            return;
          }
          pub.scrolls.insert_or_assign(global->scroll, *scrollFor(span));
          into.push_back(*global);
        }
      };
      sayGlobally(link.left, out.left);
      sayGlobally(link.right, out.right);
      if (!addressable) {
        continue;
      }
      if (std::ranges::find(pub.links, out) == pub.links.end()) {
        pub.links.push_back(std::move(out));
      }
    }
  }

  if (!opsSegments.empty()) {
    // A history may quote scrolls absent from the selected document's current
    // EDL: deleted quotations, other branches and cells still need their bytes.
    const auto include = [&](const PrimediaSpan &span) {
      if (span.scroll == breakMarkerScroll) return;
      const auto global = globalise(store, span, localSealedAs);
      const auto scroll = scrollFor(span);
      if (!global || !scroll || global->scroll.empty()) {
        throw std::runtime_error(std::format(
            "cannot publish history: scroll {} has not been published",
            span.scroll));
      }
      pub.scrolls.insert_or_assign(global->scroll, *scroll);
    };
    for (std::uint32_t i = 1; i <= store.segmentedOps().size(); ++i) {
      if (const auto *node = store.segmentedOps().get(i)) include(node->span());
    }
    for (const auto &link : store.linkView()) {
      for (const auto &span : link.left) include(span);
      for (const auto &span : link.right) include(span);
    }
  }

  pub.signature = signMutableItem(publicationSigningBuffer(pub), keys);
  return pub;
}

bool Library::add(Publication pub) {
  if (!verifyPublication(pub)) {
    return false;
  }
  const auto name  = pub.name();
  const auto found = byName.find(name);
  if (byName.end() != found && found->second.sequence >= pub.sequence) {
    // A name moves forward. An older publication arriving late is not news.
    return false;
  }
  byName.insert_or_assign(name, std::move(pub));
  return true;
}

const Publication *Library::find(const DhtTarget &name) const {
  const auto found = byName.find(name);
  return byName.end() == found ? nullptr : &found->second;
}

std::vector<const Publication *> Library::all() const {
  std::vector<const Publication *> out;
  out.reserve(byName.size());
  for (const auto &[name, pub] : byName) {
    out.push_back(&pub);
  }
  return out;
}

std::vector<Library::Sighting> Library::showing(const GlobalSpan &span) const {
  std::vector<Sighting> out;
  for (const auto &[name, pub] : byName) {
    // Walk the document's pieces, keeping track of where in its own text each
    // one begins: a sighting has to name a place in the document, not in the
    // scroll, or nothing could scroll to it.
    std::uint32_t at = 0;
    for (const auto &piece : pub.pieces) {
      const auto shared = piece.intersect(span);
      if (!shared.empty()) {
        const auto into =
            static_cast<std::uint32_t>(shared.start - piece.start);
        out.push_back(Sighting{.document = &pub,
                               .start    = at + into,
                               .end      = at + into +
                                      static_cast<std::uint32_t>(shared.length),
                               .shared = shared});
      }
      at += static_cast<std::uint32_t>(piece.length);
    }
  }
  return out;
}

std::vector<Library::FoundLink>
Library::linksTouching(const GlobalSpan &span) const {
  std::vector<FoundLink> out;
  for (const auto &[name, pub] : byName) {
    for (const auto &link : pub.links) {
      const auto onSide = [&span](const std::vector<GlobalSpan> &side) {
        return std::ranges::any_of(side, [&span](const GlobalSpan &one) {
          return !one.intersect(span).empty();
        });
      };
      if (onSide(link.left)) {
        out.push_back(
            FoundLink{.document = &pub, .link = &link, .onLeft = true});
      } else if (onSide(link.right)) {
        out.push_back(
            FoundLink{.document = &pub, .link = &link, .onLeft = false});
      }
    }
  }
  return out;
}

std::int64_t reservePublicationSequence(const std::filesystem::path &directory,
                                        const PublicKey &publisher,
                                        const std::string_view salt,
                                        const std::int64_t observedFloor) {
  if (directory.empty() || publisher.isZero() || salt.size() > 64 ||
      observedFloor < 0) {
    throw std::invalid_argument(
        "invalid publication sequence identity or floor");
  }
  // LMDB permits one environment handle per path in a process. Opening only
  // within this guard keeps its process locks intact, including on close.
  static std::mutex mutex;
  const std::scoped_lock lock(mutex);
  std::filesystem::create_directories(directory);
  const auto check = [](const int rc) {
    if (MDB_SUCCESS != rc) {
      throw std::runtime_error("publication sequence: " +
                               std::string(mdb_strerror(rc)));
    }
  };
  MDB_env *rawEnv = nullptr;
  check(mdb_env_create(&rawEnv));
  const std::unique_ptr<MDB_env, decltype(&mdb_env_close)> env(rawEnv,
                                                               mdb_env_close);
  check(mdb_env_open(env.get(), directory.string().c_str(), 0, 0600));
  MDB_txn *rawTxn = nullptr;
  check(mdb_txn_begin(env.get(), nullptr, 0, &rawTxn));
  std::unique_ptr<MDB_txn, decltype(&mdb_txn_abort)> txn(rawTxn, mdb_txn_abort);
  MDB_dbi db;
  check(mdb_dbi_open(txn.get(), nullptr, 0, &db));
  auto name = publisher.hex() + ":" + std::string(salt);
  MDB_val key{name.size(), name.data()};
  MDB_val value{};
  std::uint64_t prior = static_cast<std::uint64_t>(observedFloor);
  const auto found    = mdb_get(txn.get(), db, &key, &value);
  if (MDB_SUCCESS == found) {
    const std::string_view record(static_cast<const char *>(value.mv_data),
                                  value.mv_size);
    if (record.size() != 12 || !record.starts_with("XPS")) {
      throw PublicationSequenceUnreadable(
          std::format("publication sequence format 1: expected 12 bytes and "
                      "XPS signature, got {} bytes",
                      record.size()));
    }
    if (record[3] != '1') {
      throw PublicationSequenceUnreadable(std::format(
          "publication sequence version 1 expected (byte 49), got byte {}",
          static_cast<unsigned char>(record[3])));
    }
    std::uint64_t stored = 0;
    for (const auto byte : record.substr(4)) {
      stored = (stored << 8) | static_cast<unsigned char>(byte);
    }
    prior = std::max(prior, stored);
  } else if (MDB_NOTFOUND != found) {
    check(found);
  }
  if (prior >=
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::overflow_error("publication sequence exhausted");
  }
  const auto next    = prior + 1;
  std::string record = "XPS1";
  for (int shift = 56; shift >= 0; shift -= 8) {
    record.push_back(static_cast<char>((next >> shift) & 0xffU));
  }
  value = MDB_val{record.size(), record.data()};
  check(mdb_put(txn.get(), db, &key, &value, 0));
  check(mdb_txn_commit(txn.release()));
  return static_cast<std::int64_t>(next);
}

Publication publishDocument(Store &store, const MicroversionId &version,
                            const MutableKeys &documentKeys, std::string salt,
                            std::string title, const std::int64_t sequence,
                            const std::uint64_t published,
                            const SignedProvenance &permascrollProvenance,
                            const SignedProvenance &documentProvenance,
                            const std::string &torrentOutputDir,
                            const std::vector<PublishedHoleRecord> &holes) {
  if (!store.userPermascrollPtr()) {
    throw std::runtime_error("publication requires the author's permascroll");
  }
  if (permascrollProvenance.tsv.empty() ||
      permascrollProvenance.signature.empty() ||
      documentProvenance.tsv.empty() || documentProvenance.signature.empty()) {
    throw std::runtime_error(
        "publication requires signed permascroll and history provenance");
  }
  store.userPermascrollPtr()->sealIncremental(torrentOutputDir,
                                              permascrollProvenance, holes);
  const auto userScroll = store.userPermascroll().currentScroll();
  // The convenience path publishes a complete history snapshot. The session
  // path keeps incremental history segments in its durable SealState instead.
  const auto history =
      sealLocalSpool(store, documentKeys, "history", torrentOutputDir,
                     documentProvenance, userScroll);
  std::vector<ScrollSegment> ops;
  if (history.opsSegment) ops.push_back(*history.opsSegment);
  return publish(store, version, documentKeys, std::move(salt),
                 std::move(title), sequence, published, &userScroll, ops,
                 holes);
}

} // namespace xanadu
