/**
 * @file pending_fetch.cpp
 * @brief Implementation of pending program fetch queue and pre-admission
 * buffering.
 */
#include "common/xanadu/vagent/cluster/pending_fetch.hpp"

#include <algorithm>

namespace xanadu::vagent::cluster {

PendingFetchQueue::PendingFetchQueue(PendingFetchConfig config)
    : config_(config) {}

std::expected<void, zigzag::vortex::mesh::ActorError>
PendingFetchQueue::enqueue(
    std::string_view programRef, const xanadu::identity::Fingerprint &origin,
    std::optional<zigzag::vortex::mesh::ActorPid> pendingPid,
    std::optional<AuthenticatedActorEvent> initialEvent,
    std::chrono::steady_clock::time_point now) {

  const std::string ref(programRef);
  auto it = fetches_.find(ref);
  if (it != fetches_.end()) {
    // Coalesce into existing in-flight fetch
    if (pendingPid.has_value()) {
      if (it->second.pendingPids.size() >= 128) {
        return std::unexpected(zigzag::vortex::mesh::ActorError::ResourceLimit);
      }
      auto &pids = it->second.pendingPids;
      if (std::find(pids.begin(), pids.end(), *pendingPid) == pids.end()) {
        pids.push_back(*pendingPid);
      }
    }
    if (initialEvent.has_value()) {
      if (it->second.waitingEvents.size() >= 512) {
        return std::unexpected(zigzag::vortex::mesh::ActorError::MailboxFull);
      }
      it->second.waitingEvents.push_back(std::move(*initialEvent));
    }
    return {};
  }

  // Rate limits
  if (fetches_.size() >= config_.maxPendingFetches) {
    return std::unexpected(zigzag::vortex::mesh::ActorError::ResourceLimit);
  }

  const std::string peerKey = origin.toString();
  if (peerFetchCounts_[peerKey] >= config_.maxPendingFetchesPerPeer) {
    return std::unexpected(zigzag::vortex::mesh::ActorError::ResourceLimit);
  }

  PendingProgramFetch fetch;
  fetch.programRef   = ref;
  fetch.origin       = origin;
  fetch.deadline     = now + config_.fetchTimeout;
  fetch.bytesFetched = 0;

  if (pendingPid.has_value()) {
    fetch.pendingPids.push_back(*pendingPid);
  }
  if (initialEvent.has_value()) {
    fetch.waitingEvents.push_back(std::move(*initialEvent));
  }

  ++peerFetchCounts_[peerKey];
  fetches_.emplace(ref, std::move(fetch));
  return {};
}

std::expected<void, zigzag::vortex::mesh::ActorError>
PendingFetchQueue::bufferMessage(const zigzag::vortex::mesh::ActorPid &pid,
                                 AuthenticatedActorEvent event,
                                 std::size_t maxMailboxBytes) {

  // Locate the fetch that owns this pending PID
  PendingProgramFetch *targetFetch = nullptr;
  for (auto &[ref, fetch] : fetches_) {
    if (std::find(fetch.pendingPids.begin(), fetch.pendingPids.end(), pid) !=
        fetch.pendingPids.end()) {
      targetFetch = &fetch;
      break;
    }
  }

  if (targetFetch == nullptr) {
    return std::unexpected(zigzag::vortex::mesh::ActorError::UnknownProcess);
  }

  // Calculate current buffered bytes for this PID
  std::size_t currentBytes = 0;
  for (const auto &wEvent : targetFetch->waitingEvents) {
    if (wEvent.message.target.has_value() && *wEvent.message.target == pid) {
      currentBytes += wEvent.message.value.size();
    }
  }

  const std::size_t incomingBytes = event.message.value.size();
  if (currentBytes + incomingBytes > maxMailboxBytes) {
    return std::unexpected(zigzag::vortex::mesh::ActorError::MailboxFull);
  }

  targetFetch->waitingEvents.push_back(std::move(event));
  return {};
}

std::expected<void, zigzag::vortex::mesh::ActorError>
PendingFetchQueue::recordBytesFetched(std::string_view programRef,
                                      std::uint64_t additionalBytes) {
  const std::string ref(programRef);
  auto it = fetches_.find(ref);
  if (it == fetches_.end()) {
    return std::unexpected(zigzag::vortex::mesh::ActorError::UnknownProcess);
  }

  it->second.bytesFetched += additionalBytes;
  if (it->second.bytesFetched > config_.maxPackageBytes) {
    (void)cancel(programRef);
    return std::unexpected(
        zigzag::vortex::mesh::ActorError::TransportUnavailable);
  }

  return {};
}

std::vector<PendingProgramFetch>
PendingFetchQueue::tick(std::chrono::steady_clock::time_point now) {
  std::vector<PendingProgramFetch> timedOut;

  for (auto it = fetches_.begin(); it != fetches_.end();) {
    if (now >= it->second.deadline) {
      const std::string peerKey = it->second.origin.toString();
      auto peerIt               = peerFetchCounts_.find(peerKey);
      if (peerIt != peerFetchCounts_.end()) {
        if (--peerIt->second == 0) {
          peerFetchCounts_.erase(peerIt);
        }
      }
      timedOut.push_back(std::move(it->second));
      it = fetches_.erase(it);
    } else {
      ++it;
    }
  }

  return timedOut;
}

bool PendingFetchQueue::isPending(std::string_view programRef) const {
  return fetches_.contains(std::string(programRef));
}

bool PendingFetchQueue::isPidPending(
    const zigzag::vortex::mesh::ActorPid &pid) const {
  for (const auto &[ref, fetch] : fetches_) {
    if (std::find(fetch.pendingPids.begin(), fetch.pendingPids.end(), pid) !=
        fetch.pendingPids.end()) {
      return true;
    }
  }
  return false;
}

std::optional<PendingProgramFetch>
PendingFetchQueue::complete(std::string_view programRef) {
  const std::string ref(programRef);
  auto it = fetches_.find(ref);
  if (it == fetches_.end()) {
    return std::nullopt;
  }

  const std::string peerKey = it->second.origin.toString();
  auto peerIt               = peerFetchCounts_.find(peerKey);
  if (peerIt != peerFetchCounts_.end()) {
    if (--peerIt->second == 0) {
      peerFetchCounts_.erase(peerIt);
    }
  }

  PendingProgramFetch fetch = std::move(it->second);
  fetches_.erase(it);
  return fetch;
}

std::optional<PendingProgramFetch>
PendingFetchQueue::cancel(std::string_view programRef) {
  return complete(programRef);
}

const PendingProgramFetch *
PendingFetchQueue::getFetch(std::string_view programRef) const {
  auto it = fetches_.find(std::string(programRef));
  if (it == fetches_.end()) {
    return nullptr;
  }
  return &it->second;
}

bool PendingFetchQueue::addRequiredBlob(std::string_view programRef,
                                        const xanadu::InfoHash &hash) {
  const std::string ref(programRef);
  auto it = fetches_.find(ref);
  if (it == fetches_.end()) {
    return false;
  }
  if (std::find(it->second.requiredBlobs.begin(),
                it->second.requiredBlobs.end(),
                hash) == it->second.requiredBlobs.end()) {
    it->second.requiredBlobs.push_back(hash);
  }
  return true;
}

const PendingProgramFetch *
PendingFetchQueue::getFetchByBlob(const xanadu::InfoHash &hash) const {
  for (const auto &[ref, fetch] : fetches_) {
    if (std::find(fetch.requiredBlobs.begin(), fetch.requiredBlobs.end(),
                  hash) != fetch.requiredBlobs.end()) {
      return &fetch;
    }
  }
  return nullptr;
}

} // namespace xanadu::vagent::cluster
