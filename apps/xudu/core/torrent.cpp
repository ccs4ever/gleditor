#include "torrent.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

#include <glibmm/checksum.h>

#include "bencode.hpp"

namespace xudu {

namespace {

/// Hex digit value, or -1.
int hexValue(const char chr) {
  if (chr >= '0' && chr <= '9') {
    return chr - '0';
  }
  if (chr >= 'a' && chr <= 'f') {
    return (chr - 'a') + 10;
  }
  if (chr >= 'A' && chr <= 'F') {
    return (chr - 'A') + 10;
  }
  return -1;
}

/// A required entry of a dictionary, complained about by name when missing.
const bencode::Value &require(const bencode::Value &dict,
                              const std::string_view key,
                              const std::string_view what) {
  const auto *const found = dict.find(key);
  if (nullptr == found) {
    throw std::runtime_error("torrent: " + std::string{what} + " has no \"" +
                             std::string{key} + "\"");
  }
  return *found;
}

} // namespace

namespace {

/// One SHA-1 per piece over @p stream, which is the concatenation of whatever
/// files the torrent holds.
std::string pieceHashesOf(const std::string_view stream,
                          const std::uint64_t pieceLength) {
  std::string pieces;
  for (std::uint64_t at = 0; at < stream.size(); at += pieceLength) {
    const auto span =
        stream.substr(at, static_cast<std::size_t>(std::min<std::uint64_t>(
                              pieceLength, stream.size() - at)));
    const auto hash = sha1(span);
    pieces.append(reinterpret_cast<const char *>(hash.data()), hash.size());
  }
  return pieces;
}

/// Wrap an info dictionary as a .torrent, and take the name from its encoding.
MadeTorrent fromInfo(const bencode::Value &info) {
  // The hash is over the info dictionary's encoding, which is what the spec
  // asks for and what every other client will compute.
  MadeTorrent made;
  made.hash.bytes = sha1(info.encode());
  made.file       = bencode::Value::dict({{"info", info}}).encode();
  return made;
}

} // namespace

MadeTorrent makeTorrent(const std::string_view data, std::string name,
                        const std::uint64_t pieceLength) {
  if (0 == pieceLength) {
    throw std::invalid_argument("makeTorrent: zero piece length");
  }

  return fromInfo(bencode::Value::dict({
      {"length",
       bencode::Value::integer(static_cast<std::int64_t>(data.size()))},
      {"name", bencode::Value::string(std::move(name))},
      {"piece length",
       bencode::Value::integer(static_cast<std::int64_t>(pieceLength))},
      {"pieces", bencode::Value::string(pieceHashesOf(data, pieceLength))},
  }));
}

MadeTorrent makeTorrent(const std::span<const TorrentContent> files,
                        std::string name, const std::uint64_t pieceLength) {
  if (0 == pieceLength) {
    throw std::invalid_argument("makeTorrent: zero piece length");
  }
  if (files.empty()) {
    throw std::invalid_argument("makeTorrent: no files");
  }

  bencode::List entries;
  entries.reserve(files.size());
  std::string stream;
  for (const auto &file : files) {
    if (file.path.empty() ||
        file.path.find_first_of("/\\") != std::string::npos) {
      throw std::invalid_argument(
          "makeTorrent: file names must be plain names within the torrent's "
          "directory, got: " +
          file.path);
    }
    entries.push_back(bencode::Value::dict({
        {"length",
         bencode::Value::integer(static_cast<std::int64_t>(file.data.size()))},
        {"path", bencode::Value::list({bencode::Value::string(file.path)})},
    }));
    // Pieces run over the concatenation in the order given, so this is also
    // what fixes each file's offset within the stream.
    stream += file.data;
  }

  return fromInfo(bencode::Value::dict({
      {"files", bencode::Value::list(std::move(entries))},
      {"name", bencode::Value::string(std::move(name))},
      {"piece length",
       bencode::Value::integer(static_cast<std::int64_t>(pieceLength))},
      {"pieces", bencode::Value::string(pieceHashesOf(stream, pieceLength))},
  }));
}

std::array<std::uint8_t, 20> sha1(const std::string_view data) {
  // glib's checksum rather than a hand-rolled SHA-1: glibmm is already a hard
  // dependency of this tree, and a wrong hash here would not be a bug in this
  // program so much as a permanent corruption of every address it hands out.
  Glib::Checksum checksum(Glib::Checksum::Type::SHA1);
  checksum.update(reinterpret_cast<const guchar *>(data.data()), data.size());

  std::array<std::uint8_t, 20> out{};
  gsize size = out.size();
  checksum.get_digest(out.data(), &size);
  if (size != out.size()) {
    throw std::runtime_error("torrent: sha1 produced " + std::to_string(size) +
                             " bytes");
  }
  return out;
}

std::string toHex(const std::string_view bytes) {
  static constexpr std::string_view digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto chr : bytes) {
    const auto byte = static_cast<std::uint8_t>(chr);
    out.push_back(digits[byte >> 4U]);
    out.push_back(digits[byte & 0x0FU]);
  }
  return out;
}

std::string fromHex(const std::string_view text) {
  if (0 != text.size() % 2) {
    throw std::runtime_error("hex: \"" + std::string{text} +
                             "\" has an odd number of digits");
  }
  std::string out;
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int high = hexValue(text[i]);
    const int low  = hexValue(text[i + 1]);
    if (high < 0 || low < 0) {
      throw std::runtime_error("hex: \"" + std::string{text} + "\" is not hex");
    }
    out.push_back(static_cast<char>((high << 4) | low));
  }
  return out;
}

std::string InfoHash::hex() const {
  return toHex({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
}

InfoHash InfoHash::fromHex(const std::string_view text) {
  if (text.size() != 40) {
    throw std::runtime_error("info hash: expected 40 hex digits, got " +
                             std::to_string(text.size()));
  }
  InfoHash hash;
  for (std::size_t i = 0; i < hash.bytes.size(); i++) {
    const int high = hexValue(text[i * 2]);
    const int low  = hexValue(text[(i * 2) + 1]);
    if (high < 0 || low < 0) {
      throw std::runtime_error("info hash: \"" + std::string{text} +
                               "\" is not hex");
    }
    hash.bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return hash;
}

bool InfoHash::isZero() const {
  return std::ranges::all_of(bytes,
                             [](const std::uint8_t byte) { return 0 == byte; });
}

Metainfo Metainfo::parse(const std::string_view torrentFile) {
  const auto root = bencode::decode(torrentFile);
  if (!root.isDict()) {
    throw std::runtime_error("torrent: the top level is not a dictionary");
  }
  const auto &info = require(root, "info", "torrent");
  if (!info.isDict()) {
    throw std::runtime_error("torrent: \"info\" is not a dictionary");
  }

  Metainfo meta;
  // Over the bytes as they were found. See the note in bencode.hpp: hashing a
  // re-encoding would be hashing something only probably identical, and the
  // difference is every reference to this content resolving or not.
  meta.infoHash.bytes = sha1(info.rawEncoding());
  meta.torrentName    = require(info, "name", "info").asString();

  const auto pieceLength = require(info, "piece length", "info").asInteger();
  if (pieceLength <= 0) {
    throw std::runtime_error("torrent: \"piece length\" is not positive");
  }
  meta.bytesPerPiece = static_cast<std::uint64_t>(pieceLength);

  const auto &pieces = require(info, "pieces", "info").asString();
  if (0 != pieces.size() % 20) {
    throw std::runtime_error(
        "torrent: \"pieces\" is " + std::to_string(pieces.size()) +
        " bytes, which is not a whole number of 20-byte hashes");
  }
  meta.pieceHashes.reserve(pieces.size() / 20);
  for (std::size_t at = 0; at < pieces.size(); at += 20) {
    std::array<std::uint8_t, 20> hash{};
    std::copy_n(reinterpret_cast<const std::uint8_t *>(pieces.data()) + at,
                hash.size(), hash.begin());
    meta.pieceHashes.push_back(hash);
  }

  // Single file or many. "For the purposes of the other keys, the multi-file
  // case is treated as only having a single file by concatenating the files in
  // the order they appear in the files list", so the offsets accumulate and a
  // piece can straddle a boundary.
  std::uint64_t offset = 0;
  if (const auto *const single = info.find("length"); nullptr != single) {
    const auto length = single->asInteger();
    if (length < 0) {
      throw std::runtime_error("torrent: \"length\" is negative");
    }
    meta.entries.push_back(
        TorrentFile{meta.torrentName, static_cast<std::uint64_t>(length), 0});
    offset = static_cast<std::uint64_t>(length);
  } else {
    for (const auto &entry : require(info, "files", "info").asList()) {
      const auto length = require(entry, "length", "file entry").asInteger();
      if (length < 0) {
        throw std::runtime_error("torrent: a file has a negative \"length\"");
      }
      std::string path;
      for (const auto &part : require(entry, "path", "file entry").asList()) {
        if (!path.empty()) {
          path += '/';
        }
        path += part.asString();
      }
      meta.entries.push_back(
          TorrentFile{path, static_cast<std::uint64_t>(length), offset});
      offset += static_cast<std::uint64_t>(length);
    }
  }

  // The piece count has to account for the content, or a reference near the
  // end would name a piece that does not exist and could never be verified.
  const auto expected =
      0 == offset ? 0
                  : ((offset + meta.bytesPerPiece - 1) / meta.bytesPerPiece);
  if (expected != meta.pieceHashes.size()) {
    throw std::runtime_error(
        "torrent: " + std::to_string(meta.pieceHashes.size()) +
        " piece hashes for content needing " + std::to_string(expected));
  }

  return meta;
}

std::uint64_t Metainfo::totalLength() const {
  return entries.empty() ? 0 : entries.back().offset + entries.back().length;
}

std::optional<std::uint32_t>
Metainfo::fileIndex(const std::string_view path) const {
  for (std::size_t i = 0; i < entries.size(); i++) {
    if (entries[i].path == path) {
      return static_cast<std::uint32_t>(i);
    }
  }
  return std::nullopt;
}

const std::array<std::uint8_t, 20> &
Metainfo::pieceHash(const std::size_t index) const {
  if (index >= pieceHashes.size()) {
    throw std::runtime_error("torrent: no piece " + std::to_string(index));
  }
  return pieceHashes[index];
}

std::uint64_t Metainfo::lengthOfPiece(const std::size_t index) const {
  if (index >= pieceHashes.size()) {
    throw std::runtime_error("torrent: no piece " + std::to_string(index));
  }
  const auto begin = static_cast<std::uint64_t>(index) * bytesPerPiece;
  return std::min(bytesPerPiece, totalLength() - begin);
}

std::pair<std::size_t, std::size_t>
Metainfo::piecesForRange(const std::uint64_t offset,
                         const std::uint64_t length) const {
  if (0 == length) {
    return {0, 0};
  }
  const auto last = std::min(offset + length, totalLength());
  if (last <= offset) {
    return {0, 0};
  }
  const auto first = offset / bytesPerPiece;
  // The piece holding the final byte, then one past it: a range ending exactly
  // on a boundary must not claim the piece after it.
  const auto end = ((last - 1) / bytesPerPiece) + 1;
  return {static_cast<std::size_t>(first), static_cast<std::size_t>(end)};
}

bool Metainfo::verifyPiece(const std::size_t index,
                           const std::string_view data) const {
  if (index >= pieceHashes.size()) {
    return false;
  }
  // A short read is not a partial answer, it is different content. Hashing it
  // anyway would produce a mismatch, but checking the length first says why.
  if (data.size() != lengthOfPiece(index)) {
    return false;
  }
  return sha1(data) == pieceHashes[index];
}

namespace {

/// Undo percent-encoding. A malformed escape is left as it was found rather
/// than dropped, since a tracker URL that is slightly wrong is more use than
/// one that is silently shorter.
std::string percentDecode(const std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); i++) {
    if ('+' == text[i]) {
      out.push_back(' ');
    } else if ('%' == text[i] && i + 2 < text.size() &&
               hexValue(text[i + 1]) >= 0 && hexValue(text[i + 2]) >= 0) {
      out.push_back(static_cast<char>((hexValue(text[i + 1]) << 4) |
                                      hexValue(text[i + 2])));
      i += 2;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

/// Decode the thirty-two character base-32 form of an info hash, which older
/// magnet links use. RFC 4648 alphabet, no padding: 32 characters of five bits
/// each are exactly the 160 bits of a SHA-1.
InfoHash fromBase32(const std::string_view text) {
  InfoHash hash;
  std::uint32_t buffer = 0;
  int bits             = 0;
  std::size_t out      = 0;
  for (const auto chr : text) {
    int value = 0;
    if (chr >= 'A' && chr <= 'Z') {
      value = chr - 'A';
    } else if (chr >= 'a' && chr <= 'z') {
      value = chr - 'a';
    } else if (chr >= '2' && chr <= '7') {
      value = (chr - '2') + 26;
    } else {
      throw std::runtime_error("info hash: \"" + std::string{text} +
                               "\" is not base32");
    }
    buffer = (buffer << 5U) | static_cast<std::uint32_t>(value);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      if (out >= hash.bytes.size()) {
        throw std::runtime_error("info hash: base32 form is too long");
      }
      hash.bytes[out++] = static_cast<std::uint8_t>((buffer >> bits) & 0xFFU);
    }
  }
  if (out != hash.bytes.size()) {
    throw std::runtime_error("info hash: base32 form decoded to " +
                             std::to_string(out) + " bytes, not 20");
  }
  return hash;
}

/// Expand BEP 53's "select only" syntax: a comma-separated list of indices and
/// `first-last` ranges.
std::vector<std::uint32_t> parseSelectOnly(const std::string_view text) {
  std::vector<std::uint32_t> selected;
  std::size_t at = 0;
  while (at <= text.size()) {
    const auto comma = std::min(text.find(',', at), text.size());
    const auto item  = text.substr(at, comma - at);
    if (!item.empty()) {
      const auto dash = item.find('-');
      try {
        if (std::string_view::npos == dash) {
          selected.push_back(
              static_cast<std::uint32_t>(std::stoul(std::string{item})));
        } else {
          const auto first = static_cast<std::uint32_t>(
              std::stoul(std::string{item.substr(0, dash)}));
          const auto last = static_cast<std::uint32_t>(
              std::stoul(std::string{item.substr(dash + 1)}));
          for (auto index = first; index <= last; index++) {
            selected.push_back(index);
          }
        }
      } catch (const std::exception &) {
        // A malformed selector narrows nothing rather than failing the whole
        // link: the info hash is the part that matters and it is still good.
      }
    }
    at = comma + 1;
  }
  return selected;
}

} // namespace

void forEachMagnetParameter(
    const std::string_view uri,
    const std::function<void(std::string_view, std::string)> &visit) {
  if (!uri.starts_with("magnet:?")) {
    return;
  }
  const auto query = uri.substr(std::string_view{"magnet:?"}.size());

  std::size_t at = 0;
  while (at <= query.size()) {
    const auto amp    = std::min(query.find('&', at), query.size());
    const auto field  = query.substr(at, amp - at);
    at                = amp + 1;
    const auto equals = field.find('=');
    if (std::string_view::npos == equals) {
      continue;
    }
    visit(field.substr(0, equals), percentDecode(field.substr(equals + 1)));
  }
}

bool MagnetLink::looksLikeMagnet(const std::string_view text) {
  return text.starts_with("magnet:?");
}

MagnetLink MagnetLink::parse(const std::string_view uri) {
  if (!looksLikeMagnet(uri)) {
    throw std::runtime_error("not a magnet link: " + std::string{uri});
  }

  MagnetLink link;
  bool haveHash = false;
  forEachMagnetParameter(uri, [&link, &haveHash](const std::string_view key,
                                                 const std::string &value) {
    if ("xt" == key) {
      // Several xt parameters are allowed, and a v2 torrent carries both a
      // btih and a btmh. Take the v1 hash and ignore the rest: this addresses
      // content by SHA-1, and pretending to understand a SHA-256 merkle root
      // would mean being unable to verify anything named by it.
      constexpr std::string_view prefix = "urn:btih:";
      if (value.starts_with(prefix)) {
        const auto digits = std::string_view{value}.substr(prefix.size());
        link.hash         = 40 == digits.size() ? InfoHash::fromHex(digits)
                                                : fromBase32(digits);
        haveHash          = true;
      }
    } else if ("dn" == key) {
      link.displayName = value;
    } else if ("tr" == key) {
      link.trackers.push_back(value);
    } else if ("so" == key) {
      link.selectedFiles = parseSelectOnly(value);
    }
  });

  if (!haveHash) {
    throw std::runtime_error(
        "magnet link names no BitTorrent v1 info hash (urn:btih:): " +
        std::string{uri});
  }
  return link;
}

std::string Metainfo::magnet() const {
  // The info hash is the whole of the reference; the name is a display hint
  // and the absence of a tracker is deliberate, since a swarm is found through
  // the DHT as readily as through one.
  return "magnet:?xt=urn:btih:" + infoHash.hex() + "&dn=" + torrentName;
}

} // namespace xudu
