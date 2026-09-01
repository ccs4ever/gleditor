/**
 * @file identity_network_controller.hpp
 * @brief BEP 10 peer/torrent plugins, piece validation, and peer isolation.
 */
#ifndef XUDU_IDENTITY_NETWORK_CONTROLLER_HPP
#define XUDU_IDENTITY_NETWORK_CONTROLLER_HPP

#include <expected>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bdecode.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/extensions.hpp>
#include <libtorrent/peer_connection_handle.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/span.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "../torrent.hpp"
#include "identity_layout.hpp"
#include "identity_serialization.hpp"
#include "identity_validation.hpp"

namespace xudu::identity {

inline constexpr const char *kExtIdentityLookupName = "xudu_identity_lookup";
inline constexpr const char *kExtOracleVoteName     = "xudu_oracle_vote";
inline constexpr const char *kExtOracleVerifyName   = "xudu_oracle_verify";

inline constexpr int kExtIdentityLookupMsgId = 2;
inline constexpr int kExtOracleVoteMsgId     = 3;
inline constexpr int kExtOracleVerifyMsgId   = 4;

inline constexpr std::uint8_t kBtMsgExtended = 20;

class IdentityNetworkController;

/**
 * @class IdentityPeerPlugin
 * @brief BEP 10 extension plugin per peer connection.
 */
class IdentityPeerPlugin
    : public libtorrent::peer_plugin,
      public std::enable_shared_from_this<IdentityPeerPlugin> {
public:
  IdentityPeerPlugin(libtorrent::peer_connection_handle pc,
                     const InfoHash &swarmHash,
                     IdentityNetworkController *controller);
  ~IdentityPeerPlugin() override;

  [[nodiscard]] libtorrent::string_view type() const override {
    return "xudu_identity_peer_plugin";
  }

  void add_handshake(libtorrent::entry &h) override;
  bool on_extension_handshake(const libtorrent::bdecode_node &node) override;
  bool on_extended(int length, int msg,
                   libtorrent::span<char const> body) override;

  // Connection gate & Challenge-Response
  bool sendAuthChallenge(const Hash32 &nonce);
  bool sendAuthResponse(const Hash32 &nonce, const Fingerprint &claimed,
                        const Signature64 &sig);
  bool sendIdentityQuery(const Fingerprint &fp);
  bool sendIdentityResponse(const IdentityEntry &entry,
                            const LedgerMerkleProof &proof);
  bool sendVoteBroadcast(const VoteEntry &vote);
  bool sendEmailVerifyRequest(const EmailVerifyRequestMsg &req);

  void isolateAndDisconnect(std::string_view reason);

  [[nodiscard]] bool isAuthenticated() const noexcept {
    return isAuthenticated_;
  }
  [[nodiscard]] bool isIsolated() const noexcept { return isIsolated_; }
  [[nodiscard]] const std::optional<Fingerprint> &
  authenticatedIdentity() const noexcept {
    return authenticatedIdentity_;
  }
  [[nodiscard]] const InfoHash &swarmHash() const noexcept {
    return swarmHash_;
  }

private:
  bool sendExtendedRaw(int remoteExtId, std::string_view payload);

  libtorrent::peer_connection_handle pc_;
  InfoHash swarmHash_;
  IdentityNetworkController *controller_{nullptr};

  int remoteIdentityLookupId_{0};
  int remoteOracleVoteId_{0};
  int remoteOracleVerifyId_{0};

  bool isAuthenticated_{false};
  bool isIsolated_{false};
  std::optional<Fingerprint> authenticatedIdentity_;
  std::optional<Hash32> pendingChallengeNonce_;
};

/**
 * @class IdentityTorrentPlugin
 * @brief Torrent plugin managing peer plugins and broadcast distribution.
 */
class IdentityTorrentPlugin : public libtorrent::torrent_plugin {
public:
  IdentityTorrentPlugin(libtorrent::torrent_handle handle,
                        const InfoHash &swarmHash,
                        IdentityNetworkController *controller);
  ~IdentityTorrentPlugin() override;

  std::shared_ptr<libtorrent::peer_plugin>
  new_connection(const libtorrent::peer_connection_handle &pc) override;

  void broadcastVote(const VoteEntry &vote);
  void broadcastIdentity(const IdentityEntry &entry,
                         const LedgerMerkleProof &proof);

  [[nodiscard]] const InfoHash &swarmHash() const noexcept {
    return swarmHash_;
  }

private:
  libtorrent::torrent_handle handle_;
  InfoHash swarmHash_;
  IdentityNetworkController *controller_{nullptr};
  std::mutex mutex_;
  std::vector<std::weak_ptr<IdentityPeerPlugin>> peers_;
};

/**
 * @class IdentityNetworkController
 * @brief High-level controller managing ledger validation, BEP 10 events,
 *        and peer quarantine.
 */
class IdentityNetworkController {
public:
  struct Options {
    std::optional<Fingerprint> localFingerprint;
    std::optional<Signature64> localSigningKey;
    bool isOracleNode{false};
  };

  IdentityNetworkController();
  explicit IdentityNetworkController(Options options);
  ~IdentityNetworkController();

  /// Reference to underlying cryptographic validation pipeline.
  [[nodiscard]] EnginePipeline &pipeline() noexcept { return pipeline_; }
  [[nodiscard]] const EnginePipeline &pipeline() const noexcept {
    return pipeline_;
  }

  /// Reference to Hashcash proof-of-work engine.
  [[nodiscard]] HashcashEngine &hashcashEngine() noexcept {
    return hashcashEngine_;
  }
  [[nodiscard]] const HashcashEngine &hashcashEngine() const noexcept {
    return hashcashEngine_;
  }

  /// Options getter.
  [[nodiscard]] const Options &options() const noexcept { return options_; }

  /**
   * @brief Intercepts piece_finished_alert for identities.torrent and
   * validates.
   *
   * @param alert The piece_finished_alert from libtorrent.
   * @param pieceData Raw buffer of the passed piece.
   * @return Success, or ValidationError if the block violated ledger rules.
   */
  [[nodiscard]] std::expected<void, ValidationError>
  onPiecePassed(const libtorrent::piece_finished_alert &alert,
                std::span<const std::uint8_t> pieceData);

  /// Handles incoming vote from a peer.
  void handleIncomingVote(const VoteEntry &vote);

  /// Peer quarantine tracking.
  void quarantinePeer(std::string_view peerAddress);
  [[nodiscard]] bool isPeerQuarantined(std::string_view peerAddress) const;

  /// Hook to attach IdentityTorrentPlugin to a libtorrent session.
  void attachToSession(libtorrent::session &session,
                       const InfoHash &ledgerHash);

private:
  Options options_;
  EnginePipeline pipeline_;
  HashcashEngine hashcashEngine_;
  std::mutex quarantineMutex_;
  std::unordered_set<std::string> quarantinedPeers_;
};

} // namespace xudu::identity

#endif // XUDU_IDENTITY_NETWORK_CONTROLLER_HPP
