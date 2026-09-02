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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "../torrent.hpp"
#include "../user_permascroll.hpp"
#include "identity_layout.hpp"
#include "identity_serialization.hpp"
#include "identity_validation.hpp"

namespace xudu::identity {

inline constexpr const char *kExtIdentityLookupName = "xudu_identity_lookup";
inline constexpr const char *kExtOracleVoteName     = "xudu_oracle_vote";
inline constexpr const char *kExtOracleVerifyName   = "xudu_oracle_verify";
inline constexpr const char *kExtTranscopyrightName = "xudu_transcopyright";

inline constexpr int kExtIdentityLookupMsgId = 2;
inline constexpr int kExtOracleVoteMsgId     = 3;
inline constexpr int kExtOracleVerifyMsgId   = 4;
inline constexpr int kExtTranscopyrightMsgId = 5;

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
                        const std::array<std::uint8_t, 32> &devicePublicKey,
                        const Signature64 &sig);
  bool sendIdentityQuery(const Fingerprint &fp);
  bool sendIdentityResponse(const IdentityEntry &entry,
                            const LedgerMerkleProof &proof);
  bool sendVoteBroadcast(const VoteEntry &vote);
  bool sendEmailVerifyRequest(const EmailVerifyRequestMsg &req);
  bool sendTcInvoiceQuery(const TcInvoiceQueryMsg &query);
  bool sendTcInvoiceResponse(const TcInvoiceResponseMsg &resp);
  bool sendTcSettleRequest(const TcSettleRequestMsg &req);
  bool sendTcKeyDelivery(const TcKeyDeliveryMsg &delivery);

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

  /// The challenge this connection is waiting on an answer to. Public because
  /// it is sent to the peer the moment it is generated -- it is unpredictable,
  /// not secret -- and because a test cannot otherwise answer a random one.
  [[nodiscard]] const std::optional<Hash32> &
  pendingChallengeNonce() const noexcept {
    return pendingChallengeNonce_;
  }

private:
  bool sendExtendedRaw(int remoteExtId, std::string_view payload);

  libtorrent::peer_connection_handle pc_;
  InfoHash swarmHash_;
  IdentityNetworkController *controller_{nullptr};

  int remoteIdentityLookupId_{0};
  int remoteOracleVoteId_{0};
  int remoteOracleVerifyId_{0};
  int remoteTranscopyrightId_{0};

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
    /// The Ed25519 keypair this node signs challenges with. Was an
    /// std::optional<Signature64> -- a signature type holding a key -- which
    /// could not sign anything, so the code that "signed" a challenge sent
    /// the field verbatim and the same bytes answered every challenge.
    std::optional<MutableKeys> localDeviceKeys;
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
   * @brief Record that @p masterFingerprint has delegated signing authority
   *        to a device key, having checked that it really did.
   *
   * @param delegation The attestation, as published by the master.
   * @param masterPublicKeyArmored The master's OpenPGP public key.
   * @return false, changing nothing, when the delegation does not verify.
   *
   * This is the only way a device key becomes acceptable for peer
   * authentication. Everything the peer wire trusts descends from a call to
   * this that returned true.
   */
  bool trustDelegation(const DeviceDelegation &delegation,
                       std::string_view masterPublicKeyArmored);

  /**
   * @brief The device key @p fingerprint is known to have delegated to, if
   *        any.
   *
   * std::nullopt means no verified delegation has been seen, which is a
   * refusal rather than a gap: a peer claiming an identity we have no
   * delegation for cannot be distinguished from one impersonating it.
   */
  [[nodiscard]] std::optional<std::array<std::uint8_t, 32>>
  deviceKeyFor(const Fingerprint &fingerprint) const;

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

  mutable std::mutex delegationMutex_;
  std::map<Fingerprint, std::array<std::uint8_t, 32>> delegatedDeviceKeys_;
};

} // namespace xudu::identity

#endif // XUDU_IDENTITY_NETWORK_CONTROLLER_HPP
