/**
 * @file spool.hpp
 * @brief The primedia spool: content, appended and never changed.
 *
 * "In OSMIC, data is logically saved in the server as two cumulative spools --
 * that is, Append-and-Read-Only files." This is the first of them. Text that
 * an insertion introduced is appended here and stays at that address forever;
 * the second spool records the operations, and lives in store.hpp.
 *
 * Nothing is ever removed. Deleting text in a document removes a pointer to
 * it, not the text -- which is what makes every past state of every document
 * still reachable, and is the difference between this and a file.
 *
 * The address of a run of bytes is what gives Xanadu's transclusion its
 * meaning: "conceptually there is only one copy of anything". Two documents
 * quoting the same passage do not hold two copies of it, they hold two
 * pointers to one address, and that they are the same content is then a fact
 * about the addresses rather than a string comparison that might be a
 * coincidence.
 *
 * Nelson intends the same code to serve other media: "later primedia spools
 * can receive audio samples, video frames, fax scanlines, and other countable
 * data." The unit here is the byte because the only medium so far is text; the
 * spool does not interpret what it holds, so what a unit counts is the one
 * thing that would have to change.
 */
#ifndef XUDU_SPOOL_H
#define XUDU_SPOOL_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xudu {

/**
 * @brief A run of content at a permanent address.
 *
 * Xanadu's addresses are global; these are local to one spool, which is as far
 * as a single-machine store can honestly go. A network address would be this
 * plus the identity of the spool.
 */
struct PrimediaSpan {
  std::uint64_t start{};
  std::uint64_t length{};

  [[nodiscard]] std::uint64_t end() const { return start + length; }
  [[nodiscard]] bool empty() const { return 0 == length; }
  [[nodiscard]] bool contains(const std::uint64_t address) const {
    return address >= start && address < end();
  }

  /**
   * @brief The part of this span that @p other also covers.
   *
   * Empty when they do not meet. This is the whole of how transclusion is
   * detected: two documents show the same content exactly where their spans
   * overlap, and nothing has to compare a single character to find out.
   */
  [[nodiscard]] PrimediaSpan intersect(const PrimediaSpan &other) const {
    const auto from = std::max(start, other.start);
    const auto to   = std::min(end(), other.end());
    return to > from ? PrimediaSpan{from, to - from} : PrimediaSpan{from, 0};
  }

  /// A sub-range of this span, given an offset into it and a length.
  [[nodiscard]] PrimediaSpan slice(const std::uint64_t offset,
                                   const std::uint64_t count) const {
    const auto from = std::min(offset, length);
    return {start + from, std::min(count, length - from)};
  }

  bool operator==(const PrimediaSpan &) const = default;
};

/**
 * @class PrimediaSpool
 * @brief Append-and-read-only storage for content.
 */
class PrimediaSpool {
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

  /// The content at @p span, clamped to what has actually been written.
  [[nodiscard]] std::string read(const PrimediaSpan &span) const;

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
