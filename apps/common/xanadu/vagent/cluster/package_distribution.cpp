/**
 * @file package_distribution.cpp
 * @brief Implementation of swarm-transport package distribution and CAS
 * integration.
 */
#include "common/xanadu/vagent/cluster/package_distribution.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <span>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/merkle_ledger.hpp"

namespace xanadu::vagent::cluster {

namespace fs = std::filesystem;

SwarmPackageDistribution::SwarmPackageDistribution(
    PackageCas &cas, PendingFetchQueue &queue, SwarmDistributionOptions options,
    std::shared_ptr<xanadu::SwarmContentSource> swarm,
    std::shared_ptr<xanadu::SystemTorrentManager> torrentManager,
    std::shared_ptr<xanadu::PublicationDiscovery> discovery)
    : cas_(cas), queue_(queue), options_(std::move(options)),
      swarm_(std::move(swarm)), torrentManager_(std::move(torrentManager)),
      discovery_(std::move(discovery)) {
  if (options_.downloadDir.empty()) {
    options_.downloadDir = cas_.casDir() / "tmp" / "swarm_downloads";
  }
}

std::string
SwarmPackageDistribution::packageTopic(std::string_view programRef) {
  return "vagent:pkg:" + std::string(programRef);
}

xanadu::InfoHash
SwarmPackageDistribution::packageDhtTarget(std::string_view programRef) {
  const std::string topicStr = packageTopic(programRef);
  const std::string material = "xudu:topic:" + topicStr;
  const auto digest          = xanadu::sha1(material);
  return xanadu::InfoHash{digest};
}

std::expected<PackagePublishResult, CasError>
SwarmPackageDistribution::publishBlob(std::string_view sha256,
                                      std::string_view displayName) {
  if (!cas_.hasBlob(sha256)) {
    return std::unexpected(CasError::NotFound);
  }

  const auto madeRes = cas_.createTorrentForBlob(sha256, displayName,
                                                 options_.canonicalPieceLength);
  if (!madeRes) {
    return std::unexpected(madeRes.error());
  }

  const auto &made = *madeRes;
  const std::string name =
      displayName.empty() ? std::string(sha256) : std::string(displayName);

  if (swarm_) {
    swarm_->addTorrent(made.file, cas_.casDir().string(), true);
  }

  if (torrentManager_) {
    ManagedTorrentDescriptor desc;
    desc.infoHash    = made.hash;
    desc.name        = name;
    desc.role        = TorrentRole::GeneralContent;
    desc.dataRoot    = cas_.casDir().string();
    desc.torrentFile = made.file;
    desc.isPinned    = true;
    (void)torrentManager_->registerTorrent(desc);
  }

  PackagePublishResult res;
  res.infoHash    = made.hash;
  res.torrentFile = made.file;
  res.magnetUri   = "magnet:?xt=urn:btih:" + made.hash.hex() + "&dn=" + name;
  res.sha256      = std::string(sha256);

  if (const auto p = cas_.blobPath(sha256); p.has_value()) {
    std::error_code ec;
    res.size = fs::file_size(*p, ec);
  }

  return res;
}

std::expected<PackagePublishResult, std::string>
SwarmPackageDistribution::publishPackage(const PackageManifest &manifest,
                                         bool standalone) {
  const std::string manifestBytes = encodePackageManifest(manifest, standalone);
  const std::string progRef =
      computePackageProgramRef(manifest.kind, manifest.programBody,
                               computeTargetsDigest(manifest.targets));
  const std::string manifestSha256 = toHex32(sha256Digest(manifestBytes));

  const auto storeRes = cas_.storeBlob(manifestSha256, manifestBytes);
  if (!storeRes) {
    return std::unexpected(std::format("Failed to store manifest in CAS: {}",
                                       toString(storeRes.error())));
  }

  const xanadu::MadeTorrent made = xanadu::makeTorrent(
      manifestBytes, manifestSha256, options_.canonicalPieceLength);
  cas_.registerInfoHash(made.hash, manifestSha256);

  if (swarm_) {
    swarm_->addTorrent(made.file, cas_.casDir().string(), true);
  }

  if (torrentManager_) {
    ManagedTorrentDescriptor desc;
    desc.infoHash    = made.hash;
    desc.name        = manifestSha256;
    desc.role        = TorrentRole::GeneralContent;
    desc.dataRoot    = cas_.casDir().string();
    desc.torrentFile = made.file;
    desc.isPinned    = true;
    (void)torrentManager_->registerTorrent(desc);
  }

  // If in network seeding mode, seed all referenced payload blobs that exist in
  // CAS
  if (!standalone) {
    for (const auto &[triple, target] : manifest.targets) {
      if (!target.module.sha256.empty() && cas_.hasBlob(target.module.sha256)) {
        (void)publishBlob(target.module.sha256, target.module.filename);
      }
      for (const auto &lib : target.bundledLibs) {
        if (!lib.sha256.empty() && cas_.hasBlob(lib.sha256)) {
          (void)publishBlob(lib.sha256, lib.soname);
        }
      }
    }
  }

  PackagePublishResult res;
  res.infoHash    = made.hash;
  res.torrentFile = made.file;
  res.magnetUri =
      "magnet:?xt=urn:btih:" + made.hash.hex() + "&dn=" + manifestSha256;
  res.sha256     = manifestSha256;
  res.programRef = progRef;
  res.size       = manifestBytes.size();

  return res;
}

std::expected<PackagePublishResult, std::string>
SwarmPackageDistribution::publishPackageFile(
    const std::filesystem::path &packagePath) {
  std::ifstream in(packagePath, std::ios::binary);
  if (!in.is_open()) {
    return std::unexpected("Cannot open package file: " + packagePath.string());
  }

  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());

  auto decRes = decodePackageManifest(bytes);
  if (!decRes) {
    return std::unexpected("Invalid .vnpkg file: " + decRes.error());
  }

  const std::string fileSha256 = toHex32(sha256Digest(bytes));
  auto storeRes                = cas_.storeBlob(fileSha256, bytes);
  if (!storeRes) {
    return std::unexpected(std::format("Failed to store .vnpkg into CAS: {}",
                                       toString(storeRes.error())));
  }

  // Ingest any embedded payloads into CAS
  for (const auto &[pSha, pData] : decRes->payloads) {
    (void)cas_.storeBlob(pSha, pData);
  }

  return publishPackage(*decRes, false);
}

std::expected<void, std::string>
SwarmPackageDistribution::announceTopic(std::string_view programRef) {
  const std::string topicStr = packageTopic(programRef);
  if (swarm_) {
    swarm_->joinPublicationTopic(topicStr, cas_.casDir().string());
  }
  if (discovery_) {
    (void)discovery_->submit(topicStr, false);
  }
  return {};
}

std::expected<void, zigzag::vortex::mesh::ActorError>
SwarmPackageDistribution::initiateFetch(
    std::string_view programRef, const xanadu::identity::Fingerprint &origin,
    const xanadu::InfoHash &packageInfoHash, std::string_view expectedSha256,
    std::optional<zigzag::vortex::mesh::ActorPid> pendingPid,
    std::optional<AuthenticatedActorEvent> initialEvent,
    std::chrono::steady_clock::time_point now) {

  auto eqRes =
      queue_.enqueue(programRef, origin, pendingPid, initialEvent, now);
  if (!eqRes) {
    return eqRes;
  }

  queue_.addRequiredBlob(programRef, packageInfoHash);
  hashToProgramRef_[packageInfoHash] = std::string(programRef);

  // Check if expected blob already exists in CAS
  if (!expectedSha256.empty() && cas_.hasBlob(expectedSha256)) {
    InFlightFetchState state;
    state.programRef     = std::string(programRef);
    state.infoHash       = packageInfoHash;
    state.expectedSha256 = std::string(expectedSha256);
    state.status         = FetchStatus::Completed;
    state.startTime      = now;
    if (auto p = cas_.blobPath(expectedSha256); p.has_value()) {
      state.stagedCasPath = *p;
    }
    inFlight_[std::string(programRef)] = std::move(state);
    return {};
  }

  InFlightFetchState state;
  state.programRef     = std::string(programRef);
  state.infoHash       = packageInfoHash;
  state.expectedSha256 = std::string(expectedSha256);
  state.status         = FetchStatus::Downloading;
  state.startTime      = now;

  std::string magnet = "magnet:?xt=urn:btih:" + packageInfoHash.hex();
  if (!expectedSha256.empty()) {
    magnet += "&dn=" + std::string(expectedSha256);
  }

  if (swarm_) {
    std::error_code ec;
    fs::create_directories(options_.downloadDir, ec);
    swarm_->addMagnet(magnet, options_.downloadDir.string(), false);
  }

  if (torrentManager_) {
    ManagedTorrentDescriptor desc;
    desc.infoHash  = packageInfoHash;
    desc.name      = expectedSha256.empty() ? packageInfoHash.hex()
                                            : std::string(expectedSha256);
    desc.role      = TorrentRole::GeneralContent;
    desc.dataRoot  = options_.downloadDir.string();
    desc.magnetUri = magnet;
    desc.isPinned  = false;
    (void)torrentManager_->registerTorrent(desc);
  }

  inFlight_[std::string(programRef)] = std::move(state);
  return {};
}

void SwarmPackageDistribution::onFetchComplete(CompletionCallback callback) {
  completionCb_ = std::move(callback);
}

std::vector<std::pair<PendingProgramFetch, std::filesystem::path>>
SwarmPackageDistribution::tick(std::chrono::steady_clock::time_point now) {
  std::vector<std::pair<PendingProgramFetch, std::filesystem::path>> completed;

  if (swarm_) {
    swarm_->poll();
  }
  if (torrentManager_) {
    torrentManager_->poll();
  }

  // 1. Process timed-out fetches from queue
  auto timedOut = queue_.tick(now);
  for (const auto &to : timedOut) {
    auto it = inFlight_.find(to.programRef);
    if (it != inFlight_.end()) {
      it->second.status       = FetchStatus::TimedOut;
      it->second.errorMessage = "Fetch timed out";
      if (completionCb_) {
        completionCb_(to.programRef, std::unexpected("Fetch timed out"));
      }
      hashToProgramRef_.erase(it->second.infoHash);
      inFlight_.erase(it);
    }
  }

  // 2. Advance active in-flight fetches
  for (auto it = inFlight_.begin(); it != inFlight_.end();) {
    auto &[progRef, state] = *it;

    if (state.status == FetchStatus::Completed) {
      auto fetchRecord = queue_.complete(progRef);
      if (fetchRecord) {
        completed.push_back({*fetchRecord, state.stagedCasPath});
        if (completionCb_) {
          completionCb_(progRef, *fetchRecord);
        }
      }
      hashToProgramRef_.erase(state.infoHash);
      it = inFlight_.erase(it);
      continue;
    }

    if (swarm_) {
      auto metaOpt = swarm_->metainfo(state.infoHash);
      if (metaOpt) {
        const auto &meta = *metaOpt;
        state.totalBytes = meta.totalLength();

        if (state.totalBytes > options_.maxPackageBytes) {
          (void)queue_.recordBytesFetched(progRef, state.totalBytes);
          (void)queue_.cancel(progRef);
          if (completionCb_) {
            completionCb_(progRef,
                          std::unexpected("Package size exceeds quota"));
          }
          hashToProgramRef_.erase(state.infoHash);
          it = inFlight_.erase(it);
          continue;
        }

        // Non-blocking check: verify all pieces are complete before reading
        // stream
        if (!swarm_->isComplete(state.infoHash)) {
          ++it;
          continue;
        }

        std::string data =
            swarm_->readStream(state.infoHash, 0, meta.totalLength());
        if (data.size() == meta.totalLength()) {
          // Verify each piece hash
          bool piecesOk = true;
          for (std::size_t i = 0; i < meta.pieceCount(); ++i) {
            const auto pLen = meta.lengthOfPiece(i);
            const auto pOff =
                static_cast<std::uint64_t>(i) * meta.pieceLength();
            if (!meta.verifyPiece(i,
                                  std::string_view(data).substr(pOff, pLen))) {
              piecesOk = false;
              break;
            }
          }

          if (!piecesOk) {
            (void)queue_.cancel(progRef);
            if (completionCb_) {
              completionCb_(progRef,
                            std::unexpected("Piece hash verification failed"));
            }
            hashToProgramRef_.erase(state.infoHash);
            it = inFlight_.erase(it);
            continue;
          }

          // Verify whole-payload SHA-256
          const std::string computedSha = toHex32(sha256Digest(data));
          if (!state.expectedSha256.empty() &&
              computedSha != state.expectedSha256) {
            (void)queue_.cancel(progRef);
            if (completionCb_) {
              completionCb_(progRef,
                            std::unexpected("Whole-payload SHA-256 mismatch"));
            }
            hashToProgramRef_.erase(state.infoHash);
            it = inFlight_.erase(it);
            continue;
          }

          std::optional<PackageManifest> dec;
          if (data.starts_with(kPackageMagic)) {
            auto decRes = decodePackageManifest(data);
            if (!decRes) {
              (void)queue_.cancel(progRef);
              if (completionCb_) {
                completionCb_(progRef, std::unexpected("Corrupt manifest: " +
                                                       decRes.error()));
              }
              hashToProgramRef_.erase(state.infoHash);
              it = inFlight_.erase(it);
              continue;
            }
            const std::string computedProgRef =
                computePackageProgramRef(decRes->kind, decRes->programBody,
                                         computeTargetsDigest(decRes->targets));
            if (computedProgRef != progRef) {
              (void)queue_.cancel(progRef);
              if (completionCb_) {
                completionCb_(progRef,
                              std::unexpected("Logical programRef mismatch"));
              }
              hashToProgramRef_.erase(state.infoHash);
              it = inFlight_.erase(it);
              continue;
            }
            dec = std::move(*decRes);
          }

          const std::string shaToStore =
              state.expectedSha256.empty() ? computedSha : state.expectedSha256;
          auto storeRes = cas_.storeBlob(shaToStore, data);
          if (!storeRes) {
            (void)queue_.cancel(progRef);
            if (completionCb_) {
              completionCb_(progRef,
                            std::unexpected("Failed to commit blob to CAS"));
            }
            hashToProgramRef_.erase(state.infoHash);
            it = inFlight_.erase(it);
            continue;
          }

          cas_.registerInfoHash(state.infoHash, shaToStore);

          if (dec && !dec->payloads.empty()) {
            bool payloadsOk = true;
            for (const auto &[pSha, pBytes] : dec->payloads) {
              auto pRes = cas_.storeBlob(pSha, pBytes);
              if (!pRes) {
                payloadsOk = false;
                break;
              }
            }
            if (!payloadsOk) {
              (void)queue_.cancel(progRef);
              if (completionCb_) {
                completionCb_(progRef,
                              std::unexpected(
                                  "Failed to commit companion payload to CAS"));
              }
              hashToProgramRef_.erase(state.infoHash);
              it = inFlight_.erase(it);
              continue;
            }
          }

          (void)queue_.recordBytesFetched(progRef, data.size());
          auto fetchRecord = queue_.complete(progRef);
          if (fetchRecord) {
            completed.push_back({*fetchRecord, *storeRes});
            if (completionCb_) {
              completionCb_(progRef, *fetchRecord);
            }
          }
          hashToProgramRef_.erase(state.infoHash);
          it = inFlight_.erase(it);
          continue;
        }
      }
    }

    ++it;
  }

  return completed;
}

std::optional<InFlightFetchState>
SwarmPackageDistribution::fetchState(std::string_view programRef) const {
  const auto it = inFlight_.find(std::string(programRef));
  if (it != inFlight_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::expected<std::filesystem::path, std::string>
SwarmPackageDistribution::fetchBlobDirect(const xanadu::ContentSource &source,
                                          const xanadu::InfoHash &infoHash,
                                          std::string_view expectedSha256,
                                          std::string_view programRef) {

  const auto metaOpt = source.metainfo(infoHash);
  if (!metaOpt) {
    return std::unexpected("Metadata not available for infohash");
  }
  const auto &meta = *metaOpt;
  if (meta.totalLength() > options_.maxPackageBytes) {
    return std::unexpected("Payload exceeds maximum package size");
  }

  std::string wholePayload;
  wholePayload.reserve(meta.totalLength());

  for (std::size_t i = 0; i < meta.pieceCount(); ++i) {
    const auto pieceLen    = meta.lengthOfPiece(i);
    const auto pieceOffset = static_cast<std::uint64_t>(i) * meta.pieceLength();
    const auto pieceBytes  = source.readStream(infoHash, pieceOffset, pieceLen);
    if (!meta.verifyPiece(i, pieceBytes)) {
      return std::unexpected("Piece hash verification failed");
    }
    wholePayload += pieceBytes;
  }

  const std::string computedSha256 = toHex32(sha256Digest(wholePayload));
  if (!expectedSha256.empty() && computedSha256 != expectedSha256) {
    return std::unexpected("Whole-payload SHA-256 verification failed");
  }

  std::optional<PackageManifest> dec;
  if (!programRef.empty() || wholePayload.starts_with(kPackageMagic)) {
    auto decRes = decodePackageManifest(wholePayload);
    if (!decRes) {
      if (!programRef.empty()) {
        return std::unexpected("Failed to decode package manifest: " +
                               decRes.error());
      }
    } else {
      const std::string computedProgRef =
          computePackageProgramRef(decRes->kind, decRes->programBody,
                                   computeTargetsDigest(decRes->targets));
      if (!programRef.empty() && computedProgRef != programRef) {
        return std::unexpected("Logical programRef mismatch");
      }
      dec = std::move(*decRes);
    }
  }

  const std::string shaToStore =
      expectedSha256.empty() ? computedSha256 : std::string(expectedSha256);
  auto storeRes = cas_.storeBlob(shaToStore, wholePayload);
  if (!storeRes) {
    return std::unexpected(std::string(toString(storeRes.error())));
  }

  cas_.registerInfoHash(infoHash, shaToStore);

  if (dec && !dec->payloads.empty()) {
    for (const auto &[pSha, pBytes] : dec->payloads) {
      auto pRes = cas_.storeBlob(pSha, pBytes);
      if (!pRes) {
        return std::unexpected("Failed to commit companion payload to CAS: " +
                               std::string(toString(pRes.error())));
      }
    }
  }

  return *storeRes;
}

void SwarmPackageDistribution::connectPeer(const xanadu::InfoHash &hash,
                                           const std::string &host,
                                           std::uint16_t port) {
  if (swarm_) {
    swarm_->connectPeer(hash, host, port);
  }
  if (torrentManager_) {
    torrentManager_->connectPeer(hash, host, port);
  }
}

} // namespace xanadu::vagent::cluster
