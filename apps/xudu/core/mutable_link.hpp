/**
 * @file mutable_link.hpp
 * @brief A permanent name for content that has not been written yet.
 *
 * An info hash names bytes. That is its strength -- nobody assigns it, nobody
 * can reassign it, and the same content always produces it -- and it is also
 * the one thing it cannot do: it cannot name something that is still growing.
 * Appending to a torrent produces a different torrent, because the info hash
 * fixes the file list, the piece length and every piece hash.
 *
 * A permascroll is exactly the thing that keeps growing. So the address of a
 * scroll cannot be the address of its content, and something has to stand
 * between them.
 *
 * BEP 46 is that something, and it is already deployed. A publisher holds an
 * ed25519 key pair. The public key is the permanent name. The DHT holds a
 * signed, sequence-numbered item under that name whose payload is the info
 * hash that is current, and the publisher moves the pointer by republishing
 * with a higher sequence number. The key never changes; what it points at
 * does.
 *
 * The reason this can be used as an address at all, rather than as a hint, is
 * that the pointer is signed. A DHT node is a stranger asked to hold a value,
 * and a stranger holding your address is normally where rot and substitution
 * come from. Here the answer carries a signature over the sequence number and
 * the payload, so the only party that can move a name is the one holding the
 * private key. Resolving is therefore untrusted in exactly the way fetching a
 * piece is untrusted: the answer is checked, so the answerer does not have to
 * be trusted.
 *
 * What is in this file is the arithmetic, which needs no network and is the
 * same everywhere: reading and writing the link, working out where in the DHT
 * the pointer lives, building the exact bytes that get signed, and reading the
 * payload. Fetching the item, and the elliptic curve, are in swarm.hpp.
 */
#ifndef XUDU_MUTABLE_LINK_H
#define XUDU_MUTABLE_LINK_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "torrent.hpp"

namespace xudu {

/**
 * @brief An ed25519 public key: the permanent half of a mutable name.
 *
 * This is what a reference to a growing thing is written as. It identifies a
 * publisher's series rather than any particular content, which is why it can
 * still be right after the content it points at has been replaced.
 */
struct PublicKey {
  std::array<std::uint8_t, 32> bytes{};

  [[nodiscard]] std::string hex() const;
  /// Parse sixty-four hex digits, which is how BEP 46 writes one.
  [[nodiscard]] static PublicKey fromHex(std::string_view text);
  [[nodiscard]] bool isZero() const;

  bool operator==(const PublicKey &) const = default;
  [[nodiscard]] bool operator<(const PublicKey &other) const {
    return bytes < other.bytes;
  }
};

/**
 * @brief The private half: the right to move a name.
 *
 * Sixty-four bytes, which is the expanded form ed25519 signing uses rather
 * than the thirty-two byte seed. Holding this is the whole of the authority to
 * say what a name points at, so it is never written into a link and never
 * leaves the machine that publishes.
 */
struct SecretKey {
  std::array<std::uint8_t, 64> bytes{};

  /**
   * @brief The key as hex, for keeping it between runs.
   *
   * An identity that only lasts as long as a process is not an identity: the
   * name a publisher is known by is the one they can still publish under
   * tomorrow. Whatever holds this is holding the authority to move that name,
   * so it belongs somewhere only its owner can read.
   */
  [[nodiscard]] std::string hex() const;
  [[nodiscard]] static SecretKey fromHex(std::string_view text);
};

/// An ed25519 signature over the bytes BEP 44 asks to be signed.
struct Signature {
  std::array<std::uint8_t, 64> bytes{};

  [[nodiscard]] std::string hex() const;
  [[nodiscard]] static Signature fromHex(std::string_view text);

  bool operator==(const Signature &) const = default;
};

/**
 * @brief Where in the DHT's key space a mutable item lives.
 *
 * Also a twenty-byte SHA-1, and deliberately not an InfoHash. They are the
 * same shape and mean entirely different things -- one names content, the
 * other names a slot -- and a type that let one be passed where the other was
 * meant would turn that confusion into a silent wrong answer.
 */
struct DhtTarget {
  std::array<std::uint8_t, 20> bytes{};

  [[nodiscard]] std::string hex() const;

  bool operator==(const DhtTarget &) const = default;
  [[nodiscard]] bool operator<(const DhtTarget &other) const {
    return bytes < other.bytes;
  }
};

/**
 * @brief A BEP 46 reference: `magnet:?xs=urn:btpk:<hex key>&s=<hex salt>`.
 *
 * Note `xs` -- "exact source" -- where an ordinary magnet uses `xt`. The two
 * are the same syntax and are told apart by that parameter, which is why a
 * link has to be examined rather than guessed at from its scheme.
 */
struct MutableLink {
  PublicKey key;
  /**
   * @brief The optional salt, as raw bytes.
   *
   * Held decoded, though the link writes it in hex, because it is hashed and
   * signed as bytes. A salt lets one key carry several independent names,
   * which is what lets a publisher run more than one scroll without minting
   * more than one identity.
   */
  std::string salt;
  /// The `dn` parameter. A display hint; it identifies nothing.
  std::string displayName;
  /// `tr` parameters, in order.
  std::vector<std::string> trackers;
  /**
   * @brief An `xt=urn:btih:` hash, when the link carried one as well.
   *
   * Whatever the name pointed at when the link was written down. It is a
   * starting point for a client that cannot reach the DHT, and it is not the
   * identity: the point of a mutable name is that this goes stale.
   */
  std::optional<InfoHash> currentWhenWritten;

  /**
   * @brief Read a BEP 46 link.
   * @throws std::runtime_error if it is not one, or names no public key.
   */
  [[nodiscard]] static MutableLink parse(std::string_view uri);

  /// Whether @p text is a mutable link, as opposed to an ordinary magnet or a
  /// path. Both magnets begin the same way, so this looks for the parameter
  /// rather than the scheme.
  [[nodiscard]] static bool looksLikeMutableLink(std::string_view text);

  /// Write the link back out.
  [[nodiscard]] std::string uri() const;

  /// Where the DHT keeps this name's pointer: SHA-1 of the public key
  /// followed by the salt.
  [[nodiscard]] DhtTarget target() const;
};

/**
 * @brief The exact bytes BEP 44 signs.
 *
 * "Concatenate ("4:salt" length-of-salt ":" salt) "3:seqi" seq "e1:v" len ":"
 * and the encoded value", with the salt part omitted when there is no salt.
 *
 * Written out rather than assembled from a bencoded dictionary because it is
 * not one: it is a fragment of dictionary entries with no surrounding `d` and
 * `e`, so an encoder would not produce it. Getting this wrong does not fail
 * loudly -- it produces a signature that nobody else's client accepts, and a
 * name that resolves for its author alone.
 *
 * @param encodedValue The payload, already bencoded.
 */
[[nodiscard]] std::string mutableSigningBuffer(std::string_view salt,
                                               std::int64_t sequence,
                                               std::string_view encodedValue);

/// The payload BEP 46 stores under a name: a dictionary whose `ih` is the
/// twenty raw bytes of the info hash being pointed at.
[[nodiscard]] std::string encodeMutablePointer(const InfoHash &hash);

/// The info hash a payload points at, or nothing when it is not a BEP 46
/// payload at all. A key may hold any bencoded value; one holding something
/// else is not a mistake to be salvaged, it is somebody else's use of the DHT.
[[nodiscard]] std::optional<InfoHash>
decodeMutablePointer(std::string_view encodedValue);

} // namespace xudu

#endif // XUDU_MUTABLE_LINK_H
