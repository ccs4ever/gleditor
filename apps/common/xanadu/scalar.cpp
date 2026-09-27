#include "scalar.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "truthiness.hpp"

namespace xanadu {

namespace {

constexpr std::uint64_t exponentMask = 0x7ff0000000000000ULL;
constexpr std::uint64_t mantissaMask = 0x000fffffffffffffULL;
/// The mantissa's high bit: set means quiet, clear means signalling.
constexpr std::uint64_t quietBit = 0x0008000000000000ULL;

/// Long enough for every double `to_chars` can produce. The worst case is 24
/// characters (`-4.9406564584124654e-324`); 32 leaves room without a check
/// that could only ever be dead.
constexpr std::size_t renderingRoom = 32;

bool integerSyntax(const std::string_view text) noexcept {
  if (text.empty()) return false;
  const std::size_t first = text.front() == '-' ? 1 : 0;
  if (first == text.size()) return false;
  return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(first),
                     text.end(),
                     [](const char ch) { return ch >= '0' && ch <= '9'; });
}

} // namespace

InferredTextValue inferTextValue(const std::string_view text) noexcept {
  if (text.empty() || text.size() > maxInferredScalarTextBytes) return {};

  // from_chars does not accept a leading plus, though it is a normal way to
  // enter a signed number. Keep that sign in the text while parsing the value.
  auto numeric = text;
  if (numeric.front() == '+') numeric.remove_prefix(1);
  if (numeric.empty()) return {};

  if (integerSyntax(numeric)) {
    std::int64_t value{};
    const auto done =
        std::from_chars(numeric.data(), numeric.data() + numeric.size(), value);
    if (done.ec == std::errc{} && done.ptr == numeric.data() + numeric.size()) {
      return {.kind = ValueKind::Int64,
              .bits = std::bit_cast<std::uint64_t>(value)};
    }
    // An out-of-range integer must not silently lose precision as a double.
    return {};
  }

  // Numeric 1 and 0 remain integers; the same explicit truthiness literals
  // provide the words that can become boolean cells.
  if (const auto truth = truthLiteral(text)) {
    return {.kind = ValueKind::Bool, .bits = *truth ? 1ULL : 0ULL};
  }

  const bool floatSyntax =
      numeric.find_first_of(".eE") != std::string_view::npos ||
      asciiEqualsIgnoreCase(numeric, "nan") ||
      asciiEqualsIgnoreCase(numeric, "inf") ||
      asciiEqualsIgnoreCase(numeric, "infinity") ||
      asciiEqualsIgnoreCase(numeric, "-nan") ||
      asciiEqualsIgnoreCase(numeric, "-inf") ||
      asciiEqualsIgnoreCase(numeric, "-infinity");
  if (!floatSyntax) return {};
  double value{};
  if (!parseDouble(numeric, value) || isSignallingNaN(value)) return {};
  return {.kind = ValueKind::Double, .bits = canonicalDoubleBits(value)};
}

bool isSignallingNaN(const double value) noexcept {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  return (bits & exponentMask) == exponentMask && (bits & mantissaMask) != 0 &&
         (bits & quietBit) == 0;
}

std::uint64_t canonicalDoubleBits(const double value) noexcept {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  if ((bits & exponentMask) == exponentMask && (bits & mantissaMask) != 0) {
    return canonicalQuietNaN;
  }
  // Covers -0.0 without naming it: it is the one value with two patterns, and
  // 0.0 == -0.0 is true while their bits differ.
  if (0.0 == value) {
    return 0;
  }
  return bits;
}

ScalarValue scalarValue(const double value) {
  if (isSignallingNaN(value)) {
    throw std::invalid_argument(
        "a signalling NaN asks every later reader to raise an exception on "
        "use, which is not a value a cell can carry -- see design R6");
  }
  std::array<char, renderingRoom> buffer{};
  // No precision argument, deliberately: this overload is shortest
  // round-trip, which the standard requires to be exact, so the rendering is a
  // function of the value rather than a choice about how to show it.
  const auto done =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ScalarValue{
      .text = std::string(buffer.data(), done.ptr),
      .kind = ValueKind::Double,
      .bits = canonicalDoubleBits(value),
  };
}

ScalarValue scalarValue(const bool value) {
  return ScalarValue{
      .text = value ? "true" : "false",
      .kind = ValueKind::Bool,
      .bits = value ? 1ULL : 0ULL,
  };
}

ScalarValue scalarValue(const std::int64_t value) {
  std::array<char, renderingRoom> buffer{};
  const auto done =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ScalarValue{
      .text = std::string(buffer.data(), done.ptr),
      .kind = ValueKind::Int64,
      // Two's complement, reinterpreted: `value` is recovered by the reverse
      // cast, so a negative integer round-trips rather than being stored as a
      // huge unsigned one that happens to have the same bits.
      .bits = std::bit_cast<std::uint64_t>(value),
  };
}

bool parseDouble(const std::string_view text, double &out) noexcept {
  const auto *const first = text.data();
  const auto *const last  = text.data() + text.size();
  const auto done         = std::from_chars(first, last, out);
  // The whole string or nothing: a trailing character means this is not a
  // rendering of a double, and answering with the prefix would turn "42x" into
  // 42 silently.
  return done.ec == std::errc{} && done.ptr == last;
}

} // namespace xanadu
