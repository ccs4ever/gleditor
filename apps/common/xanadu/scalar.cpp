#include "scalar.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
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

ScalarValue scalarTimestamp(const std::int64_t epochNanos) {
  return ScalarValue{
      .text = formatUtcTimestampIso8601(epochNanos),
      .kind = ValueKind::Timestamp,
      .bits = std::bit_cast<std::uint64_t>(epochNanos),
  };
}

std::string formatUtcTimestampIso8601(const std::int64_t epochNanos) {
  constexpr std::int64_t kNanosPerSec = 1'000'000'000LL;
  std::int64_t s                      = epochNanos / kNanosPerSec;
  std::int64_t remNanos               = epochNanos % kNanosPerSec;
  if (remNanos < 0) {
    remNanos += kNanosPerSec;
    s -= 1;
  }
  const std::chrono::sys_seconds tp{std::chrono::seconds{s}};
  const auto days = std::chrono::floor<std::chrono::days>(tp);
  const std::chrono::year_month_day ymd{days};
  const std::chrono::hh_mm_ss hms{tp - days};

  const int year       = static_cast<int>(ymd.year());
  const unsigned month = static_cast<unsigned>(ymd.month());
  const unsigned day   = static_cast<unsigned>(ymd.day());
  const unsigned hour  = static_cast<unsigned>(hms.hours().count());
  const unsigned min   = static_cast<unsigned>(hms.minutes().count());
  const unsigned sec   = static_cast<unsigned>(hms.seconds().count());

  if (year >= 0 && year <= 9999) {
    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:09d}Z",
                       year, month, day, hour, min, sec, remNanos);
  }
  if (year < 0) {
    return std::format("{:05d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:09d}Z",
                       year, month, day, hour, min, sec, remNanos);
  }
  return std::format("{:d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:09d}Z", year,
                     month, day, hour, min, sec, remNanos);
}

bool parseUtcTimestampIso8601(const std::string_view text,
                              std::int64_t &outNanos) noexcept {
  if (text.empty()) {
    return false;
  }
  std::size_t pos   = 0;
  bool negativeYear = false;
  if (text[pos] == '-') {
    negativeYear = true;
    ++pos;
  } else if (text[pos] == '+') {
    ++pos;
  }

  // Parse Year
  const std::size_t yearStart = pos;
  while (pos < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
  if (pos - yearStart < 4 || pos >= text.size() || text[pos] != '-') {
    return false;
  }
  int year = 0;
  if (std::from_chars(text.data() + yearStart, text.data() + pos, year).ec !=
      std::errc{}) {
    return false;
  }
  if (negativeYear) {
    year = -year;
  }
  ++pos; // skip '-'

  // Parse Month
  if (pos + 2 > text.size() ||
      std::isdigit(static_cast<unsigned char>(text[pos])) == 0 ||
      std::isdigit(static_cast<unsigned char>(text[pos + 1])) == 0) {
    return false;
  }
  unsigned month = 0;
  std::from_chars(text.data() + pos, text.data() + pos + 2, month);
  pos += 2;
  if (pos >= text.size() || text[pos] != '-') {
    return false;
  }
  ++pos; // skip '-'

  // Parse Day
  if (pos + 2 > text.size() ||
      std::isdigit(static_cast<unsigned char>(text[pos])) == 0 ||
      std::isdigit(static_cast<unsigned char>(text[pos + 1])) == 0) {
    return false;
  }
  unsigned day = 0;
  std::from_chars(text.data() + pos, text.data() + pos + 2, day);
  pos += 2;

  // Validate Date
  const std::chrono::year_month_day ymd{std::chrono::year{year},
                                        std::chrono::month{month},
                                        std::chrono::day{day}};
  if (!ymd.ok()) {
    return false;
  }

  // Separator 'T', 't', or ' '
  if (pos >= text.size() ||
      (text[pos] != 'T' && text[pos] != 't' && text[pos] != ' ')) {
    return false;
  }
  ++pos;

  // Parse Hour:Minute:Second
  if (pos + 8 > text.size() || text[pos + 2] != ':' || text[pos + 5] != ':') {
    return false;
  }
  unsigned hour = 0;
  unsigned min  = 0;
  unsigned sec  = 0;
  if (std::from_chars(text.data() + pos, text.data() + pos + 2, hour).ec !=
          std::errc{} ||
      std::from_chars(text.data() + pos + 3, text.data() + pos + 5, min).ec !=
          std::errc{} ||
      std::from_chars(text.data() + pos + 6, text.data() + pos + 8, sec).ec !=
          std::errc{}) {
    return false;
  }
  pos += 8;
  if (hour > 23 || min > 59 || sec > 60) {
    return false;
  }
  if (sec == 60) {
    sec = 59; // Clamp leap second
  }

  // Optional Fraction (.123456789)
  std::int64_t fractionNanos = 0;
  if (pos < text.size() && (text[pos] == '.' || text[pos] == ',')) {
    ++pos;
    const std::size_t fracStart = pos;
    while (pos < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
      ++pos;
    }
    const std::size_t fracDigits = pos - fracStart;
    if (fracDigits == 0) {
      return false;
    }
    // Convert up to 9 digits with padding or ties-to-even rounding
    if (fracDigits <= 9) {
      std::int64_t val = 0;
      std::from_chars(text.data() + fracStart, text.data() + pos, val);
      for (std::size_t i = fracDigits; i < 9; ++i) {
        val *= 10;
      }
      fractionNanos = val;
    } else {
      std::int64_t val = 0;
      std::from_chars(text.data() + fracStart, text.data() + fracStart + 9,
                      val);
      const char nextChar = text[fracStart + 9];
      const int nextDigit = nextChar - '0';
      // Check if remainder beyond 10th digit has any non-zero digits
      bool hasMoreNonZero = false;
      for (std::size_t i = fracStart + 10; i < pos; ++i) {
        if (text[i] != '0') {
          hasMoreNonZero = true;
          break;
        }
      }
      if (nextDigit > 5 || (nextDigit == 5 && hasMoreNonZero)) {
        val += 1;
      } else if (nextDigit == 5 && !hasMoreNonZero) {
        // Exactly half: ties to even
        if (val % 2 != 0) {
          val += 1;
        }
      }
      fractionNanos = val;
    }
  }

  // Timezone Offset
  int offsetMinutes = 0;
  if (pos < text.size()) {
    if (text[pos] == 'Z' || text[pos] == 'z') {
      ++pos;
    } else if (text[pos] == '+' || text[pos] == '-') {
      const bool negOffset = (text[pos] == '-');
      ++pos;
      if (pos + 2 > text.size()) {
        return false;
      }
      int offH = 0;
      if (std::from_chars(text.data() + pos, text.data() + pos + 2, offH).ec !=
          std::errc{}) {
        return false;
      }
      pos += 2;
      int offM = 0;
      if (pos < text.size() && text[pos] == ':') {
        ++pos;
      }
      if (pos + 2 <= text.size() &&
          std::isdigit(static_cast<unsigned char>(text[pos])) != 0 &&
          std::isdigit(static_cast<unsigned char>(text[pos + 1])) != 0) {
        std::from_chars(text.data() + pos, text.data() + pos + 2, offM);
        pos += 2;
      }
      if (offH > 23 || offM > 59) {
        return false;
      }
      offsetMinutes = offH * 60 + offM;
      if (negOffset) {
        offsetMinutes = -offsetMinutes;
      }
    } else {
      return false;
    }
  }

  if (pos != text.size()) {
    return false; // Trailing garbage
  }

  // Convert civil date and time to seconds since epoch
  const std::chrono::sys_days sd{ymd};
  const auto epochDays = sd.time_since_epoch();
  const std::int64_t totalSec =
      std::chrono::duration_cast<std::chrono::seconds>(epochDays).count() +
      static_cast<std::int64_t>(hour) * 3600LL +
      static_cast<std::int64_t>(min) * 60LL + static_cast<std::int64_t>(sec) -
      static_cast<std::int64_t>(offsetMinutes) * 60LL;

  constexpr std::int64_t kNanosPerSec = 1'000'000'000LL;
  constexpr std::int64_t kMinSec =
      std::numeric_limits<std::int64_t>::min() / kNanosPerSec;
  constexpr std::int64_t kMaxSec =
      std::numeric_limits<std::int64_t>::max() / kNanosPerSec;

  if (totalSec < kMinSec || totalSec > kMaxSec) {
    return false;
  }

  const std::int64_t baseNanos = totalSec * kNanosPerSec;
  if (baseNanos >= 0) {
    if (std::numeric_limits<std::int64_t>::max() - baseNanos < fractionNanos) {
      return false;
    }
  }
  outNanos = baseNanos + fractionNanos;
  return true;
}

std::int64_t
roundInstantToNearestTiesToEven(const std::int64_t nanos,
                                const std::int64_t unitNanos) noexcept {
  if (unitNanos <= 1) {
    return nanos;
  }
  const std::int64_t q = nanos / unitNanos;
  const std::int64_t r = nanos % unitNanos;
  if (r == 0) {
    return nanos;
  }
  const std::int64_t half = unitNanos / 2;
  const std::int64_t absR = r < 0 ? -r : r;
  if (absR < half) {
    return q * unitNanos;
  }
  if (absR > half) {
    const std::int64_t step = (nanos >= 0) ? 1 : -1;
    return (q + step) * unitNanos;
  }
  // Exactly half: ties to even
  if (q % 2 != 0) {
    const std::int64_t step = (nanos >= 0) ? 1 : -1;
    return (q + step) * unitNanos;
  }
  return q * unitNanos;
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
