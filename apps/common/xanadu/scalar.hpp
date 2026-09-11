/**
 * @file scalar.hpp
 * @brief A scalar cell's two halves: the bytes it reads as, and the bits it is.
 *
 * A cell holding 42.0 carries *both*. The span is ordinary spooled permascroll
 * text, which is what makes the scalar a link endpoint, formattable through
 * LinkType::Format, transcludable, diffable and transcopyright-bearing like any
 * other content. The bits are the typed value, so a query compares, adds and
 * entangles without ever parsing text or round-tripping through a formatter.
 *
 * Neither half is a lossy view of the other, and that is the point of doing it
 * this way rather than picking one:
 *
 * - The rendering is `std::to_chars` with **no precision argument** -- shortest
 *   round-trip, which the standard requires to be exact. That makes it a
 *   mathematical function of the value rather than a formatting choice, so two
 *   cells holding the same double always spool the same bytes.
 * - Canonicalisation applies to the **bits only**, and only so that value
 *   equality works: every NaN collapses to one quiet pattern and -0.0 to +0.0.
 *   It is emphatically *not* an address canonicalisation -- two people who
 *   typed 3.14 have not quoted each other, and giving numeric coincidence a
 *   shared primedia address would classify it as DiffKind::Universal and light
 *   up Identity Gold for a transclusion that never happened. See design R6,
 *   which rejects the "scroll of all doubles" on exactly that ground, and
 *   user_permascroll.hpp, which removed findExistingSpan for the same reason.
 *
 * The arithmetic that makes "same value implies same address" unsound anyway:
 * 2^53 - 2 bit patterns are NaN and none compares equal to anything including
 * itself, and +-0.0 is two patterns for one value.
 */
#ifndef XUDU_SCALAR_HPP
#define XUDU_SCALAR_HPP

#include <cstdint>
#include <string>

#include "ops.hpp"

namespace xanadu {

/// The quiet NaN every NaN canonicalises to. The standard's preferred pattern:
/// exponent all ones, the mantissa's high bit set and nothing else.
inline constexpr std::uint64_t canonicalQuietNaN = 0x7ff8000000000000ULL;

/**
 * @brief Whether @p value is a *signalling* NaN.
 *
 * Refused at the API boundary rather than canonicalised, because a signalling
 * NaN is a request to raise an exception on use, and storing one would mean
 * every later reader of that cell inherits a trap the author did not ask for.
 * Canonicalising it instead would quietly discard that intent.
 *
 * Read off the bit pattern rather than via std::isnan, since the question is
 * about which NaN this is rather than whether it is one. Note that on an
 * architecture that quiets signalling NaNs when they pass through a register
 * (x87, unlike SSE), an sNaN may already have been quieted before it gets
 * here; that is the platform's doing and not something this can undo.
 */
[[nodiscard]] bool isSignallingNaN(double value) noexcept;

/**
 * @brief @p value's canonical bits: NaN collapsed, negative zero normalised.
 *
 * For value equality only. Two cells whose bits compare equal hold the same
 * value; two cells holding the same value do *not* thereby share an address.
 */
[[nodiscard]] std::uint64_t canonicalDoubleBits(double value) noexcept;

/// A scalar ready to become a cell: what to spool, what `flags` should say, and
/// what goes in CompactOpNode::value.
struct ScalarValue {
  std::string text; ///< the shortest round-trip rendering
  ValueKind kind{ValueKind::None};
  std::uint64_t bits{0}; ///< canonical, per canonicalDoubleBits()

  bool operator==(const ScalarValue &) const = default;
};

/**
 * @brief @p value as a scalar cell's two halves.
 *
 * @throws std::invalid_argument if @p value is a signalling NaN.
 */
[[nodiscard]] ScalarValue scalarValue(double value);

/// `true` and `false`, spelled out. A bool's bits are 1 and 0.
[[nodiscard]] ScalarValue scalarValue(bool value);

[[nodiscard]] ScalarValue scalarValue(std::int64_t value);

/// The double @p text renders, if it renders one exactly. Round-tripping a
/// rendering is the property a test asserts and a reader of a foreign cell
/// checks; it is not how a query reads a value, which is what the bits are for.
[[nodiscard]] bool parseDouble(std::string_view text, double &out) noexcept;

} // namespace xanadu

#endif // XUDU_SCALAR_HPP
