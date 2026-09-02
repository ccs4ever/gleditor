/**
 * @file pgp_verify.hpp
 * @brief OpenPGP verification, over RNP.
 *
 * This is the root of the identity ledger's trust. Two questions are asked
 * here and nowhere else:
 *
 * 1. Does an armored key really belong to the fingerprint claiming it?
 *    A fingerprint is a hash of a key, not a key -- so a record that carries
 *    both is making a claim, not proving one. Checking that claim is what
 *    stops a peer from presenting its own key under somebody else's name and
 *    having every downstream signature check pass against it.
 *
 * 2. Is a detached OpenPGP signature over these bytes genuinely by that key?
 *    That is what a DeviceDelegation is: a master key saying a particular
 *    Ed25519 device key may sign on its behalf.
 *
 * Everything above this file verifies fast per-message Ed25519 signatures
 * against a device key. This file is what makes that device key mean
 * something.
 */
#ifndef XUDU_IDENTITY_PGP_VERIFY_HPP
#define XUDU_IDENTITY_PGP_VERIFY_HPP

#include <optional>
#include <string>
#include <string_view>

#include "identity_layout.hpp"

namespace xudu::identity::pgp {

/**
 * @brief The fingerprint of an armored OpenPGP public key, as the key itself
 *        reports it.
 *
 * @return std::nullopt when the input is not a key RNP can parse, or carries
 *         no public key at all.
 */
[[nodiscard]] std::optional<Fingerprint>
fingerprintOf(std::string_view armoredKey);

/**
 * @brief Whether @p armoredKey really is the key @p claimed names.
 *
 * The comparison a record carrying both a key and a fingerprint needs before
 * anything else is done with either. False when the key does not parse, so
 * an unreadable key is an unusable one rather than a trusted one.
 */
[[nodiscard]] bool keyMatchesFingerprint(std::string_view armoredKey,
                                         const Fingerprint &claimed);

/**
 * @brief Whether @p armoredSignature is a good detached OpenPGP signature
 *        over @p message by @p armoredKey.
 *
 * Requires at least one signature in the packet and every signature present
 * to verify: a packet whose second signature is bad is a bad packet.
 */
[[nodiscard]] bool verifyDetached(std::string_view armoredKey,
                                  std::string_view message,
                                  std::string_view armoredSignature);

} // namespace xudu::identity::pgp

#endif // XUDU_IDENTITY_PGP_VERIFY_HPP
