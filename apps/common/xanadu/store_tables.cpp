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
#include <set>

#include "bencode.hpp"
#include "scroll_codec.hpp"
#include "spool.hpp"

namespace xanadu {

namespace {

/// Keys of the container's dictionary, written once here so that the reader
/// and the writer cannot disagree about them.
constexpr auto keyLocalSegments  = "local";
constexpr auto keyDocumentId     = "document";
constexpr auto keyDeployment     = "deployment";
constexpr auto keyPublishedLocal = "published_local";

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
  if (const auto mime = value.find("mime");
      mime.has_value() && mime->isString()) {
    segment->mimeType = mime->asString();
  }
  return segment;
}

bencode::Value encodeDeployment(const Scroll &scroll) {
  bencode::List segments;
  for (const auto &segment : scroll.segments)
    segments.push_back(encodeRegistrySegment(segment));
  std::string publisherBytes(32, '\0');
  if (scroll.isNamed()) {
    std::copy(scroll.publisher->bytes.begin(), scroll.publisher->bytes.end(),
              publisherBytes.begin());
  }
  return bencode::Value::dict(
      {{"publisher", bencode::Value::string(std::move(publisherBytes))},
       {"salt", bencode::Value::string(scroll.salt)},
       {"mime", bencode::Value::string(scroll.defaultMimeType)},
       {"segments", bencode::Value::list(std::move(segments))}});
}

Scroll decodeDeployment(const bencode::Value &value) {
  const auto publisher = value.find("publisher");
  const auto salt      = value.find("salt");
  const auto mime      = value.find("mime");
  const auto segments  = value.find("segments");
  if (!publisher || !publisher->isString() ||
      publisher->asString().size() != 32 || !salt || !salt->isString() ||
      !mime || !mime->isString() || !segments || !segments->isList())
    throw StoreTablesUnreadable("invalid deployed scroll descriptor");
  Scroll scroll;
  PublicKey publisherKey;
  std::copy(publisher->asString().begin(), publisher->asString().end(),
            publisherKey.bytes.begin());
  if (!publisherKey.isZero()) scroll.publisher = publisherKey;
  scroll.salt            = salt->asString();
  scroll.defaultMimeType = mime->asString();
  for (const auto &item : segments->asList()) {
    const auto segment = decodeRegistrySegment(item);
    if (!segment || segment->length > UINT64_MAX - segment->at ||
        (!scroll.segments.empty() &&
         segment->at < scroll.segments.back().end()))
      throw StoreTablesUnreadable("invalid deployed scroll segment");
    scroll.segments.push_back(*segment);
  }
  return scroll;
}

} // namespace

std::vector<std::uint8_t> serializeStoreTables(const StoreTables &tables) {
  bencode::List local;
  local.reserve(tables.localSegments.size());
  for (const auto &segment : tables.localSegments) {
    local.push_back(encodeRegistrySegment(segment));
  }

  bencode::List deployment;
  for (const auto &scroll : tables.deployedScrolls)
    deployment.push_back(encodeDeployment(scroll));
  if (tables.publishedLocalScroll > tables.deployedScrolls.size())
    throw StoreTablesUnreadable("published local scroll is outside deployment");
  const auto body =
      bencode::Value::dict(
          {
              {keyDocumentId, bencode::Value::string(std::string{
                                  reinterpret_cast<const char *>(
                                      tables.documentId.bytes().data()),
                                  tables.documentId.bytes().size()})},
              {keyLocalSegments, bencode::Value::list(std::move(local))},
              {keyDeployment, bencode::Value::list(std::move(deployment))},
              {keyPublishedLocal,
               bencode::Value::integer(tables.publishedLocalScroll)},
          })
          .encode();

  std::vector<std::uint8_t> out;
  out.reserve(storeTablesSignature.size() + sizeof(std::uint32_t) +
              body.size());
  out.insert(out.end(), storeTablesSignature.begin(),
             storeTablesSignature.end());
  const auto version = storeTablesFormatVersion;
  const auto *vBytes = reinterpret_cast<const std::uint8_t *>(&version);
  out.insert(out.end(), vBytes, vBytes + sizeof(version));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

void writeStoreTables(const std::filesystem::path &path,
                      const StoreTables &tables) {
  const auto bytes = serializeStoreTables(tables);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw StoreTablesUnreadable("cannot write " + path.string());
  }
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    throw StoreTablesUnreadable("cannot write " + path.string());
  }
}

StoreTables deserializeStoreTables(std::span<const std::uint8_t> bytes) {
  constexpr auto prelude = storeTablesSignature.size() + sizeof(std::uint32_t);
  if (bytes.size() < prelude) {
    throw StoreTablesUnreadable("data is only " + std::to_string(bytes.size()) +
                                " bytes long, which is too short to hold a "
                                "store table header");
  }
  if (!std::equal(storeTablesSignature.begin(), storeTablesSignature.end(),
                  bytes.data())) {
    throw StoreTablesUnreadable(
        "data does not begin with the store table signature "
        "(\\x89XUDUTBL\\r\\n\\x1a\\n), so it is not a store's side tables");
  }
  std::uint32_t version = 0;
  std::memcpy(&version, bytes.data() + storeTablesSignature.size(),
              sizeof(version));
  if (version != storeTablesFormatVersion) {
    throw StoreTablesUnreadable("is store table format version " +
                                std::to_string(version) +
                                " and this build reads version " +
                                std::to_string(storeTablesFormatVersion));
  }

  bencode::Value decoded;
  try {
    const std::string_view sv(
        reinterpret_cast<const char *>(bytes.data() + prelude),
        bytes.size() - prelude);
    decoded = bencode::decode(sv);
  } catch (const std::exception &e) {
    throw StoreTablesUnreadable(
        std::string(
            "store tables have a header but nothing readable after it: ") +
        e.what());
  }
  if (!decoded.isDict()) {
    throw StoreTablesUnreadable(
        "store tables have a header but nothing readable after it");
  }

  StoreTables tables;
  const auto document = decoded.find(keyDocumentId);
  const auto local    = decoded.find(keyLocalSegments);
  if (!document || !document->isString() ||
      !DocumentId::fromBytes(document->asString(), tables.documentId))
    throw StoreTablesUnreadable(
        "store tables have an invalid document identity");
  if (!local || !local->isList())
    throw StoreTablesUnreadable(
        "store tables have an invalid local segment table");
  for (const auto &item : local->asList()) {
    const auto segment = decodeRegistrySegment(item);
    if (!segment)
      throw StoreTablesUnreadable(
          "store tables have a local segment they cannot read");
    tables.localSegments.push_back(*segment);
  }
  const auto deployment = decoded.find(keyDeployment);
  const auto binding    = decoded.find(keyPublishedLocal);
  if (!deployment || !deployment->isList() || !binding ||
      !binding->isInteger() || binding->asInteger() < 0 ||
      static_cast<std::uint64_t>(binding->asInteger()) >
          deployment->asList().size())
    throw StoreTablesUnreadable(
        "store tables have an invalid deployment binding");
  std::set<std::string> keys;
  for (const auto &item : deployment->asList()) {
    auto scroll    = decodeDeployment(item);
    const auto key = scrollKey(scroll);
    if (key && !keys.insert(*key).second)
      throw StoreTablesUnreadable(
          "store tables have duplicate deployed scrolls");
    tables.deployedScrolls.push_back(std::move(scroll));
  }
  tables.publishedLocalScroll = static_cast<ScrollId>(binding->asInteger());
  return tables;
}

StoreTables readStoreTables(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>()};
  return deserializeStoreTables(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
}

} // namespace xanadu
