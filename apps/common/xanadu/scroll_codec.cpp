/**
 * @file scroll_codec.cpp
 * @brief The shared scroll-segment encoding. See scroll_codec.hpp for why it
 *        lives here rather than beside its first caller.
 */
#include "scroll_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "identity/identity_layout.hpp"

namespace xanadu {

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

} // namespace xanadu
