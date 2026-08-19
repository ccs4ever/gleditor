/**
 * @file blessing.cpp
 * @brief Implementation of author endorsements / blessings of link packages.
 */
#include "blessing.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

#include "bencode.hpp"
#include "swarm.hpp"

namespace xudu {

namespace {

constexpr auto keyAuthor          = "author";
constexpr auto keyTargetDoc       = "doc";
constexpr auto keyEndorsedPackage = "pkg";
constexpr auto keyNote            = "note";
constexpr auto keySignature       = "sig";
constexpr auto keyTime            = "time";

std::string rawBytes(const PublicKey &key) {
  return std::string{reinterpret_cast<const char *>(key.bytes.data()),
                     key.bytes.size()};
}

std::string rawBytes(const Signature &sig) {
  return std::string{reinterpret_cast<const char *>(sig.bytes.data()),
                     sig.bytes.size()};
}

bencode::Dict dictOf(const Blessing &blessing, const bool includeSignature) {
  bencode::Dict dict = {
      {keyAuthor, bencode::Value::string(rawBytes(blessing.author))},
      {keyTargetDoc, bencode::Value::string(blessing.targetDocument)},
      {keyEndorsedPackage, bencode::Value::string(blessing.endorsedPackage)},
      {keyTime, bencode::Value::integer(static_cast<std::int64_t>(blessing.timestamp))},
  };
  if (!blessing.note.empty()) {
    dict.emplace(keyNote, bencode::Value::string(blessing.note));
  }
  if (includeSignature) {
    dict.emplace(keySignature,
                 bencode::Value::string(rawBytes(blessing.signature)));
  }
  return dict;
}

} // namespace

std::string Blessing::describe() const {
  return std::format("Blessing by {}… of pkg \"{}\" for doc \"{}\"",
                     author.hex().substr(0, 8), endorsedPackage, targetDocument);
}

std::string blessingSigningBuffer(const Blessing &blessing) {
  return bencode::Value::dict(dictOf(blessing, false)).encode();
}

std::string encodeBlessing(const Blessing &blessing) {
  return bencode::Value::dict(dictOf(blessing, true)).encode();
}

bool verifyBlessing(const Blessing &blessing) {
  return verifyMutableItem(blessingSigningBuffer(blessing), blessing.signature,
                           blessing.author);
}

std::optional<Blessing> decodeBlessing(const std::string_view encoded) {
  bencode::Value root;
  try {
    root = bencode::decode(encoded);
  } catch (const std::exception &) {
    return std::nullopt;
  }
  if (!root.isDict()) {
    return std::nullopt;
  }

  const auto *author    = root.find(keyAuthor);
  const auto *targetDoc = root.find(keyTargetDoc);
  const auto *endorsed  = root.find(keyEndorsedPackage);
  const auto *time      = root.find(keyTime);
  const auto *note      = root.find(keyNote);
  const auto *sig       = root.find(keySignature);

  if (nullptr == author || !author->isString() || author->asString().size() != 32 ||
      nullptr == targetDoc || !targetDoc->isString() ||
      nullptr == endorsed || !endorsed->isString() ||
      nullptr == time || !time->isInteger() ||
      nullptr == sig || !sig->isString() || sig->asString().size() != 64) {
    return std::nullopt;
  }

  Blessing blessing;
  std::copy(author->asString().begin(), author->asString().end(),
            blessing.author.bytes.begin());
  blessing.targetDocument  = targetDoc->asString();
  blessing.endorsedPackage = endorsed->asString();
  blessing.timestamp       = static_cast<std::uint64_t>(time->asInteger());
  std::copy(sig->asString().begin(), sig->asString().end(),
            blessing.signature.bytes.begin());
  if (nullptr != note && note->isString()) {
    blessing.note = note->asString();
  }

  return blessing;
}

Blessing createBlessing(const MutableKeys &keys,
                        std::string targetDocument,
                        std::string endorsedPackage,
                        std::string note,
                        const std::uint64_t timestamp) {
  Blessing blessing;
  blessing.author          = keys.publicKey;
  blessing.targetDocument  = std::move(targetDocument);
  blessing.endorsedPackage = std::move(endorsedPackage);
  blessing.note            = std::move(note);
  blessing.timestamp       = timestamp;
  blessing.signature = signMutableItem(blessingSigningBuffer(blessing), keys);
  return blessing;
}

} // namespace xudu
