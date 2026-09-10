/**
 * @file store_tables.cpp
 * @brief The store's side tables, written and read as one container.
 */
#include "store_tables.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>

#include "bencode.hpp"
#include "scroll_codec.hpp"
#include "spool.hpp"

namespace xanadu {

namespace {

/// Keys of the container's dictionary, written once here so that the reader
/// and the writer cannot disagree about them.
constexpr auto keyScrolls       = "scrolls";
constexpr auto keyLocalSegments = "local";
constexpr auto keyLinks         = "links";
constexpr auto keyCurrent       = "current";
constexpr auto keyVersions      = "versions";

std::string rawBytes(const std::array<std::uint8_t, 32> &bytes) {
  return std::string{reinterpret_cast<const char *>(bytes.data()),
                     bytes.size()};
}

/// A registry segment: the shared encoding, plus the MIME type.
///
/// The shared codec has no key for a MIME type because a publication's
/// segments do not need one -- a manifest says what its content is elsewhere
/// -- but a registry's segments have nowhere else to get it, and the plaintext
/// table this replaces carried it. Added around the shared encoding rather
/// than inside it, so that a store's needs do not change the bytes a
/// publication is signed over.
bencode::Value encodeRegistrySegment(const ScrollSegment &segment) {
  auto dict = encodeSegment(segment).asDict();
  dict.emplace("mime", bencode::Value::string(segment.mimeType));
  return bencode::Value::dict(std::move(dict));
}

std::optional<ScrollSegment>
decodeRegistrySegment(const bencode::Value &value) {
  auto segment = decodeSegment(value);
  if (!segment.has_value()) {
    return std::nullopt;
  }
  if (const auto *mime = value.find("mime");
      nullptr != mime && mime->isString()) {
    segment->mimeType = mime->asString();
  }
  return segment;
}

/// A registry scroll, which is not a publication's scroll: it may have no
/// publisher at all -- Scroll::ofTorrentFile() makes those, and they are how
/// content that exists only as one fixed torrent is named -- and it carries
/// the default MIME type that a manifest has no use for.
bencode::Value encodeRegistryScroll(const Scroll &scroll) {
  bencode::List segments;
  segments.reserve(scroll.segments.size());
  for (const auto &segment : scroll.segments) {
    segments.push_back(encodeRegistrySegment(segment));
  }
  bencode::Dict dict = {
      {"mime", bencode::Value::string(scroll.defaultMimeType)},
      {"salt", bencode::Value::string(scroll.salt)},
      {"segments", bencode::Value::list(std::move(segments))},
  };
  // Omitted rather than written as thirty-two zero bytes, so that "this scroll
  // has no publisher" is the absence of a key rather than a value that has to
  // be recognised as meaning nothing.
  if (scroll.isNamed()) {
    dict.emplace("key",
                 bencode::Value::string(rawBytes(scroll.publisher.bytes)));
  }
  return bencode::Value::dict(std::move(dict));
}

std::optional<Scroll> decodeRegistryScroll(const bencode::Value &value) {
  if (!value.isDict()) {
    return std::nullopt;
  }
  const auto *salt     = value.find("salt");
  const auto *segments = value.find("segments");
  if (nullptr == salt || !salt->isString() || nullptr == segments ||
      !segments->isList()) {
    return std::nullopt;
  }
  Scroll scroll;
  scroll.salt = salt->asString();
  if (const auto *key = value.find("key");
      nullptr != key && key->isString() && key->asString().size() == 32) {
    std::copy(key->asString().begin(), key->asString().end(),
              scroll.publisher.bytes.begin());
  }
  if (const auto *mime = value.find("mime");
      nullptr != mime && mime->isString()) {
    scroll.defaultMimeType = mime->asString();
  }
  for (const auto &item : segments->asList()) {
    auto segment = decodeRegistrySegment(item);
    if (!segment.has_value()) {
      return std::nullopt;
    }
    scroll.segments.push_back(*segment);
  }
  return scroll;
}

/// A span in this store's own coordinates: a ScrollId rather than a global
/// key, which is what makes it a *local* table and not a publishable one.
bencode::Value encodeLocalSpan(const PrimediaSpan &span) {
  return bencode::Value::dict({
      {"len", bencode::Value::integer(static_cast<std::int64_t>(span.length))},
      {"scroll",
       bencode::Value::integer(static_cast<std::int64_t>(span.scroll))},
      {"start", bencode::Value::integer(static_cast<std::int64_t>(span.start))},
  });
}

std::optional<PrimediaSpan> decodeLocalSpan(const bencode::Value &value) {
  if (!value.isDict()) {
    return std::nullopt;
  }
  const auto *scroll = value.find("scroll");
  const auto *start  = value.find("start");
  const auto *length = value.find("len");
  if (nullptr == scroll || !scroll->isInteger() || nullptr == start ||
      !start->isInteger() || nullptr == length || !length->isInteger() ||
      start->asInteger() < 0 || length->asInteger() < 0 ||
      scroll->asInteger() < 0) {
    return std::nullopt;
  }
  return PrimediaSpan{static_cast<ScrollId>(scroll->asInteger()),
                      static_cast<std::uint64_t>(start->asInteger()),
                      static_cast<std::uint64_t>(length->asInteger())};
}

bencode::List encodeLocalSpans(const std::vector<PrimediaSpan> &spans) {
  bencode::List out;
  out.reserve(spans.size());
  for (const auto &span : spans) {
    out.push_back(encodeLocalSpan(span));
  }
  return out;
}

bool decodeLocalSpans(const bencode::Value &value,
                      std::vector<PrimediaSpan> &into) {
  if (!value.isList()) {
    return false;
  }
  for (const auto &item : value.asList()) {
    const auto span = decodeLocalSpan(item);
    if (!span.has_value()) {
      return false;
    }
    into.push_back(*span);
  }
  return true;
}

/// Carries `tier` and `curator`, which the plaintext table did not: a link
/// adopted from a third-party package came back as the reader's own
/// author-tier link with no curator, every time a store was saved and
/// reopened. Nothing had to change for that to be fixed except writing the
/// fields down.
bencode::Value encodeLink(const Link &link) {
  return bencode::Value::dict({
      {"curator", bencode::Value::string(link.curator)},
      {"id", bencode::Value::integer(static_cast<std::int64_t>(link.id))},
      {"left", bencode::Value::list(encodeLocalSpans(link.left))},
      {"owner", bencode::Value::string(link.owner)},
      {"right", bencode::Value::list(encodeLocalSpans(link.right))},
      {"tier",
       bencode::Value::string(std::string{prominenceTierName(link.tier)})},
      {"type", bencode::Value::string(std::string{linkTypeName(link.type)})},
  });
}

std::optional<Link> decodeLink(const bencode::Value &value) {
  if (!value.isDict()) {
    return std::nullopt;
  }
  const auto *id    = value.find("id");
  const auto *type  = value.find("type");
  const auto *left  = value.find("left");
  const auto *right = value.find("right");
  if (nullptr == id || !id->isInteger() || id->asInteger() < 0 ||
      nullptr == type || !type->isString() || nullptr == left ||
      nullptr == right) {
    return std::nullopt;
  }
  Link link;
  link.id   = static_cast<std::uint64_t>(id->asInteger());
  link.type = linkTypeFromName(type->asString());
  if (const auto *owner = value.find("owner");
      nullptr != owner && owner->isString()) {
    link.owner = owner->asString();
  }
  if (const auto *curator = value.find("curator");
      nullptr != curator && curator->isString()) {
    link.curator = curator->asString();
  }
  if (const auto *tier = value.find("tier");
      nullptr != tier && tier->isString()) {
    link.tier = prominenceTierFromName(tier->asString());
  }
  if (!decodeLocalSpans(*left, link.left) ||
      !decodeLocalSpans(*right, link.right)) {
    return std::nullopt;
  }
  return link;
}

/// A microversion, by its printed name rather than by its digits.
///
/// The name is the operation sequence -- "2a4" spells out the path that
/// rebuilds it -- so writing it as text is writing the thing itself, not a
/// rendering of it. It also survives a change to how a name is held in memory,
/// which a list of ordinals would not.
bencode::Value encodeAnnotation(const MicroversionId &id,
                                const VersionAnnotation &annotation) {
  return bencode::Value::dict({
      {"alias", bencode::Value::string(annotation.alias)},
      {"description", bencode::Value::string(annotation.description)},
      {"tag", bencode::Value::string(annotation.tag)},
      {"timestamp", bencode::Value::string(annotation.timestamp)},
      {"version", bencode::Value::string(id.str())},
  });
}

std::optional<std::pair<MicroversionId, VersionAnnotation>>
decodeAnnotation(const bencode::Value &value) {
  if (!value.isDict()) {
    return std::nullopt;
  }
  const auto *version = value.find("version");
  if (nullptr == version || !version->isString()) {
    return std::nullopt;
  }
  MicroversionId id;
  try {
    id = MicroversionId::parse(version->asString());
  } catch (const std::exception &) {
    return std::nullopt;
  }
  VersionAnnotation annotation;
  const auto text = [&value](const char *const key) -> std::string {
    const auto *found = value.find(key);
    return nullptr != found && found->isString() ? found->asString()
                                                 : std::string{};
  };
  annotation.alias       = text("alias");
  annotation.description = text("description");
  annotation.tag         = text("tag");
  annotation.timestamp   = text("timestamp");
  return std::pair{id, std::move(annotation)};
}

} // namespace

void writeStoreTables(const std::filesystem::path &path,
                      const StoreTables &tables) {
  bencode::List scrolls;
  scrolls.reserve(tables.scrolls.size());
  for (const auto &scroll : tables.scrolls) {
    scrolls.push_back(encodeRegistryScroll(scroll));
  }
  bencode::List local;
  local.reserve(tables.localSegments.size());
  for (const auto &segment : tables.localSegments) {
    local.push_back(encodeRegistrySegment(segment));
  }
  bencode::List links;
  links.reserve(tables.links.size());
  for (const auto &[id, link] : tables.links) {
    links.push_back(encodeLink(link));
  }
  bencode::List current;
  current.reserve(tables.currentVersions.size());
  for (const auto &id : tables.currentVersions) {
    current.push_back(bencode::Value::string(id.str()));
  }
  bencode::List versions;
  versions.reserve(tables.versionAnnotations.size());
  for (const auto &[id, annotation] : tables.versionAnnotations) {
    versions.push_back(encodeAnnotation(id, annotation));
  }

  const auto body =
      bencode::Value::dict(
          {
              {keyCurrent, bencode::Value::list(std::move(current))},
              {keyLinks, bencode::Value::list(std::move(links))},
              {keyLocalSegments, bencode::Value::list(std::move(local))},
              {keyScrolls, bencode::Value::list(std::move(scrolls))},
              {keyVersions, bencode::Value::list(std::move(versions))},
          })
          .encode();

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw StoreTablesUnreadable("cannot write " + path.string());
  }
  out.write(reinterpret_cast<const char *>(storeTablesSignature.data()),
            static_cast<std::streamsize>(storeTablesSignature.size()));
  const auto version = storeTablesFormatVersion;
  out.write(reinterpret_cast<const char *>(&version), sizeof(version));
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (!out) {
    throw StoreTablesUnreadable("cannot write " + path.string());
  }
}

StoreTables readStoreTables(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>()};

  constexpr auto prelude = storeTablesSignature.size() + sizeof(std::uint32_t);
  if (bytes.size() < prelude) {
    throw StoreTablesUnreadable(path.string() + " is only " +
                                std::to_string(bytes.size()) +
                                " bytes long, which is too short to hold a "
                                "store table header");
  }
  if (!std::equal(storeTablesSignature.begin(), storeTablesSignature.end(),
                  reinterpret_cast<const std::uint8_t *>(bytes.data()))) {
    throw StoreTablesUnreadable(
        path.string() +
        " does not begin with the store table signature "
        "(\\x89XUDUTBL\\r\\n\\x1a\\n), so it is not a store's side tables");
  }
  std::uint32_t version = 0;
  std::memcpy(&version, bytes.data() + storeTablesSignature.size(),
              sizeof(version));
  if (version != storeTablesFormatVersion) {
    throw StoreTablesUnreadable(
        path.string() + " is store table format version " +
        std::to_string(version) + " and this build reads version " +
        std::to_string(storeTablesFormatVersion));
  }

  bencode::Value decoded;
  try {
    decoded = bencode::decode(std::string_view{bytes}.substr(prelude));
  } catch (const std::exception &e) {
    throw StoreTablesUnreadable(
        path.string() +
        " has a header but nothing readable after it: " + e.what());
  }
  if (!decoded.isDict()) {
    throw StoreTablesUnreadable(path.string() +
                                " has a header but nothing readable after it");
  }

  StoreTables tables;
  if (const auto *scrolls = decoded.find(keyScrolls);
      nullptr != scrolls && scrolls->isList()) {
    for (const auto &item : scrolls->asList()) {
      auto scroll = decodeRegistryScroll(item);
      if (!scroll.has_value()) {
        throw StoreTablesUnreadable(path.string() +
                                    " has a scroll it cannot read");
      }
      tables.scrolls.push_back(std::move(*scroll));
    }
  }
  if (const auto *local = decoded.find(keyLocalSegments);
      nullptr != local && local->isList()) {
    for (const auto &item : local->asList()) {
      auto segment = decodeRegistrySegment(item);
      if (!segment.has_value()) {
        throw StoreTablesUnreadable(path.string() +
                                    " has a local segment it cannot read");
      }
      tables.localSegments.push_back(*segment);
    }
  }
  if (const auto *links = decoded.find(keyLinks);
      nullptr != links && links->isList()) {
    for (const auto &item : links->asList()) {
      auto link = decodeLink(item);
      if (!link.has_value()) {
        throw StoreTablesUnreadable(path.string() +
                                    " has a link it cannot read");
      }
      tables.links.emplace(link->id, std::move(*link));
    }
  }
  // A name that will not parse is refused rather than skipped. These say which
  // state the author is looking at and what they called it, so dropping one
  // quietly reopens the document somewhere else with no account of why.
  if (const auto *current = decoded.find(keyCurrent);
      nullptr != current && current->isList()) {
    for (const auto &item : current->asList()) {
      if (!item.isString()) {
        throw StoreTablesUnreadable(
            path.string() + " has a current version that is not a name");
      }
      try {
        tables.currentVersions.push_back(
            MicroversionId::parse(item.asString()));
      } catch (const std::exception &e) {
        throw StoreTablesUnreadable(path.string() + " has \"" +
                                    item.asString() +
                                    "\" as a current version, which is not a "
                                    "microversion name: " +
                                    e.what());
      }
    }
  }
  if (const auto *versions = decoded.find(keyVersions);
      nullptr != versions && versions->isList()) {
    for (const auto &item : versions->asList()) {
      auto annotation = decodeAnnotation(item);
      if (!annotation.has_value()) {
        throw StoreTablesUnreadable(path.string() +
                                    " has a version annotation it cannot read");
      }
      tables.versionAnnotations.emplace(annotation->first,
                                        std::move(annotation->second));
    }
  }
  return tables;
}

} // namespace xanadu
