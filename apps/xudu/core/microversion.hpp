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

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xudu {

/**
 * @class MicroversionId
 * @brief The name of one state of a document.
 *
 * Held as the alternating number-and-letter segments it is written with, so
 * that stepping along the sequence is arithmetic rather than string editing.
 * The empty id is state zero, the null document, which every history starts
 * from and which no edit produced.
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

  MicroversionId() = default;
  explicit MicroversionId(std::vector<Segment> aSegments)
      : parts(std::move(aSegments)) {}

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
  [[nodiscard]] bool isZero() const { return parts.empty(); }

  [[nodiscard]] const std::vector<Segment> &segments() const { return parts; }

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

  bool operator==(const MicroversionId &) const = default;
  /// Ordering by the sequence names are replayed in, so that a container of
  /// these iterates in an order a person would recognise.
  [[nodiscard]] bool operator<(const MicroversionId &other) const;

private:
  std::vector<Segment> parts;
};

} // namespace xudu

#endif // XUDU_MICROVERSION_H
