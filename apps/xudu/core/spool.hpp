/**
 * @file spool.hpp
 * @brief Addresses for content, and the local spool that is one home for it.
 *
 * "In OSMIC, data is logically saved in the server as two cumulative spools --
 * that is, Append-and-Read-Only files." This is the first of them: text that
 * an insertion introduced is appended here and stays at that address forever.
 * The second spool records the operations, and lives in store.hpp.
 *
 * Nothing is ever removed. Deleting text in a document removes a pointer to
 * it, not the text -- which is what makes every past state of every document
 * still reachable, and is the difference between this and a file.
 *
 * An address is what gives transclusion its meaning: "conceptually there is
 * only one copy of anything". Two documents quoting the same passage hold two
 * pointers to one address rather than two copies, and that they are the same
 * content is then a fact about the addresses rather than a string comparison
 * that might be a coincidence.
 *
 * Which is why an address has to say *which* content, and not only where in
 * it. An offset into one machine's spool means nothing to anyone else and
 * nothing to you either once that machine is gone. So a span carries a scroll:
 * the local spool, or -- see scroll.hpp -- somebody's append-only sequence,
 * whose name resolves anywhere and whose offsets never move.
 */
#ifndef XUDU_SPOOL_H
#define XUDU_SPOOL_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace xudu {

/**
 * @brief Which body of content an address points into.
 *
 * Zero is the local primedia spool, which is where anything typed here goes.
 * Anything else names an entry in the store's table of scrolls; see Scroll in
 * scroll.hpp. An id is local to one store, and the table is what turns it back
 * into something globally meaningful.
 */
using ScrollId = std::uint32_t;
inline constexpr ScrollId localScroll = 0;

/**
 * @brief A run of content at a permanent address.
 *
 * @p start is an offset into the scroll, and a scroll only ever grows, so the
 * address of a byte is settled the moment it is written and nothing that
 * happens afterwards can move it. In particular it does not depend on which
 * torrent is currently carrying that stretch, which is a fact that changes
 * every time a segment is sealed.
 */
struct PrimediaSpan {
  ScrollId scroll{localScroll};
  std::uint64_t start{};
  std::uint64_t length{};

  [[nodiscard]] std::uint64_t end() const { return start + length; }
  [[nodiscard]] bool empty() const { return 0 == length; }
  [[nodiscard]] bool isLocal() const { return localScroll == scroll; }
  [[nodiscard]] bool contains(const ScrollId which,
                              const std::uint64_t address) const {
    return which == scroll && address >= start && address < end();
  }

  /**
   * @brief The part of this span that @p other also covers.
   *
   * Empty when they do not meet, and empty whenever they are addresses into
   * different content -- byte 100 of one torrent has nothing to do with byte
   * 100 of another, and treating overlapping numbers as overlapping content
   * would report transclusions nobody made.
   *
   * This is the whole of how transclusion is detected: two documents show the
   * same content exactly where their spans overlap, and nothing has to compare
   * a single character to find out.
   */
  [[nodiscard]] PrimediaSpan intersect(const PrimediaSpan &other) const {
    if (scroll != other.scroll) {
      return {scroll, start, 0};
    }
    const auto from = std::max(start, other.start);
    const auto to   = std::min(end(), other.end());
    return to > from ? PrimediaSpan{scroll, from, to - from}
                     : PrimediaSpan{scroll, from, 0};
  }

  /// A sub-range of this span, given an offset into it and a length.
  [[nodiscard]] PrimediaSpan slice(const std::uint64_t offset,
                                   const std::uint64_t count) const {
    const auto from = std::min(offset, length);
    return {scroll, start + from, std::min(count, length - from)};
  }

  bool operator==(const PrimediaSpan &) const = default;
};

/**
 * @brief Something that can turn an address back into bytes.
 *
 * Version::materialize() takes one of these rather than a spool, because a
 * version's pieces may point at content this machine never typed. The store
 * is the implementation that knows about every scroll; the spool below is the
 * one that knows about the local one only.
 */
class SpanReader {
public:
  SpanReader()          = default;
  virtual ~SpanReader() = default;

  SpanReader(const SpanReader &)            = delete;
  SpanReader &operator=(const SpanReader &) = delete;
  SpanReader(SpanReader &&)                 = delete;
  SpanReader &operator=(SpanReader &&)      = delete;

  /**
   * @brief The content at @p span.
   *
   * An implementation that cannot reach the content returns fewer bytes than
   * asked for, or none. It must not return the wrong bytes: a caller has no
   * way to tell a substitution from the real thing, which is exactly the
   * failure content addressing exists to prevent.
   */
  [[nodiscard]] virtual std::string read(const PrimediaSpan &span) const = 0;
};

/**
 * @class PrimediaSpool
 * @brief Append-and-read-only storage for content typed here.
 */
class PrimediaSpool : public SpanReader {
public:
  /**
   * @brief Add content, and say where it landed.
   *
   * Appending the same bytes twice gives two addresses rather than one. That
   * is deliberate: two people who happen to type the same sentence have not
   * quoted each other, and treating them as one would invent a transclusion
   * nobody made.
   */
  PrimediaSpan append(std::string_view bytes);

  /**
   * @brief The content at @p span, clamped to what has been written.
   * @throws std::runtime_error for a span into anything but the local spool.
   *         The spool has no way to reach other content and must not answer
   *         with the wrong bytes.
   */
  [[nodiscard]] std::string read(const PrimediaSpan &span) const override;

  [[nodiscard]] std::uint64_t size() const {
    return static_cast<std::uint64_t>(contents.size());
  }

  /// The whole spool, for writing it out.
  [[nodiscard]] const std::string &bytes() const { return contents; }

  /// Replace the contents wholesale, when reading a stored spool back.
  void adopt(std::string stored) { contents = std::move(stored); }

private:
  std::string contents;
};

} // namespace xudu

#endif // XUDU_SPOOL_H
// vi: set sw=2 sts=2 ts=2 et:
