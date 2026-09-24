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
constexpr auto keyLocalSegments = "local";
constexpr auto keyDocumentId    = "document";

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

} // namespace

void writeStoreTables(const std::filesystem::path &path,
                      const StoreTables &tables) {
  bencode::List local;
  local.reserve(tables.localSegments.size());
  for (const auto &segment : tables.localSegments) {
    local.push_back(encodeRegistrySegment(segment));
  }

  const auto body =
      bencode::Value::dict(
          {
              {keyDocumentId, bencode::Value::string(std::string{
                                  reinterpret_cast<const char *>(
                                      tables.documentId.bytes().data()),
                                  tables.documentId.bytes().size()})},
              {keyLocalSegments, bencode::Value::list(std::move(local))},
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
  if (version != 2 && version != storeTablesFormatVersion) {
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
  if (const auto *document = decoded.find(keyDocumentId); nullptr != document) {
    if (!document->isString() ||
        !DocumentId::fromBytes(document->asString(), tables.documentId)) {
      throw StoreTablesUnreadable(path.string() +
                                  " has a document identity it cannot read");
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

  return tables;
}

} // namespace xanadu
