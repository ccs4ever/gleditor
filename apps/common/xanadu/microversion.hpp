/**
 * @file microversion.hpp
 * @brief Names for states of a document, in a time that branches.
 *
 * OSMIC's objection to ordinary undo is that it is destructive: go back five
 * states, type one character, and the five you came through are gone. Its
 * answer is that time is a graph rather than a line, every state of it stays
 * reachable, and each one has a name.
 *
 * The naming rule is Nelson's: "change 2 creates state 2. A branch is given a
 * letter, after which new integers begin with 1 again; thus change 2a4 creates
 * state 2a4." So a name is a run of numbers and letters -- 2a4, 2a4b3 -- and
 * it is not merely a label. It is the whole of what is needed to rebuild the
 * state, because it spells out the sequence of edits from the empty document
 * that leads to it: 2a4 is reached by doing 1, 2, 2a1, 2a2, 2a3, 2a4 and
 * nothing else. That is why nothing here stores a version.
 *
 * An extension beyond what Nelson wrote down: a branch letter here is not
 * limited to one character. Twenty-six siblings off one state was a limit
 * nothing about the underlying idea asked for, only the single-letter
 * alphabet OSMIC's examples happened to use. Past z, a branch spells the same
 * way a spreadsheet names a column past Z: aa, ab, ..., az, ba, ..., zz, aaa
 * -- bijective base 26, not the base 26 a number would use, which is why z
 * increments to aa rather than repeating a digit. In memory a branch is kept
 * as the ordinal that letter run names (a=1, z=26, aa=27, ...) rather than
 * the letters themselves, which is what keeps comparing two ids plain integer
 * comparison instead of "shorter sorts first, then lexicographic" -- the
 * closed form for ordering bijective base-26 strings correctly, and an easy
 * one to get quietly wrong. The letters are spelled out only where a name
 * meets text: parse() and str().
 */
#ifndef XUDU_MICROVERSION_H
#define XUDU_MICROVERSION_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xanadu {

/**
 * @class MicroversionId
 * @brief The name of one state of a document.
 *
 * Held as the alternating number-and-letter segments it is written with, so
 * that stepping along the sequence is arithmetic rather than string editing.
 * The empty id is state zero, the null document, which every history starts
 * from and which no edit produced.
 *
 * The first two segments live inside the object and a name that needs more
 * spills to the heap. That is not a micro-optimisation: SegmentedOpsSpool
 * keeps one of these per operation in indexLookup, so a std::vector here was
 * an allocation and a pointer chase for every operation a document has ever
 * had -- for a name that is almost always the eight bytes "2" or "2a4" fit
 * in. Two is where the boundary belongs because of what a name means: a state
 * on a chain is one segment, a branch off it is two, and only a branch off a
 * branch reaches for the heap.
 */
class MicroversionId {
public:
  /**
   * @brief One "branch letter, then a number" step of a name.
   *
   * branch is the ordinal a letter run names (see this file's own comment),
   * not the letters: zero means the first segment's absent branch, which
   * cannot be confused with a real one, since a real branch's letters are
   * never empty and so never name ordinal zero.
   */
  struct Segment {
    std::uint32_t branch{noBranch};
    std::uint32_t number{};
    bool operator==(const Segment &) const = default;
  };

  /// The first segment's absent branch, which no letter run ever names.
  static constexpr std::uint32_t noBranch = 0;

  /// Segments a name holds without touching the heap.
  static constexpr std::size_t inlineSegments = 2;

  MicroversionId() = default;
  explicit MicroversionId(std::span<const Segment> aSegments);

  MicroversionId(const MicroversionId &other);
  MicroversionId(MicroversionId &&other) noexcept;
  MicroversionId &operator=(const MicroversionId &other);
  MicroversionId &operator=(MicroversionId &&other) noexcept;
  ~MicroversionId();

  /**
   * @brief Read a name as it is written.
   * @throws std::invalid_argument for anything that is not one: a leading
   *         letter, two letters running, a zero in a segment, a number that
   *         does not fit.
   *
   * "0" and the empty string are both state zero, since that is what a program
   * printing the root and a program printing nothing each produce.
   */
  [[nodiscard]] static MicroversionId parse(std::string_view text);

  /// The name as it is written. State zero is "0".
  [[nodiscard]] std::string str() const;

  /// Whether this is state zero, the empty document no edit produced.
  [[nodiscard]] bool isZero() const { return 0 == count; }

  /// The segments, wherever they are being held. A view rather than a
  /// container: which of the two storages is in use is this class's business
  /// and nobody else's.
  [[nodiscard]] std::span<const Segment> segments() const noexcept {
    return {data(), count};
  }

  /**
   * @brief The state this one was reached from.
   *
   * One step back along the sequence: 2a4 came from 2a3, and 2a1 from 2, since
   * a branch's first state hangs off whatever it branched from. State zero's
   * parent is state zero, which makes walking backwards terminate rather than
   * run off the end.
   */
  [[nodiscard]] MicroversionId parent() const;

  /// The next state along this same chain: 2a4 gives 2a5.
  [[nodiscard]] MicroversionId next() const;

  /**
   * @brief The first state of a new branch off this one, under @p ordinal.
   *
   * 2 with ordinal 1 (the letter "a") gives 2a1. Which ordinal is free is a
   * question about what has been recorded, so the store answers it rather
   * than this; see nextBranchOrdinal().
   */
  [[nodiscard]] MicroversionId branch(std::uint32_t ordinal) const;

  /// The letters (a=1, z=26, aa=27, ...) @p ordinal names. What str() calls
  /// to spell out a branch, exposed so a store enumerating candidate
  /// ordinals can report one back the way a person wrote it.
  [[nodiscard]] static std::string branchLetters(std::uint32_t ordinal);

  /**
   * @brief Every state from the first edit up to and including this one.
   *
   * The edits that rebuild this state, in the order they must be replayed.
   * State zero yields nothing to do, which is correct: it is the empty
   * document. This is the whole of OSMIC's "versioning on demand" -- the name
   * says what to replay, so no version has to be stored anywhere.
   */
  [[nodiscard]] std::vector<MicroversionId> path() const;

  /// Whether @p other is reached by continuing on from this state.
  [[nodiscard]] bool isAncestorOf(const MicroversionId &other) const;

  [[nodiscard]] bool operator==(const MicroversionId &other) const noexcept;
  /// Ordering by the sequence names are replayed in, so that a container of
  /// these iterates in an order a person would recognise.
  [[nodiscard]] bool operator<(const MicroversionId &other) const;

private:
  /// Where the segments are, which is the inline buffer until there are more
  /// of them than it holds. `count` is the only thing that says which, so
  /// nothing has to be kept consistent with anything else.
  [[nodiscard]] Segment *data() noexcept {
    return count > inlineSegments ? heapParts : inlineParts;
  }
  [[nodiscard]] const Segment *data() const noexcept {
    return count > inlineSegments ? heapParts : inlineParts;
  }

  /// Point at storage for @p n segments, leaving their values unwritten. The
  /// id must own nothing when this is called.
  void takeStorageFor(std::size_t n);

  /// Give back the heap block, if this name was long enough to have one.
  void release() noexcept;

  /// A spilled name's block is exactly as long as the name, so there is no
  /// capacity to record: every operation that changes a name's length builds
  /// a new one at the length it ends up. Names are made by copy-and-step --
  /// parent(), next(), branch() -- rather than grown in place, so there is no
  /// repeated append for a capacity to have amortised.
  union {
    Segment inlineParts[inlineSegments]{};
    Segment *heapParts;
  };
  std::uint32_t count{0};
};

/// Three words. The inline buffer is two of them and it replaced a
/// std::vector that was three, so a name got smaller and stopped allocating
/// at the same time. SegmentedOpsSpool::indexLookup holds one per operation,
/// which is where any growth here would be paid.
static_assert(sizeof(MicroversionId) <= 3 * sizeof(std::uint64_t));

} // namespace xanadu

#endif // XUDU_MICROVERSION_H
