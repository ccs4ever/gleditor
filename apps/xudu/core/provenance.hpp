/**
 * @file provenance.hpp
 * @brief Who wrote this, said in the open and signed with GnuPG.
 *
 * The publication manifest is already signed, by the ed25519 key that is the
 * publisher's name. That signature answers "is this the same publisher as last
 * time" perfectly well and answers "who is this" not at all: the key was minted
 * by this program, means nothing outside it, and is bound to no person.
 *
 * A person's OpenPGP key is bound to a person -- by an identity in the key, by
 * whoever has signed it, by however long it has been in use. So provenance here
 * is a plain YAML record naming the author, detached-signed with GnuPG, and
 * sealed into the torrent alongside the content it is about. Three properties
 * follow, and each is the reason for one of those decisions:
 *
 *   - It is checkable by anything, not only by this program. `gpg --verify`
 *     against a record a person can read is the whole procedure, and YAML is
 *     chosen over the bencode everything else here uses for exactly that
 *     reason: it is meant to be read by whoever is deciding whether to believe
 *     it.
 *   - It is bound to the bytes. The record is written and signed *before* the
 *     torrent is sealed and goes into that torrent, so the info hash covers the
 *     content and the claim of authorship together. Neither can be swapped for
 *     another afterwards without producing a different address.
 *   - It says what it covers. The record carries the length and digest of the
 *     content being sealed, so a reader who has both can check that the record
 *     is about what it arrived with rather than about something else.
 *
 * Signing is delegated to gpg rather than reimplemented. The whole value of an
 * OpenPGP signature is the web of trust and the key management around it, none
 * of which this program has any business duplicating -- and a reader will reach
 * for gpg to check it, so gpg is what should produce it.
 */
#ifndef XUDU_PROVENANCE_H
#define XUDU_PROVENANCE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xudu {

/// Who is claiming authorship, as a person rather than as a key this program
/// minted.
struct Author {
  std::string name;
  std::string email;
  /**
   * @brief Which OpenPGP key signs for them.
   *
   * Anything gpg accepts as a key specification: a fingerprint, a long key id,
   * an email address. Empty means gpg's default signing key, which is the
   * common case for somebody with one key.
   */
  std::string gpgKey;

  /// Enough to sign with: a name and an email address. A record with neither
  /// names nobody and is not worth signing.
  [[nodiscard]] bool named() const { return !name.empty() && !email.empty(); }
  bool operator==(const Author &) const = default;
};

/**
 * @brief What is being claimed, before it is signed.
 *
 * Everything here goes into the YAML. Fields a caller leaves empty are left
 * out rather than written blank, so a record says only what somebody meant to
 * say.
 */
struct Provenance {
  Author author;
  /// What the document is called, and the name it is published under.
  std::string title;
  std::string salt;
  /// The ed25519 key this machine publishes as, in hex. What the OpenPGP
  /// signature binds a person to: it says "the holder of this OpenPGP key
  /// vouches that this publishing name is theirs", which is the join between
  /// an identity people already have and a name this program made up.
  std::string publisher;
  /// Which state of the document, as the store names it.
  std::string version;
  /// Seconds since the epoch.
  std::uint64_t published{};
  /// Bytes of content this record covers, and their SHA-256 in lowercase hex.
  std::uint64_t contentLength{};
  std::string contentDigest;
  /// Global keys of scrolls the document quotes, so the record says what it
  /// was built out of as well as who built it.
  std::vector<std::string> quotes;
  /// Anything else worth recording, as `key: value`. Written after the fields
  /// above, in the order given.
  std::vector<std::pair<std::string, std::string>> extra;

  /// The YAML this record is, which is what gets signed.
  [[nodiscard]] std::string toYaml() const;
};

/// SHA-256 of @p data in lowercase hex. Not the SHA-1 a torrent addresses by:
/// this is a statement a person is signing, and it should not be weaker than
/// the signature over it.
[[nodiscard]] std::string sha256Hex(std::string_view data);

/// A record and the detached OpenPGP signature over it.
struct SignedProvenance {
  /// The YAML exactly as it was signed. Kept as text rather than rebuilt,
  /// because a signature is over bytes and a re-rendering is only probably the
  /// same bytes.
  std::string yaml;
  /// The detached signature, ASCII-armoured.
  std::string signature;
};

/// Whether gpg can be run at all. Publishing needs it, so this is what a
/// caller checks before getting as far as a torrent.
[[nodiscard]] bool gpgAvailable();

/**
 * @brief Sign @p record with GnuPG.
 *
 * Detached and armoured: the record stays readable, and the signature is a
 * separate file anybody can check with `gpg --verify`.
 *
 * @throws std::runtime_error when gpg is not installed, when the author is not
 *         named, or when gpg refuses -- most often because the key asked for
 *         is not in the keyring or has no secret half here. The complaint gpg
 *         made is included, since it is nearly always the actionable part.
 */
[[nodiscard]] SignedProvenance signProvenance(const Provenance &record);

/// What checking a signature found.
struct ProvenanceCheck {
  /// Whether gpg accepted the signature over the record.
  bool signatureValid{};
  /// Whether the signing key is one this keyring trusts, as opposed to merely
  /// having. A valid signature by an unknown key is a real signature by
  /// somebody you have no reason to believe, and the two must not be reported
  /// as the same thing.
  bool keyTrusted{};
  /// The primary key's fingerprint, when gpg reported one.
  std::string fingerprint;
  /// The identity in the signing key, as gpg prints it.
  std::string signer;
  /// What gpg said, for showing when it refused.
  std::string detail;
};

/**
 * @brief Check a detached signature over a record.
 *
 * Whether the key is anybody you know is a separate question from whether the
 * signature is good, and both are reported. Nothing here decides what to do
 * about the answer.
 */
[[nodiscard]] ProvenanceCheck verifyProvenance(const SignedProvenance &signed_);

/**
 * @brief Read the fields back out of a record's YAML.
 *
 * A deliberately small reader for the deliberately small YAML written above:
 * top-level `key: value` scalars and one list of plain strings. It is not a
 * YAML parser and does not pretend to be -- what it is for is showing a reader
 * who signed something, after gpg has said the signature is good.
 *
 * @return Nothing when the text is not of that shape.
 */
[[nodiscard]] std::optional<Provenance> parseProvenance(std::string_view yaml);

/// Names the sealed files carry inside a torrent, so a reader knows what to
/// look for and a writer cannot spell them differently.
inline constexpr auto provenanceFileName  = "AUTHORSHIP.yaml";
inline constexpr auto provenanceSigName   = "AUTHORSHIP.yaml.asc";
inline constexpr auto sealedContentName   = "primedia";

} // namespace xudu

#endif // XUDU_PROVENANCE_H
// vi: set sw=2 sts=2 ts=2 et:
