#include "common/xanadu/vortex/mesh_checkpoint.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/identity/identity_serialization.hpp"

namespace zigzag::vortex::mesh {
namespace {
using V                                        = xanadu::bencode::Value;
constexpr std::size_t maximumContinuationBytes = 4 * 1024 * 1024;

const V &field(const V &value, std::string_view name) {
  if (!value.isDict())
    throw std::runtime_error("checkpoint: dictionary expected");
  const auto it = value.asDict().find(std::string(name));
  if (it == value.asDict().end())
    throw std::runtime_error("checkpoint: missing field");
  return it->second;
}
const std::string &text(const V &value, std::string_view name) {
  return field(value, name).asString();
}
V pidValue(const ActorPid &pid) {
  return V::dict({{"node", V::string(pid.node)},
                  {"cursor", V::string(pid.cursor)},
                  {"epoch", V::string(std::to_string(pid.epoch))},
                  {"spawn", V::string(std::to_string(pid.spawnSequence))}});
}
std::uint64_t number(const V &value, std::string_view name) {
  const auto &input = text(value, name);
  std::uint64_t out = 0;
  const auto [end, ec] =
      std::from_chars(input.data(), input.data() + input.size(), out);
  if (ec != std::errc{} || end != input.data() + input.size() ||
      std::to_string(out) != input)
    throw std::runtime_error("checkpoint: invalid counter");
  return out;
}
std::expected<ActorMessage, std::string> message(const std::string &encoded) {
  auto parsed = xanadu::identity::decodeActorMessage(
      {reinterpret_cast<const std::uint8_t *>(encoded.data()), encoded.size()});
  if (!parsed) return std::unexpected("checkpoint: invalid remembered message");
  return *parsed;
}
} // namespace

std::expected<std::string, std::string>
encodeContinuation(const ActorContinuation &value) {
  if (!value.pid.isValid() || value.programRef.empty() ||
      value.baseRef.empty() || !value.spawnRequest.isValid() ||
      value.spawnRequest.type != xanadu::identity::MessageType::ActorSpawn ||
      value.spawnRequest.programRef != value.programRef)
    return std::unexpected("checkpoint: invalid message-boundary continuation");
  const auto spawn = xanadu::identity::encodeActorMessage(value.spawnRequest);
  if (!spawn) return std::unexpected("checkpoint: invalid spawn request");
  xanadu::bencode::List records;
  for (const auto &record : value.completed) {
    if (record.requestId.empty() || record.requestId.size() > 128 ||
        record.contentHash.size() != 64 ||
        !std::ranges::all_of(record.contentHash, [](unsigned char ch) {
          return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        }))
      return std::unexpected(
          "checkpoint: pending messages cannot be checkpointed");
    std::string encodedResponse;
    if (record.response) {
      auto response = xanadu::identity::encodeActorMessage(*record.response);
      if (!response)
        return std::unexpected("checkpoint: invalid completed response");
      encodedResponse = std::move(*response);
    }
    records.push_back(V::dict({{"request", V::string(record.requestId)},
                               {"hash", V::string(record.contentHash)},
                               {"response", V::string(encodedResponse)}}));
  }
  auto encoded = V::dict({{"version", V::integer(1)},
                          {"resume", V::string("await-message")},
                          {"pid", pidValue(value.pid)},
                          {"program", V::string(value.programRef)},
                          {"base", V::string(value.baseRef)},
                          {"spawn", V::string(*spawn)},
                          {"completed", V::list(std::move(records))}})
                     .encode();
  if (encoded.size() > maximumContinuationBytes)
    return std::unexpected("checkpoint: continuation byte budget exceeded");
  return encoded;
}

std::expected<ActorContinuation, std::string>
decodeContinuation(std::string_view bytes, std::size_t maximumRecords) {
  try {
    if (bytes.size() > maximumContinuationBytes)
      return std::unexpected("checkpoint: continuation byte budget exceeded");
    const auto value = xanadu::bencode::decode(bytes);
    if (!value.isDict() || value.asDict().size() != 7 ||
        value.encode() != bytes || field(value, "version").asInteger() != 1 ||
        text(value, "resume") != "await-message")
      return std::unexpected("checkpoint: unsupported continuation format");
    const auto &pid = field(value, "pid");
    if (!pid.isDict() || pid.asDict().size() != 4)
      return std::unexpected("checkpoint: invalid PID");
    auto spawn = message(text(value, "spawn"));
    if (!spawn) return std::unexpected(spawn.error());
    ActorContinuation out{{text(pid, "node"), text(pid, "cursor"),
                           number(pid, "epoch"), number(pid, "spawn")},
                          text(value, "program"),
                          text(value, "base"),
                          std::move(*spawn),
                          {}};
    const auto &completed = field(value, "completed").asList();
    if (completed.size() > maximumRecords)
      return std::unexpected("checkpoint: dedup budget exceeded");
    std::set<std::string> seen;
    for (const auto &record : completed) {
      if (!record.isDict() || record.asDict().size() != 3)
        return std::unexpected("checkpoint: invalid dedup record");
      std::optional<ActorMessage> response;
      if (!text(record, "response").empty()) {
        auto parsed = message(text(record, "response"));
        if (!parsed) return std::unexpected(parsed.error());
        response = std::move(*parsed);
      }
      const auto &request = text(record, "request");
      if (!seen.insert(request).second ||
          (response && response->requestId != request))
        return std::unexpected("checkpoint: duplicate or mismatched response");
      out.completed.push_back(
          {request, text(record, "hash"), std::move(response)});
    }
    const auto valid = encodeContinuation(out);
    if (!valid || *valid != bytes)
      return std::unexpected("checkpoint: invalid canonical continuation");
    return out;
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}

CheckpointBackend checkpointBackend(xanadu::SwarmContentSource &swarm) {
  return {[&swarm](const auto &keys, const auto &salt, const auto &hash,
                   auto sequence) -> std::expected<void, std::string> {
            try {
              swarm.publishMutable(keys, salt, hash, sequence);
              return {};
            } catch (const std::exception &error) {
              return std::unexpected(error.what());
            }
          },
          [&swarm](const auto &key, const auto &salt, const auto &hash,
                   auto sequence) {
            return swarm.publicationAcknowledged(key, salt, hash, sequence);
          }};
}
CheckpointPublisher::CheckpointPublisher(CheckpointBackend backend)
    : backend_(std::move(backend)) {}

std::expected<CheckpointStatus, std::string>
CheckpointPublisher::begin(const PublishedValue &value,
                           const xanadu::InfoHash &carrier,
                           const xanadu::MutableKeys &keys, std::string salt,
                           std::int64_t previousSequence) {
  if (!backend_.publish || !backend_.acknowledged || carrier.isZero() ||
      salt.empty() || salt.size() > 64 || previousSequence < 0 ||
      previousSequence == std::numeric_limits<std::int64_t>::max() ||
      keys.publicKey != value.publication.publisher)
    return std::unexpected("checkpoint: invalid publisher, salt or sequence");
  if (current_ && current_->state == CheckpointState::AwaitingAcknowledgement)
    return std::unexpected(
        "checkpoint: previous publication is unacknowledged");
  if (!decodeValue(encodeValue(value)) ||
      !decodeContinuation(value.continuation))
    return std::unexpected("checkpoint: signed continuation required");
  const std::vector<xanadu::TorrentContent> files{
      {.path = "vortex-value.bencode", .data = encodeValue(value)}};
  if (xanadu::makeTorrent(files, "vortex-value").hash != carrier)
    return std::unexpected(
        "checkpoint: carrier does not address this signed state");
  const auto sequence = previousSequence + 1;
  if (auto sent = backend_.publish(keys, salt, carrier, sequence); !sent)
    return std::unexpected(sent.error());
  current_ =
      CheckpointStatus{.name = {.key = keys.publicKey, .salt = std::move(salt)},
                       .hash = carrier,
                       .sequence = sequence,
                       .state    = CheckpointState::AwaitingAcknowledgement};
  return *current_;
}
std::optional<CheckpointStatus> CheckpointPublisher::poll() {
  if (current_ && current_->state == CheckpointState::AwaitingAcknowledgement &&
      backend_.acknowledged(current_->name.key, current_->name.salt,
                            current_->hash, current_->sequence))
    current_->state = CheckpointState::Committed;
  return current_;
}

std::expected<ResumedCheckpoint, std::string>
resolveCheckpoint(const xanadu::MutableLink &name,
                  MeshSwarmTransport &transport,
                  std::shared_ptr<xanadu::UserPermascroll> reader,
                  std::chrono::milliseconds timeout, ValueLimits limits) {
  const auto pointer = transport.source().resolveMutable(name, timeout);
  if (!pointer)
    return std::unexpected("checkpoint: current signed pointer unavailable");
  if (auto prepared = transport.prepareValue(pointer->hash, limits); !prepared)
    return std::unexpected(prepared.error());
  auto value =
      fetchValue(pointer->hash, transport.source(), std::move(reader), limits);
  if (!value) return std::unexpected(value.error());
  if (value->value.publication.publisher != name.key)
    return std::unexpected(
        "checkpoint: pointer and state have different signing devices");
  auto continuation = decodeContinuation(value->value.continuation);
  if (!continuation || continuation->pid.node != value->value.origin.str() ||
      continuation->pid.epoch != value->value.origin.arenaEpoch)
    return std::unexpected(
        "checkpoint: signed continuation does not match node identity");
  return ResumedCheckpoint{std::move(*value), std::move(*continuation),
                           pointer->sequence};
}
} // namespace zigzag::vortex::mesh
