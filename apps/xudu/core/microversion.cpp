#include "microversion.hpp"

#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace xudu {

MicroversionId MicroversionId::parse(const std::string_view text) {
  // Both spellings of the root, because a program printing state zero writes
  // "0" and a program printing an empty history writes nothing.
  if (text.empty() || "0" == text) {
    return {};
  }

  std::vector<Segment> parsed;
  std::size_t pos = 0;
  char branch     = noBranch;

  while (pos < text.size()) {
    // A branch is introduced by its letter. Every segment after the first must
    // have one -- there is no way to reach a second segment except by
    // branching -- and the first segment may, because the null document can be
    // branched from like any other state: quoting a passage into a second
    // document does exactly that, and the state it produces is a1.
    //
    // Reading one back was the omission. str() has always written a1, so a
    // store holding a quotation into a second document could be written and
    // not read: "expected a number at offset 0".
    if (0 != std::isalpha(static_cast<unsigned char>(text[pos]))) {
      branch = static_cast<char>(
          std::tolower(static_cast<unsigned char>(text[pos])));
      pos++;
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

  return MicroversionId{std::move(parsed)};
}

std::string MicroversionId::str() const {
  if (parts.empty()) {
    return "0";
  }
  std::string out;
  for (const auto &segment : parts) {
    if (noBranch != segment.branch) {
      out.push_back(segment.branch);
    }
    out += std::to_string(segment.number);
  }
  return out;
}

MicroversionId MicroversionId::parent() const {
  if (parts.empty()) {
    // Walking backwards from the root terminates here rather than running off
    // the end, so a caller can loop until isZero() without a separate guard.
    return {};
  }
  auto shorter = parts;
  if (1 == shorter.back().number) {
    // The first state of a branch hangs off whatever the branch came from, so
    // the whole segment goes rather than its number going to zero.
    shorter.pop_back();
  } else {
    shorter.back().number--;
  }
  return MicroversionId{std::move(shorter)};
}

MicroversionId MicroversionId::next() const {
  if (parts.empty()) {
    return MicroversionId{{Segment{noBranch, 1}}};
  }
  auto further = parts;
  further.back().number++;
  return MicroversionId{std::move(further)};
}

MicroversionId MicroversionId::branch(const char letter) const {
  auto branched = parts;
  branched.push_back(
      Segment{static_cast<char>(std::tolower(static_cast<unsigned char>(letter))),
              1});
  return MicroversionId{std::move(branched)};
}

std::vector<MicroversionId> MicroversionId::path() const {
  std::vector<MicroversionId> steps;
  std::vector<Segment> prefix;

  for (const auto &segment : parts) {
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
  if (parts.size() > other.parts.size()) {
    return false;
  }
  for (std::size_t i = 0; i + 1 < parts.size(); i++) {
    if (parts[i] != other.parts[i]) {
      return false;
    }
  }
  if (parts.empty()) {
    // State zero precedes everything except itself.
    return !other.parts.empty();
  }
  const auto &mine  = parts.back();
  const auto &their = other.parts[parts.size() - 1];
  if (mine.branch != their.branch) {
    return false;
  }
  // Same chain and further along it, or the same point and then a branch off
  // it -- which is the case where the names are equal at this segment and
  // `other` carries more.
  return their.number > mine.number ||
         (their.number == mine.number && other.parts.size() > parts.size());
}

bool MicroversionId::operator<(const MicroversionId &other) const {
  const auto common = std::min(parts.size(), other.parts.size());
  for (std::size_t i = 0; i < common; i++) {
    if (parts[i].branch != other.parts[i].branch) {
      return parts[i].branch < other.parts[i].branch;
    }
    if (parts[i].number != other.parts[i].number) {
      return parts[i].number < other.parts[i].number;
    }
  }
  return parts.size() < other.parts.size();
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:
