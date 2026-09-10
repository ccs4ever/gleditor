#include "microversion.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace xanadu {

namespace {

/// Bijective base 26: the letters spell like a spreadsheet column name, so
/// a=1 rather than a=0 and z increments to aa rather than wrapping. Ordinary
/// base 26 would need a true zero digit to do that and 'a' is not one --
/// it is the reason "aa" is not the same name as "a" the way "01" and "1"
/// are the same number.
std::uint32_t branchOrdinalFromLetters(const std::string_view letters) {
  std::uint32_t ordinal = 0;
  for (const char c : letters) {
    ordinal = (ordinal * 26) + (static_cast<std::uint32_t>(c - 'a') + 1);
  }
  return ordinal;
}

} // namespace

void MicroversionId::takeStorageFor(const std::size_t n) {
  if (n > inlineSegments) {
    heapParts = new Segment[n];
  }
  count = static_cast<std::uint32_t>(n);
}

void MicroversionId::release() noexcept {
  if (count > inlineSegments) {
    delete[] heapParts;
  }
  count = 0;
}

MicroversionId::MicroversionId(const std::span<const Segment> aSegments) {
  takeStorageFor(aSegments.size());
  std::copy(aSegments.begin(), aSegments.end(), data());
}

MicroversionId::MicroversionId(const MicroversionId &other)
    : MicroversionId(other.segments()) {}

MicroversionId::MicroversionId(MicroversionId &&other) noexcept {
  if (other.count > inlineSegments) {
    // The block moves as it stands. Its length is the name's length, so
    // taking the pointer takes the capacity with it.
    heapParts = other.heapParts;
  } else {
    std::copy_n(other.inlineParts, other.count, inlineParts);
  }
  count       = other.count;
  other.count = 0;
}

MicroversionId &MicroversionId::operator=(const MicroversionId &other) {
  if (this != &other) {
    const auto theirs = other.segments();
    release();
    takeStorageFor(theirs.size());
    std::copy(theirs.begin(), theirs.end(), data());
  }
  return *this;
}

MicroversionId &MicroversionId::operator=(MicroversionId &&other) noexcept {
  if (this != &other) {
    release();
    if (other.count > inlineSegments) {
      heapParts = other.heapParts;
    } else {
      std::copy_n(other.inlineParts, other.count, inlineParts);
    }
    count       = other.count;
    other.count = 0;
  }
  return *this;
}

MicroversionId::~MicroversionId() { release(); }

bool MicroversionId::operator==(const MicroversionId &other) const noexcept {
  return std::ranges::equal(segments(), other.segments());
}

std::string MicroversionId::branchLetters(std::uint32_t ordinal) {
  std::string letters;
  while (ordinal > 0) {
    ordinal -= 1;
    letters.push_back(static_cast<char>('a' + (ordinal % 26)));
    ordinal /= 26;
  }
  std::reverse(letters.begin(), letters.end());
  return letters;
}

MicroversionId MicroversionId::parse(const std::string_view text) {
  // Both spellings of the root, because a program printing state zero writes
  // "0" and a program printing an empty history writes nothing.
  if (text.empty() || "0" == text) {
    return {};
  }

  std::vector<Segment> parsed;
  std::size_t pos      = 0;
  std::uint32_t branch = noBranch;

  while (pos < text.size()) {
    // A branch is introduced by its letters. Every segment after the first
    // must have some -- there is no way to reach a second segment except by
    // branching -- and the first segment may, because the null document can
    // be branched from like any other state: quoting a passage into a second
    // document does exactly that, and the state it produces is a1.
    //
    // Reading one back was the omission. str() has always written a1, so a
    // store holding a quotation into a second document could be written and
    // not read: "expected a number at offset 0".
    if (0 != std::isalpha(static_cast<unsigned char>(text[pos]))) {
      const auto lettersFrom = pos;
      while (pos < text.size() &&
             0 != std::isalpha(static_cast<unsigned char>(text[pos]))) {
        pos++;
      }
      std::string letters{text.substr(lettersFrom, pos - lettersFrom)};
      for (auto &c : letters) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      branch = branchOrdinalFromLetters(letters);
    } else if (!parsed.empty()) {
      throw std::invalid_argument("microversion \"" + std::string{text} +
                                  "\": expected a branch letter at offset " +
                                  std::to_string(pos));
    }

    const auto digitsFrom = pos;
    std::uint64_t number  = 0;
    while (pos < text.size() &&
           0 != std::isdigit(static_cast<unsigned char>(text[pos]))) {
      number = (number * 10) + static_cast<std::uint64_t>(text[pos] - '0');
      if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("microversion \"" + std::string{text} +
                                    "\": number too large");
      }
      pos++;
    }
    if (pos == digitsFrom) {
      throw std::invalid_argument("microversion \"" + std::string{text} +
                                  "\": expected a number at offset " +
                                  std::to_string(digitsFrom));
    }
    // Segments count from one: the first state of a chain is 1, not 0, so a
    // zero here is a name that could never have been produced.
    if (0 == number) {
      throw std::invalid_argument("microversion \"" + std::string{text} +
                                  "\": segments are numbered from one");
    }
    parsed.push_back(Segment{branch, static_cast<std::uint32_t>(number)});
  }

  return MicroversionId{parsed};
}

std::string MicroversionId::str() const {
  if (isZero()) {
    return "0";
  }
  std::string out;
  for (const auto &segment : segments()) {
    if (noBranch != segment.branch) {
      out += branchLetters(segment.branch);
    }
    out += std::to_string(segment.number);
  }
  return out;
}

MicroversionId MicroversionId::parent() const {
  const auto mine = segments();
  if (mine.empty()) {
    // Walking backwards from the root terminates here rather than running off
    // the end, so a caller can loop until isZero() without a separate guard.
    return {};
  }
  if (1 == mine.back().number) {
    // The first state of a branch hangs off whatever the branch came from, so
    // the whole segment goes rather than its number going to zero.
    return MicroversionId{mine.first(mine.size() - 1)};
  }
  MicroversionId shorter{mine};
  shorter.data()[shorter.count - 1].number--;
  return shorter;
}

MicroversionId MicroversionId::next() const {
  const auto mine = segments();
  if (mine.empty()) {
    const Segment first{noBranch, 1};
    return MicroversionId{std::span{&first, 1}};
  }
  MicroversionId further{mine};
  further.data()[further.count - 1].number++;
  return further;
}

MicroversionId MicroversionId::branch(const std::uint32_t ordinal) const {
  const auto mine = segments();
  MicroversionId branched;
  // Built at its final length rather than grown into: a name is short, and a
  // spilled one holds exactly its own segments and no spare capacity.
  branched.takeStorageFor(mine.size() + 1);
  std::copy(mine.begin(), mine.end(), branched.data());
  branched.data()[mine.size()] = Segment{ordinal, 1};
  return branched;
}

std::vector<MicroversionId> MicroversionId::path() const {
  std::vector<MicroversionId> steps;
  std::vector<Segment> prefix;

  for (const auto &segment : segments()) {
    // Every state this segment passes through, from its first to the one the
    // name stops at. A branch restarts the count at one, which is exactly what
    // makes the name enough to replay from.
    prefix.push_back(Segment{segment.branch, 0});
    for (std::uint32_t number = 1; number <= segment.number; number++) {
      prefix.back().number = number;
      steps.push_back(MicroversionId{prefix});
    }
  }
  return steps;
}

bool MicroversionId::isAncestorOf(const MicroversionId &other) const {
  const auto ours   = segments();
  const auto theirs = other.segments();
  if (ours.size() > theirs.size()) {
    return false;
  }
  for (std::size_t i = 0; i + 1 < ours.size(); i++) {
    if (ours[i] != theirs[i]) {
      return false;
    }
  }
  if (ours.empty()) {
    // State zero precedes everything except itself.
    return !theirs.empty();
  }
  const auto &mine  = ours.back();
  const auto &their = theirs[ours.size() - 1];
  if (mine.branch != their.branch) {
    return false;
  }
  // Same chain and further along it, or the same point and then a branch off
  // it -- which is the case where the names are equal at this segment and
  // `other` carries more.
  return their.number > mine.number ||
         (their.number == mine.number && theirs.size() > ours.size());
}

bool MicroversionId::operator<(const MicroversionId &other) const {
  const auto parts  = segments();
  const auto theirs = other.segments();
  const auto common = std::min(parts.size(), theirs.size());
  for (std::size_t i = 0; i < common; i++) {
    // Correct because branch is the ordinal and not the letters: comparing
    // "aa" < "z" as strings gets this backwards (aa is the 27th branch, z
    // the 26th), and getting it right for letters means comparing length
    // first and lexicographically second. Plain integer comparison of the
    // ordinal is that closed form, for free.
    if (parts[i].branch != theirs[i].branch) {
      return parts[i].branch < theirs[i].branch;
    }
    if (parts[i].number != theirs[i].number) {
      return parts[i].number < theirs[i].number;
    }
  }
  return parts.size() < theirs.size();
}

} // namespace xanadu
