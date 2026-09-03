/**
 * @file publication.cpp
 * @brief Implementation of the published, signed document manifest.
 */
#include "publication.hpp" // IWYU pragma: associated

#include <algorithm>
#include <array>
#include <format>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <filesystem>
#include <fstream>
#include <gleditor/mimetype.hpp>

#include "bencode.hpp"
#include "binary_ops.hpp"
#include "store.hpp"
#include "swarm.hpp"

namespace xudu {

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

bencode::Value encodeHole(const PublishedHoleRecord &hole) {
  bencode::Dict dict = {
      {"at", bencode::Value::integer(static_cast<std::int64_t>(hole.at))},
      {"len", bencode::Value::integer(static_cast<std::int64_t>(hole.length))},
      {"reason",
       bencode::Value::string(std::string(holeReasonName(hole.reason)))},
  };
  if (!identity::constantTimeEquals(hole.contentCommitment,
                                    std::array<std::uint8_t, 32>{})) {
    dict.emplace(
        "commitment",
        bencode::Value::string(std::string{
            reinterpret_cast<const char *>(hole.contentCommitment.data()),
            hole.contentCommitment.size()}));
  }
  if (hole.transcopyright.has_value()) {
    const auto &tc       = *hole.transcopyright;
    bencode::Dict tcDict = {
        {"flat", bencode::Value::integer(tc.flatFee ? 1 : 0)},
        {"key_id", bencode::Value::string(std::string{
                       reinterpret_cast<const char *>(tc.keyId.data()),
                       tc.keyId.size()})},
        {"memo", bencode::Value::string(tc.licenseMemo)},
        {"nonce", bencode::Value::string(std::string{
                      reinterpret_cast<const char *>(tc.nonce.data()),
                      tc.nonce.size()})},
        {"price", bencode::Value::integer(
                      static_cast<std::int64_t>(tc.priceAtomicUnits))},
        {"sym", bencode::Value::string(tc.currencySymbol)},
        {"wallet", bencode::Value::string(tc.authorWallet.toString())},
    };
    if (!tc.authorPubKey.isZero()) {
      tcDict.emplace(
          "pubkey",
          bencode::Value::string(std::string{
              reinterpret_cast<const char *>(tc.authorPubKey.bytes.data()),
              tc.authorPubKey.bytes.size()}));
    }
    dict.emplace("transcopyright", bencode::Value::dict(std::move(tcDict)));
  }
  return bencode::Value::dict(std::move(dict));
}

std::optional<PublishedHoleRecord> decodeHole(const bencode::Value &value) {
  if (!value.isDict()) {
    return std::nullopt;
  }
  const auto *at     = value.find("at");
  const auto *length = value.find("len");
  const auto *reason = value.find("reason");
  if (nullptr == at || !at->isInteger() || nullptr == length ||
      !length->isInteger() || nullptr == reason || !reason->isString()) {
    return std::nullopt;
  }
  if (at->asInteger() < 0 || length->asInteger() < 0) {
    return std::nullopt;
  }
  PublishedHoleRecord hole;
  hole.at     = static_cast<std::uint64_t>(at->asInteger());
  hole.length = static_cast<std::uint64_t>(length->asInteger());
  hole.reason = holeReasonFromName(reason->asString());

  if (const auto *comm = value.find("commitment");
      nullptr != comm && comm->isString() && comm->asString().size() == 32) {
    std::copy(comm->asString().begin(), comm->asString().end(),
              hole.contentCommitment.begin());
  }

  if (const auto *tcVal = value.find("transcopyright");
      nullptr != tcVal && tcVal->isDict()) {
    TranscopyrightDescriptor tc;
    const auto *price  = tcVal->find("price");
    const auto *flat   = tcVal->find("flat");
    const auto *sym    = tcVal->find("sym");
    const auto *memo   = tcVal->find("memo");
    const auto *wallet = tcVal->find("wallet");
    const auto *keyId  = tcVal->find("key_id");
    const auto *nonce  = tcVal->find("nonce");
    const auto *pubkey = tcVal->find("pubkey");

    if (nullptr == price || !price->isInteger() || nullptr == keyId ||
        !keyId->isString() || keyId->asString().size() != 32 ||
        nullptr == nonce || !nonce->isString() ||
        nonce->asString().size() != 24) {
      return std::nullopt;
    }
    tc.priceAtomicUnits = static_cast<std::uint64_t>(price->asInteger());
    if (nullptr != flat && flat->isInteger()) {
      tc.flatFee = flat->asInteger() != 0;
    }
    if (nullptr != sym && sym->isString()) {
      tc.currencySymbol = sym->asString();
    }
    if (nullptr != memo && memo->isString()) {
      tc.licenseMemo = memo->asString();
    }
    if (nullptr != wallet && wallet->isString()) {
      auto fp = identity::Fingerprint::fromString(wallet->asString());
      if (fp.has_value()) {
        tc.authorWallet = *fp;
      }
    }
    std::copy(keyId->asString().begin(), keyId->asString().end(),
              tc.keyId.begin());
    std::copy(nonce->asString().begin(), nonce->asString().end(),
              tc.nonce.begin());
    if (nullptr != pubkey && pubkey->isString() &&
        pubkey->asString().size() == 32) {
      std::copy(pubkey->asString().begin(), pubkey->asString().end(),
                tc.authorPubKey.bytes.begin());
    }
    hole.transcopyright = tc;
  }
  return hole;
}

bencode::Value encodeSpan(const GlobalSpan &span) {
  return bencode::Value::dict({
      {"len", bencode::Value::integer(static_cast<std::int64_t>(span.length))},
      {"scroll", bencode::Value::string(span.scroll)},
      {"start", bencode::Value::integer(static_cast<std::int64_t>(span.start))},
  });
}

std::optional<GlobalSpan> decodeSpan(const bencode::Value &value) {
  const auto *scroll = value.find("scroll");
  const auto *start  = value.find("start");
  const auto *length = value.find("len");
  if (nullptr == scroll || !scroll->isString() || nullptr == start ||
      !start->isInteger() || nullptr == length || !length->isInteger()) {
    return std::nullopt;
  }
  if (start->asInteger() < 0 || length->asInteger() < 0) {
    return std::nullopt;
  }
  return GlobalSpan{scroll->asString(),
                    static_cast<std::uint64_t>(start->asInteger()),
                    static_cast<std::uint64_t>(length->asInteger())};
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

bencode::Value encodeSegment(const ScrollSegment &segment) {
  bencode::Dict dict = {
      {"at", bencode::Value::integer(static_cast<std::int64_t>(segment.at))},
      {"file", bencode::Value::integer(segment.fileIndex)},
      {"len",
       bencode::Value::integer(static_cast<std::int64_t>(segment.length))},
      {"path", bencode::Value::string(segment.path)},
      {"stream", bencode::Value::integer(
                     static_cast<std::int64_t>(segment.streamOffset))},
      {"torrent",
       bencode::Value::string(std::string{
           reinterpret_cast<const char *>(segment.torrent.bytes.data()),
           segment.torrent.bytes.size()})},
  };
  if (segment.kind != SegmentKind::Plain) {
    dict.emplace("kind", bencode::Value::integer(
                             static_cast<std::int64_t>(segment.kind)));
  }
  if (segment.holeRecord.has_value()) {
    dict.emplace("hole", encodeHole(*segment.holeRecord));
  }
  return bencode::Value::dict(std::move(dict));
}

std::optional<ScrollSegment> decodeSegment(const bencode::Value &value) {
  const auto *at      = value.find("at");
  const auto *length  = value.find("len");
  const auto *torrent = value.find("torrent");
  const auto *stream  = value.find("stream");
  const auto *file    = value.find("file");
  const auto *path    = value.find("path");
  if (nullptr == at || !at->isInteger() || nullptr == length ||
      !length->isInteger() || nullptr == torrent || !torrent->isString() ||
      torrent->asString().size() != 20 || nullptr == stream ||
      !stream->isInteger() || nullptr == file || !file->isInteger() ||
      nullptr == path || !path->isString()) {
    return std::nullopt;
  }
  ScrollSegment segment;
  segment.at           = static_cast<std::uint64_t>(at->asInteger());
  segment.length       = static_cast<std::uint64_t>(length->asInteger());
  segment.streamOffset = static_cast<std::uint64_t>(stream->asInteger());
  segment.fileIndex    = static_cast<std::uint32_t>(file->asInteger());
  segment.path         = path->asString();
  std::copy(torrent->asString().begin(), torrent->asString().end(),
            segment.torrent.bytes.begin());
  if (const auto *kindVal = value.find("kind");
      nullptr != kindVal && kindVal->isInteger()) {
    segment.kind = static_cast<SegmentKind>(kindVal->asInteger());
  }
  if (const auto *holeVal = value.find("hole"); nullptr != holeVal) {
    segment.holeRecord = decodeHole(*holeVal);
  }
  return segment;
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
  const auto *key      = value.find("key");
  const auto *salt     = value.find("salt");
  const auto *segments = value.find("segments");
  if (nullptr == key || !key->isString() || key->asString().size() != 32 ||
      nullptr == salt || !salt->isString() || nullptr == segments ||
      !segments->isList()) {
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
  const auto *type    = value.find("type");
  const auto *owner   = value.find("owner");
  const auto *left    = value.find("left");
  const auto *right   = value.find("right");
  const auto *tier    = value.find("tier");
  const auto *curator = value.find("curator");
  if (nullptr == type || !type->isString() || nullptr == owner ||
      !owner->isString() || nullptr == left || nullptr == right) {
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
  if (nullptr != tier && tier->isString()) {
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
  if (nullptr != curator && curator->isString()) {
    link.curator = curator->asString();
  }
  return link;
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

  bencode::Dict manifest{
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

std::string scrollKey(const Scroll &scroll) {
  if (scroll.isNamed()) {
    return scrollKeyFor(scroll.publisher, scroll.salt);
  }
  if (scroll.segments.empty()) {
    return {};
  }
  const auto &first = scroll.segments.front();
  return std::format("file:{}:{}", first.torrent.hex(), first.fileIndex);
}

std::string scrollKeyFor(const PublicKey &publisher, const std::string &salt) {
  return std::format("btpk:{}:{}", publisher.hex(), salt);
}

GlobalSpan GlobalSpan::intersect(const GlobalSpan &other) const {
  if (scroll != other.scroll) {
    return {};
  }
  const auto from = std::max(start, other.start);
  const auto to   = std::min(end(), other.end());
  if (to <= from) {
    return {};
  }
  return GlobalSpan{scroll, from, to - from};
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
  return MutableLink{publisher, salt, {}}.target();
}

std::string Publication::uri() const {
  return MutableLink{publisher, salt, {}}.uri();
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

  const auto *publisher = root.find(keyPublisher);
  const auto *salt      = root.find(keySalt);
  const auto *title     = root.find(keyTitle);
  const auto *version   = root.find(keyVersion);
  const auto *sequence  = root.find(keySequence);
  const auto *time      = root.find(keyTime);
  const auto *pieces    = root.find(keyPieces);
  const auto *links     = root.find(keyLinks);
  const auto *scrolls   = root.find(keyScrolls);
  const auto *signature = root.find(keySignature);
  if (nullptr == publisher || !publisher->isString() ||
      publisher->asString().size() != 32 || nullptr == salt ||
      !salt->isString() || nullptr == title || !title->isString() ||
      nullptr == version || !version->isString() || nullptr == sequence ||
      !sequence->isInteger() || nullptr == time || !time->isInteger() ||
      nullptr == pieces || nullptr == links || !links->isList() ||
      nullptr == scrolls || !scrolls->isDict() || nullptr == signature ||
      !signature->isString() || signature->asString().size() != 64) {
    return std::nullopt;
  }

  Publication pub;
  std::copy(publisher->asString().begin(), publisher->asString().end(),
            pub.publisher.bytes.begin());
  pub.salt      = salt->asString();
  pub.title     = title->asString();
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

  // Absent in a manifest published before the operations were sealed in --
  // not a decode failure, since the pieces still decode to a whole document
  // on their own; that manifest simply says nothing about the history behind
  // them.
  if (const auto *opsSegs = root.find(keyOpsSegs);
      nullptr != opsSegs && opsSegs->isList()) {
    for (const auto &item : opsSegs->asList()) {
      auto segment = decodeSegment(item);
      if (!segment) {
        return std::nullopt;
      }
      pub.opsSegments.push_back(std::move(*segment));
    }
  }

  if (const auto *holesVal = root.find(keyHoles);
      nullptr != holesVal && holesVal->isList()) {
    for (const auto &item : holesVal->asList()) {
      auto hole = decodeHole(item);
      if (!hole) {
        return std::nullopt;
      }
      pub.holes.push_back(std::move(*hole));
    }
  }

  // Checked last, over everything just read: a manifest that does not verify
  // is somebody's claim to have published what they did not, and the only
  // thing to do with it is to fail to read it.
  if (!verifyPublication(pub)) {
    return std::nullopt;
  }
  return pub;
}

SealedScroll sealLocalSpool(const Store &store, const MutableKeys &keys,
                            const std::string &salt, const std::string &into,
                            const SignedProvenance &provenance,
                            const Scroll &priorScroll,
                            const std::uint32_t opsAlreadySealed) {
  if (provenance.yaml.empty() || provenance.signature.empty()) {
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

  const auto opsTotal  = static_cast<std::uint32_t>(store.opCount());
  const bool hasNewOps = opsAlreadySealed < opsTotal;
  const auto newOps =
      hasNewOps ? sealableOps(store, opsAlreadySealed) : std::string{};

  // The content first, so a fresh segment's bytes begin at offset zero of its
  // own piece stream -- what keeps every address already handed out pointing
  // where it did. New operations, when there are any, follow it; the record
  // and its signature always come last, since they describe this seal rather
  // than being seal-specific content of their own.
  //
  // Only what is new is sealed: what @p priorScroll already carries is not
  // reread or rehashed here, only carried forward into the scroll this
  // returns. The operations are sealed in because a xanadoc is its history
  // and not the state it happens to have reached: a reader given the pieces
  // alone gets a document that cannot be gone back through, which is the one
  // thing this model exists to make possible. They ride in the torrent
  // rather than the manifest because they are bulk -- fetched in pieces, from
  // peers, only by a reader who wants them -- and because the info hash then
  // covers them, so operations that do not hash to what the reference names
  // cannot be passed off as the publisher's any more than the content can.
  std::vector<TorrentContent> files;
  if (hasNewPrimedia) {
    files.push_back(
        TorrentContent{sealedContentName, std::string{newPrimedia}});
  }
  if (hasNewOps) {
    files.push_back(TorrentContent{sealedOpsName, newOps});
  }
  files.push_back(TorrentContent{provenanceFileName, provenance.yaml});
  files.push_back(TorrentContent{provenanceSigName, provenance.signature});
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
    // A directory named as the torrent names it, holding the files it
    // describes, so that a seeder handed this finds what it expects.
    const std::filesystem::path dir = std::filesystem::path(into) / name;
    std::filesystem::create_directories(dir);
    {
      std::ofstream out(std::filesystem::path(into) / (name + ".torrent"),
                        std::ios::binary);
      out << sealed.torrentFile;
    }
    for (const auto &file : files) {
      std::ofstream out(dir / file.path, std::ios::binary);
      out << file.data;
    }
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

  const auto parentProv = parseProvenance(provenance.yaml);

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
    signedMediaProv.yaml      = mediaProv.toYaml();
    signedMediaProv.signature = provenance.signature;

    std::vector<TorrentContent> files;
    files.push_back(TorrentContent{name, data});
    files.push_back(TorrentContent{provenanceFileName, signedMediaProv.yaml});
    files.push_back(
        TorrentContent{provenanceSigName, signedMediaProv.signature});

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
        out << signedMediaProv.yaml;
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
  const auto *scroll    = root.find(keySealScroll);
  const auto *opsSealed = root.find(keySealOpsSealed);
  const auto *opsSegs   = root.find(keySealOpsSegs);
  if (nullptr == scroll || nullptr == opsSealed || !opsSealed->isInteger() ||
      opsSealed->asInteger() < 0 || nullptr == opsSegs || !opsSegs->isList()) {
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
  // A piece of the local spool has a global name exactly when the local spool
  // has been sealed: the offsets are the same bytes, so the sealed scroll's
  // name is the address it always had, said globally.
  const auto *const scroll =
      span.isLocal() ? localSealedAs : store.scroll(span.scroll);
  return nullptr == scroll ? std::string{} : scrollKey(*scroll);
}

std::optional<GlobalSpan> globalise(const Store &store,
                                    const PrimediaSpan &span,
                                    const Scroll *const localSealedAs) {
  auto key = globalKeyOf(store, span, localSealedAs);
  if (key.empty()) {
    return std::nullopt;
  }
  return GlobalSpan{std::move(key), span.start, span.length};
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
      return PrimediaSpan{static_cast<ScrollId>(i + 1), span.start,
                          span.length};
    }
  }
  const auto found = scrolls.find(span.scroll);
  if (scrolls.end() == found) {
    return std::nullopt;
  }
  return PrimediaSpan{store.addScroll(found->second), span.start, span.length};
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
    if (nullptr == node || node->scrollId == localScroll) {
      continue; // zero is the scroll being sealed; see sealableOps()'s comment
    }
    if (named.contains(node->scrollId)) {
      continue;
    }
    const auto *const scroll = store.scroll(node->scrollId);
    if (nullptr == scroll) {
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
    history.putOp(record.produces, record.op);
  }
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
    const auto same = [&link](const auto &entry) {
      return entry.second.type == link.type &&
             entry.second.owner == link.owner &&
             entry.second.left == link.left && entry.second.right == link.right;
    };
    if (std::ranges::any_of(store.links(), same)) {
      continue;
    }
    taken.version = store.addLink(taken.version, std::move(link));
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
                    const std::vector<ScrollSegment> &opsSegments) {
  Publication pub;
  pub.publisher   = keys.publicKey;
  pub.salt        = std::move(salt);
  pub.title       = std::move(title);
  pub.version     = version;
  pub.sequence    = sequence;
  pub.opsSegments = opsSegments;
  pub.published   = published;

  const auto document  = store.rebuild(version);
  const auto scrollFor = [&store, localSealedAs](const PrimediaSpan &span) {
    return span.isLocal() ? localSealedAs : store.scroll(span.scroll);
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
    pub.scrolls.insert_or_assign(global->scroll, *scrollFor(piece));
    pub.pieces.push_back(*global);
  }

  // The links whose ends this document actually shows. A store may hold links
  // about anything; what a publication carries are the ones that say something
  // about what it published.
  for (const auto &piece : document.pieces()) {
    for (const auto *link : store.linksTouching(piece)) {
      GlobalLink out;
      out.type               = link->type;
      out.owner              = link->owner;
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
      sayGlobally(link->left, out.left);
      sayGlobally(link->right, out.right);
      if (!addressable) {
        continue;
      }
      if (std::ranges::find(pub.links, out) == pub.links.end()) {
        pub.links.push_back(std::move(out));
      }
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
        out.push_back(Sighting{
            &pub, at + into,
            at + into + static_cast<std::uint32_t>(shared.length), shared});
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
        out.push_back(FoundLink{&pub, &link, true});
      } else if (onSide(link.right)) {
        out.push_back(FoundLink{&pub, &link, false});
      }
    }
  }
  return out;
}

Publication
publishDocument(Store &store, const MicroversionId &version,
                const MutableKeys &documentKeys, std::string salt,
                std::string title, const std::int64_t sequence,
                const std::uint64_t published,
                const SignedProvenance &permascrollProvenance,
                [[maybe_unused]] const SignedProvenance &documentProvenance,
                const std::string &torrentOutputDir) {
  if (!torrentOutputDir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(torrentOutputDir, ec);
  }
  if (store.userPermascrollPtr()) {
    store.userPermascrollPtr()->sealIncremental(torrentOutputDir,
                                                permascrollProvenance);
  }

  const auto userScroll = store.userPermascroll().currentScroll();
  return publish(store, version, documentKeys, std::move(salt),
                 std::move(title), sequence, published, &userScroll);
}

} // namespace xudu
