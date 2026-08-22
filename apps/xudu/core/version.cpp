#include "version.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xudu {

std::uint32_t Version::length() const { return total; }

std::size_t Version::splitAt(const std::uint32_t offset) {
  // Appending is the common case -- it is what typing at the end of a document
  // is -- and it used to walk every piece to discover that there was nothing
  // to split. The end of the list is where it belongs, and that is known
  // without looking.
  if (offset >= total) {
    return runs.size();
  }
  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < runs.size(); i++) {
    if (seen == offset) {
      return i;
    }
    const auto after = seen + runs[i].length;
    if (offset < after) {
      // The offset falls inside this piece: cut it in two so that a boundary
      // exists where the caller wants to work.
      const auto into = static_cast<std::uint64_t>(offset) - seen;
      const auto tail = runs[i].slice(into, runs[i].length - into);
      runs[i]         = runs[i].slice(0, into);
      runs.insert(runs.begin() + static_cast<std::ptrdiff_t>(i) + 1, tail);
      return i + 1;
    }
    seen = after;
  }
  // Past the end, which is a valid place to insert: the end of the list.
  return runs.size();
}

namespace {

/// Whether @p second carries straight on from @p first: the same scroll, and
/// beginning exactly where the other ends.
///
/// Two such pieces stand for precisely what one piece across both would, so
/// keeping them apart records the history of how the text was typed rather
/// than anything about the document. Nothing above here can tell the
/// difference: every question asked of a version -- what it says, which
/// addresses it holds, where a quotation of it appears -- is answered from
/// addresses, and the addresses are the same either way.
[[nodiscard]] bool joins(const PrimediaSpan &first,
                         const PrimediaSpan &second) {
  return first.scroll == second.scroll && first.end() == second.start;
}

} // namespace

void Version::insert(const std::uint32_t at, const PrimediaSpan &span) {
  if (span.empty()) {
    return;
  }
  const auto where = splitAt(std::min(at, length()));
  total += static_cast<std::uint32_t>(span.length);

  // Typing carries on from what was typed a moment ago, so the new span
  // usually continues the piece before it. Lengthening that piece rather than
  // adding another is what keeps a document that has been typed into for an
  // hour down to a handful of pieces instead of one per keystroke -- and the
  // number of pieces is the multiplier on every other cost here.
  if (where > 0 && joins(runs[where - 1], span)) {
    runs[where - 1].length += span.length;
    joinFollowing(where - 1);
    return;
  }

  runs.insert(runs.begin() + static_cast<std::ptrdiff_t>(where), span);
  joinFollowing(where);
  // No search for empty pieces: the span was checked above, and splitAt()
  // either lands on a boundary and splits nothing or cuts strictly inside a
  // piece, leaving two non-empty halves.
}

void Version::joinFollowing(const std::size_t index) {
  if (index + 1 < runs.size() && joins(runs[index], runs[index + 1])) {
    runs[index].length += runs[index + 1].length;
    runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(index) + 1);
  }
}

void Version::insertSpans(const std::uint32_t at,
                          const std::vector<PrimediaSpan> &spans) {
  // Gathered and inserted once. Inserting them one at a time shifted the tail
  // of the vector per span, which for a quotation of many pieces is the same
  // work repeated for no reason.
  std::vector<PrimediaSpan> wanted;
  wanted.reserve(spans.size());
  std::uint64_t added = 0;
  for (const auto &span : spans) {
    // An empty span is normally nothing to insert, but a break marker is
    // also zero-length by definition (see breakMarkerScroll) and must not be
    // dropped here: this is the path rearrange() uses to put back what
    // remove() took out, and a break caught inside a moved range should move
    // with it rather than silently vanish.
    if (span.empty() && breakMarkerScroll != span.scroll) {
      continue;
    }
    added += span.length;
    // Joined on the way in as well, since a quotation of a run of consecutive
    // pieces is a quotation of one stretch.
    if (!wanted.empty() && joins(wanted.back(), span)) {
      wanted.back().length += span.length;
      continue;
    }
    wanted.push_back(span);
  }
  if (wanted.empty()) {
    return;
  }

  const auto where = splitAt(std::min(at, length()));
  runs.insert(runs.begin() + static_cast<std::ptrdiff_t>(where), wanted.begin(),
              wanted.end());
  total += static_cast<std::uint32_t>(added);
  // And at both seams, where the batch meets what was already there.
  joinFollowing(where + wanted.size() - 1);
  if (where > 0) {
    joinFollowing(where - 1);
  }
  // Empty spans were dropped on the way in, and splitting leaves none, so
  // there is nothing here to compact either.
}

void Version::insertBreak(const std::uint32_t at) {
  const auto where = splitAt(std::min(at, length()));
  const PrimediaSpan marker{breakMarkerScroll, 0, 0};
  runs.insert(runs.begin() + static_cast<std::ptrdiff_t>(where), marker);
  // Merge with a marker already sitting on either side, the same seams
  // insertSpans() checks for a batch of real pieces.
  joinFollowing(where);
  if (where > 0) {
    joinFollowing(where - 1);
  }
}

std::vector<std::uint32_t> Version::forcedBreaks() const {
  std::vector<std::uint32_t> breaks;
  std::uint32_t seen = 0;
  for (const auto &run : runs) {
    if (breakMarkerScroll == run.scroll) {
      breaks.push_back(seen);
    }
    seen += static_cast<std::uint32_t>(run.length);
  }
  return breaks;
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
    // A break marker points at no address at all -- see breakMarkerScroll --
    // so it is not among "the spans this range points at" no matter how the
    // doc comment's range happens to fall around it. This is what keeps a
    // transclusion from ever picking one up: Store::replay's Transclude case
    // builds what it inserts from exactly this method.
    if (breakMarkerScroll != run.scroll && after > from && seen < to) {
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

  // Cut at both ends first, so that the pieces between them are exactly what
  // is going and no piece is partly kept.
  const auto first = splitAt(at);
  const auto last  = splitAt(at + count);
  // Taken directly from the piece list rather than through spansFor(), which
  // deliberately omits break markers: a marker caught inside the removed
  // range is exactly what a rearrangement (remove() then insertSpans() of
  // what came back) needs to carry along, so that a break embedded in a
  // moved passage moves with it instead of being silently dropped.
  const std::vector<PrimediaSpan> taken(
      runs.begin() + static_cast<std::ptrdiff_t>(first),
      runs.begin() + static_cast<std::ptrdiff_t>(last));
  runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(first),
             runs.begin() + static_cast<std::ptrdiff_t>(last));
  total -= count;
  // What was on either side of the removal may now be consecutive.
  if (first > 0) {
    joinFollowing(first - 1);
  }
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
    // A break marker names no real content -- see breakMarkerScroll -- and
    // asking a reader to resolve it is not just wasted work, it is asking a
    // reader that only knows its own local spool (PrimediaSpool::read(), as
    // opposed to Store::read()'s more forgiving "unreachable reads as
    // nothing") to make sense of a scroll id that was never meant to name
    // anything.
    if (breakMarkerScroll == run.scroll) {
      continue;
    }
    out += reader.read(run);
  }
  return out;
}

std::optional<std::uint64_t>
Version::addressAt(const std::uint32_t offset) const {
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
      found.push_back(
          Extent{seen + into,
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
