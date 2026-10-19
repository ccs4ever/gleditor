#include "identity_serialization.hpp"

#include <charconv>
#include <iterator>

namespace xanadu::identity {
namespace {
bool boundedText(std::string_view text, std::size_t limit) {
  return !text.empty() && text.size() <= limit &&
         std::ranges::none_of(
             text, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

bool validInfoHash(std::string_view hash) {
  return (hash.size() == 40 || hash.size() == 64) &&
         std::ranges::all_of(hash, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

libtorrent::entry encodePid(const ActorPid &pid) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["node"]   = pid.node;
  e["cursor"] = pid.cursor;
  e["epoch"]  = std::to_string(pid.epoch);
  e["spawn"]  = std::to_string(pid.spawnSequence);
  return e;
}

std::expected<std::uint64_t, SerializationError>
decodeUnsigned(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::string_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }
  const auto raw = node.string_value();
  std::uint64_t value{};
  const auto [end, ec] =
      std::from_chars(raw.data(), raw.data() + raw.size(), value);
  if (raw.empty() || (raw.size() > 1 && raw[0] == '0') || ec != std::errc{} ||
      end != raw.data() + raw.size()) {
    return std::unexpected(SerializationError::InvalidActorMessage);
  }
  return value;
}

std::expected<ActorPid, SerializationError>
decodePid(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t ||
      node.dict_size() != 4) {
    return std::unexpected(SerializationError::InvalidActorPid);
  }
  const auto nodeName = node.dict_find_string("node");
  const auto cursor   = node.dict_find_string("cursor");
  const auto epoch    = decodeUnsigned(node.dict_find("epoch"));
  const auto spawn    = decodeUnsigned(node.dict_find("spawn"));
  if (!nodeName || !cursor || !epoch || !spawn) {
    return std::unexpected(SerializationError::InvalidActorPid);
  }
  const auto nameText   = nodeName.string_value();
  const auto cursorText = cursor.string_value();
  ActorPid pid{std::string(nameText.data(), nameText.size()),
               std::string(cursorText.data(), cursorText.size()), *epoch,
               *spawn};
  if (!pid.isValid()) {
    return std::unexpected(SerializationError::InvalidActorPid);
  }
  return pid;
}
} // namespace

bool ActorPid::isValid() const noexcept {
  return boundedText(node, 256) && boundedText(cursor, 128) && epoch != 0 &&
         spawnSequence != 0;
}

bool ActorMessage::isValid() const noexcept {
  if (!boundedText(requestId, 128) || (source && !source->isValid()) ||
      (target && !target->isValid()) || programRef.size() > 4096 ||
      hashcashStamp.size() > 4096 || capability.size() > 16384 ||
      reason.size() > 1024 || value.size() > 32768) {
    return false;
  }
  const bool noBulk = infoHash.empty() && sequence == 0;
  const bool noSpawn =
      programRef.empty() && hashcashStamp.empty() && capability.empty();
  switch (type) {
  case MessageType::ActorSpawn:
    return source && !target && boundedText(programRef, 4096) &&
           boundedText(hashcashStamp, 4096) && !capability.empty() &&
           value.empty() && reason.empty() && noBulk;
  case MessageType::ActorSpawnAck:
    return noSpawn && noBulk && value.empty() &&
           ((target && reason.empty()) || (!target && !reason.empty()));
  case MessageType::ActorSend:
  case MessageType::ActorDeliverValue:
    return source && target && noSpawn && noBulk && reason.empty();
  case MessageType::ActorMonitor:
    return source && target && noSpawn && noBulk && value.empty() &&
           reason.empty();
  case MessageType::ActorDown:
    return source && target && noSpawn && noBulk && value.empty() &&
           !reason.empty();
  case MessageType::ActorAck:
    return source && target && noSpawn && noBulk && value.empty() &&
           reason.empty();
  case MessageType::ActorDeliverWriteSet:
    return source && target && noSpawn && value.empty() && reason.empty() &&
           validInfoHash(infoHash);
  case MessageType::ActorCreditUpdate:
    return source && !target && noSpawn && infoHash.empty() && value.empty() &&
           reason.empty();
  case MessageType::ActorLeaseHeartbeat:
    return source && target && noSpawn && infoHash.empty() && value.empty() &&
           reason.empty();
  case MessageType::ActorCutPrune:
    return source && noSpawn && infoHash.empty() && value.empty() &&
           !reason.empty();
  case MessageType::ActorSparkSteal:
    return source && target && noSpawn && infoHash.empty() && !value.empty() &&
           !reason.empty();
  default:
    return false;
  }
}

std::expected<std::string, SerializationError>
encodeActorMessage(const ActorMessage &message) {
  if (!message.isValid()) {
    return std::unexpected(SerializationError::InvalidActorMessage);
  }
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["request"] = message.requestId;
  if (message.source) e["source"] = encodePid(*message.source);
  if (message.target) e["target"] = encodePid(*message.target);
  if (!message.programRef.empty()) e["program"] = message.programRef;
  if (!message.hashcashStamp.empty()) e["stamp"] = message.hashcashStamp;
  if (!message.capability.empty()) e["capability"] = message.capability;
  if (!message.value.empty()) e["value"] = message.value;
  if (!message.reason.empty()) e["reason"] = message.reason;
  if (!message.infoHash.empty()) {
    e["infohash"] = message.infoHash;
  }
  if (!message.infoHash.empty() || message.sequence != 0) {
    e["sequence"] = std::to_string(message.sequence);
  }
  std::string out(1, static_cast<char>(message.type));
  libtorrent::bencode(std::back_inserter(out), e);
  if (out.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  return out;
}

std::expected<ActorMessage, SerializationError>
decodeActorMessage(std::span<const std::uint8_t> bytes) {
  const auto frame = decodeExtendedMessage(bytes);
  if (!frame) return std::unexpected(frame.error());
  if (frame->type < MessageType::ActorSpawn ||
      frame->type > MessageType::ActorSparkSteal) {
    return std::unexpected(SerializationError::UnknownMessageType);
  }
  if (frame->payload.empty())
    return std::unexpected(SerializationError::InvalidBencode);
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const auto *begin = reinterpret_cast<const char *>(frame->payload.data());
  if (libtorrent::bdecode(begin, begin + frame->payload.size(), node, ec,
                          nullptr, 8, 128) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  if (static_cast<std::size_t>(node.data_section().size()) !=
      frame->payload.size()) {
    return std::unexpected(SerializationError::TrailingData);
  }
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }
  ActorMessage message;
  message.type = frame->type;
  for (int i = 0; i < node.dict_size(); ++i) {
    const auto [rawKey, field] = node.dict_at(i);
    const std::string_view key(rawKey.data(), rawKey.size());
    if (key == "source" || key == "target") {
      const auto pid = decodePid(field);
      if (!pid) return std::unexpected(pid.error());
      (key == "source" ? message.source : message.target) = *pid;
    } else if (key == "sequence") {
      const auto seq = decodeUnsigned(field);
      if (!seq) return std::unexpected(seq.error());
      message.sequence = *seq;
    } else {
      std::string *destination = nullptr;
      if (key == "request")
        destination = &message.requestId;
      else if (key == "program")
        destination = &message.programRef;
      else if (key == "stamp")
        destination = &message.hashcashStamp;
      else if (key == "capability")
        destination = &message.capability;
      else if (key == "value")
        destination = &message.value;
      else if (key == "infohash")
        destination = &message.infoHash;
      else if (key == "reason")
        destination = &message.reason;
      else
        return std::unexpected(SerializationError::InvalidActorMessage);
      if (field.type() != libtorrent::bdecode_node::string_t) {
        return std::unexpected(SerializationError::TypeMismatch);
      }
      const auto text = field.string_value();
      destination->assign(text.data(), text.size());
    }
  }
  if (!message.infoHash.empty() && !validInfoHash(message.infoHash)) {
    return std::unexpected(SerializationError::InvalidInfoHash);
  }
  if (!message.isValid())
    return std::unexpected(SerializationError::InvalidActorMessage);
  // Re-encoding also rejects duplicate keys, noncanonical integers and omitted
  // mandatory fields, leaving one representation for content-addressed dedup.
  const auto canonical = encodeActorMessage(message);
  if (!canonical || canonical->size() != bytes.size() ||
      !std::equal(canonical->begin(), canonical->end(), bytes.begin(),
                  [](char a, std::uint8_t b) {
                    return static_cast<std::uint8_t>(a) == b;
                  })) {
    return std::unexpected(SerializationError::InvalidActorMessage);
  }
  return message;
}
} // namespace xanadu::identity
