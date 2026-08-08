#include "version.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xudu {

std::uint32_t Version::length() const {
  std::uint64_t total = 0;
  for (const auto &run : runs) {
    total += run.length;
  }
  return static_cast<std::uint32_t>(total);
}

std::size_t Version::splitAt(const std::uint32_t offset) {
  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < runs.size(); i++) {
    if (seen == offset) {
      return i;
    }
    const auto after = seen + runs[i].length;
    if (offset < after) {
      // The offset falls inside this piece: cut it in two so that a boundary
      // exists where the caller wants to work.
      const auto into  = static_cast<std::uint64_t>(offset) - seen;
      const auto tail  = runs[i].slice(into, runs[i].length - into);
      runs[i]          = runs[i].slice(0, into);
      runs.insert(runs.begin() + static_cast<std::ptrdiff_t>(i) + 1, tail);
      return i + 1;
    }
    seen = after;
  }
  // Past the end, which is a valid place to insert: the end of the list.
  return runs.size();
}

void Version::compact() {
  std::erase_if(runs, [](const PrimediaSpan &run) { return run.empty(); });
}

void Version::insert(const std::uint32_t at, const PrimediaSpan &span) {
  if (span.empty()) {
    return;
  }
  const auto where = splitAt(std::min(at, length()));
  runs.insert(runs.begin() + static_cast<std::ptrdiff_t>(where), span);
  compact();
}

void Version::insertSpans(const std::uint32_t at,
                          const std::vector<PrimediaSpan> &spans) {
  auto where = splitAt(std::min(at, length()));
  for (const auto &span : spans) {
    if (span.empty()) {
      continue;
    }
    runs.insert(runs.begin() + static_cast<std::ptrdiff_t>(where), span);
    where++;
  }
  compact();
}

std::vector<PrimediaSpan> Version::spansFor(const std::uint32_t at,
                                            const std::uint32_t length) const {
  std::vector<PrimediaSpan> taken;
  if (0 == length) {
    return taken;
  }
  const std::uint64_t from = at;
  const std::uint64_t to   = static_cast<std::uint64_t>(at) + length;
  std::uint64_t seen       = 0;

  for (const auto &run : runs) {
    const auto after = seen + run.length;
    if (after > from && seen < to) {
      // The overlap of this piece with the range asked for, expressed as an
      // offset into the piece rather than into the version.
      const auto begin = std::max(seen, from) - seen;
      const auto count = std::min(after, to) - seen - begin;
      taken.push_back(run.slice(begin, count));
    }
    seen = after;
    if (seen >= to) {
      break;
    }
  }
  return taken;
}

std::vector<PrimediaSpan> Version::remove(const std::uint32_t at,
                                          const std::uint32_t length) {
  if (0 == length || at >= this->length()) {
    return {};
  }
  const auto count = std::min(length, this->length() - at);
  const auto taken = spansFor(at, count);

  // Cut at both ends first, so that the pieces between them are exactly what
  // is going and no piece is partly kept.
  const auto first = splitAt(at);
  const auto last  = splitAt(at + count);
  runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(first),
             runs.begin() + static_cast<std::ptrdiff_t>(last));
  compact();
  return taken;
}

void Version::rearrange(const std::uint32_t at, const std::uint32_t length,
                        const std::uint32_t to) {
  if (0 == length || at >= this->length()) {
    return;
  }
  const auto count = std::min(length, this->length() - at);
  // Moving a range into itself has no destination distinct from where it
  // already is, and would otherwise compute an offset inside a range that is
  // about to stop existing.
  if (to > at && to < at + count) {
    return;
  }
  const auto taken = remove(at, count);
  // The removal shifted everything after it, so a destination past the cut
  // moves back by what was taken.
  const auto target = to > at ? to - count : to;
  insertSpans(target, taken);
}

std::string Version::materialize(const SpanReader &reader) const {
  std::string out;
  out.reserve(length());
  for (const auto &run : runs) {
    out += reader.read(run);
  }
  return out;
}

std::optional<std::uint64_t> Version::addressAt(const std::uint32_t offset) const {
  std::uint64_t seen = 0;
  for (const auto &run : runs) {
    const auto after = seen + run.length;
    if (offset < after) {
      return run.start + (static_cast<std::uint64_t>(offset) - seen);
    }
    seen = after;
  }
  return std::nullopt;
}

std::vector<Extent> Version::occurrencesOf(const PrimediaSpan &span) const {
  std::vector<Extent> found;
  if (span.empty()) {
    return found;
  }
  std::uint32_t seen = 0;
  for (const auto &run : runs) {
    const auto shared = run.intersect(span);
    if (!shared.empty()) {
      // Where the shared part sits within this piece, carried back out into
      // the version's own coordinates.
      const auto into = static_cast<std::uint32_t>(shared.start - run.start);
      found.push_back(Extent{seen + into,
                             seen + into + static_cast<std::uint32_t>(shared.length)});
    }
    seen += static_cast<std::uint32_t>(run.length);
  }

  // Adjacent pieces that happen to carry consecutive addresses read as one
  // quotation to a person, and reporting them separately would draw a seam
  // through the middle of it.
  std::vector<Extent> merged;
  for (const auto &extent : found) {
    if (!merged.empty() && merged.back().end == extent.start) {
      merged.back().end = extent.end;
    } else {
      merged.push_back(extent);
    }
  }
  return merged;
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:
