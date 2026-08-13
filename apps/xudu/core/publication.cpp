/**
 * @file publication.cpp
 * @brief Implementation of the published, signed document manifest.
 */
#include "publication.hpp" // IWYU pragma: associated

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

#include <filesystem>
#include <fstream>

#include "bencode.hpp"
#include "store.hpp"
#include "swarm.hpp"

namespace xudu {

namespace {

/// Keys of the manifest dictionary. Short, and written once here so that a
/// reader and a writer cannot disagree about them.
constexpr auto keyLinks     = "links";
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

std::optional<std::vector<GlobalSpan>> decodeSpans(const bencode::Value &value) {
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
  return bencode::Value::dict({
      {"at", bencode::Value::integer(static_cast<std::int64_t>(segment.at))},
      {"file", bencode::Value::integer(segment.fileIndex)},
      {"len",
       bencode::Value::integer(static_cast<std::int64_t>(segment.length))},
      {"path", bencode::Value::string(segment.path)},
      {"stream",
       bencode::Value::integer(static_cast<std::int64_t>(segment.streamOffset))},
      {"torrent",
       bencode::Value::string(std::string{
           reinterpret_cast<const char *>(segment.torrent.bytes.data()),
           segment.torrent.bytes.size()})},
  });
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
  return bencode::Value::dict({
      {"left", encodeSpans(link.left)},
      {"owner", bencode::Value::string(link.owner)},
      {"right", encodeSpans(link.right)},
      {"type", bencode::Value::string(linkTypeName(link.type))},
  });
}

std::optional<GlobalLink> decodeLink(const bencode::Value &value) {
  const auto *type  = value.find("type");
  const auto *owner = value.find("owner");
  const auto *left  = value.find("left");
  const auto *right = value.find("right");
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

  bencode::Dict manifest{
      {keyLinks, bencode::Value::list(std::move(links))},
      {keyPieces, encodeSpans(pub.pieces)},
      {keyPublisher, bencode::Value::string(rawBytes(pub.publisher))},
      {keySalt, bencode::Value::string(pub.salt)},
      {keyScrolls, bencode::Value::dict(std::move(scrolls))},
      {keySequence, bencode::Value::integer(pub.sequence)},
      {keyTime, bencode::Value::integer(static_cast<std::int64_t>(pub.published))},
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
  if (!swarmSupported()) {
    return false;
  }
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

  // Checked last, over everything just read: a manifest that does not verify
  // is somebody's claim to have published what they did not, and the only
  // thing to do with it is to fail to read it.
  if (!verifyPublication(pub)) {
    return std::nullopt;
  }
  return pub;
}

SealedScroll sealLocalSpool(const Store &store, const MutableKeys &keys,
                            const std::string &salt, const std::string &into) {
  const auto &bytes = store.primedia().bytes();
  const auto name   = salt.empty() ? std::string{"primedia"} : salt;
  auto made         = makeTorrent(bytes, name);

  SealedScroll sealed;
  sealed.hash        = made.hash;
  sealed.torrentFile = std::move(made.file);

  sealed.scroll.publisher = keys.publicKey;
  sealed.scroll.salt      = salt;
  ScrollSegment segment;
  segment.at           = 0;
  segment.length       = bytes.size();
  segment.torrent      = sealed.hash;
  segment.streamOffset = 0;
  segment.fileIndex    = 0;
  segment.path         = name;
  sealed.scroll.segments.push_back(segment);

  if (!into.empty()) {
    const std::filesystem::path dir(into);
    std::filesystem::create_directories(dir);
    {
      std::ofstream out(dir / (name + ".torrent"), std::ios::binary);
      out << sealed.torrentFile;
    }
    {
      // The data as its own file, named as the torrent names it, so that a
      // seeder handed the directory finds what the torrent describes.
      std::ofstream out(dir / name, std::ios::binary);
      out << bytes;
    }
  }
  return sealed;
}

Publication publish(const Store &store, const MicroversionId &version,
                    const MutableKeys &keys, std::string salt,
                    std::string title, const std::int64_t sequence,
                    const std::uint64_t published,
                    const Scroll *const localSealedAs) {
  if (!swarmSupported()) {
    throw std::runtime_error(
        "publishing needs signing, which this build has no ed25519 for; "
        "rebuild with libtorrent-rasterbar installed");
  }

  Publication pub;
  pub.publisher = keys.publicKey;
  pub.salt      = std::move(salt);
  pub.title     = std::move(title);
  pub.version   = version;
  pub.sequence  = sequence;
  pub.published = published;

  const auto document = store.rebuild(version);
  for (const auto &piece : document.pieces()) {
    // A piece of the local spool is publishable exactly when the local spool
    // has been sealed: the offsets are the same bytes, so the sealed scroll's
    // name is the address it has always had, said globally.
    const auto *scroll =
        piece.isLocal() ? localSealedAs : store.scroll(piece.scroll);
    if (nullptr == scroll) {
      throw std::runtime_error(std::format(
          "cannot publish: {} bytes at {} are content this machine has not "
          "published; seal it into a scroll first",
          piece.length, piece.start));
    }
    const auto key = scrollKey(*scroll);
    if (key.empty()) {
      throw std::runtime_error(
          "cannot publish: a scroll with no name and no segments has no "
          "address a reader could resolve");
    }
    pub.pieces.push_back(GlobalSpan{key, piece.start, piece.length});
    pub.scrolls.insert_or_assign(key, *scroll);
  }

  // The links whose ends this document actually shows. A store may hold links
  // about anything; what a publication carries are the ones that say something
  // about what it published.
  for (const auto &piece : document.pieces()) {
    for (const auto *link : store.linksTouching(piece)) {
      GlobalLink out;
      out.type  = link->type;
      out.owner = link->owner;
      bool addressable = true;
      const auto globalise = [&](const std::vector<PrimediaSpan> &side,
                                 std::vector<GlobalSpan> &into) {
        for (const auto &span : side) {
          const auto *scroll =
              span.isLocal() ? localSealedAs : store.scroll(span.scroll);
          if (nullptr == scroll) {
            // An end nobody else could resolve. The link is dropped rather
            // than published half-addressed: half a link is a claim about a
            // passage that cannot be checked.
            addressable = false;
            return;
          }
          into.push_back(
              GlobalSpan{scrollKey(*scroll), span.start, span.length});
          pub.scrolls.insert_or_assign(scrollKey(*scroll), *scroll);
        }
      };
      globalise(link->left, out.left);
      globalise(link->right, out.right);
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
        const auto into = static_cast<std::uint32_t>(shared.start - piece.start);
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

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:
