/**
 * @file edl_transform.cpp
 * @brief Implementation of the composable EDL transformation monoid.
 */
#include "common/xanadu/enfilade/edl_transform.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace xanadu::enfilade {

EdlSlice EdlSlice::subSlice(const std::uint32_t offsetInSlice,
                            const std::uint32_t count) const {
  switch (kind) {
  case SliceKind::Source:
    return EdlSlice{SliceKind::Source, count, sourceOffset + offsetInSlice,
                    PrimediaSpan{}};
  case SliceKind::Primedia:
    return EdlSlice{SliceKind::Primedia, count, 0,
                    span.slice(offsetInSlice, count)};
  case SliceKind::Break:
    return EdlSlice{SliceKind::Break, 0, 0,
                    PrimediaSpan{breakMarkerScroll, 0, 0}};
  }
  return {};
}

namespace {

[[nodiscard]] bool canJoin(const EdlSlice &first,
                           const EdlSlice &second) noexcept {
  if (first.kind != second.kind) {
    return false;
  }
  switch (first.kind) {
  case SliceKind::Source:
    return first.sourceOffset + first.length == second.sourceOffset;
  case SliceKind::Primedia:
    return first.span.scroll == second.span.scroll &&
           first.span.end() == second.span.start;
  case SliceKind::Break:
    return true; // Consecutive break markers collapse into one
  }
  return false;
}

void join(EdlSlice &first, const EdlSlice &second) noexcept {
  if (first.kind == SliceKind::Source) {
    first.length += second.length;
  } else if (first.kind == SliceKind::Primedia) {
    first.length += second.length;
    first.span.length += second.span.length;
  }
  // Break: first already has breakMarkerScroll, length == 0.
}

} // namespace

EdlTransform::EdlTransform(const std::uint32_t inputLength)
    : inputLength_(inputLength), outputLength_(inputLength) {
  if (inputLength > 0) {
    slices_.push_back(EdlSlice{SliceKind::Source, inputLength, 0, {}});
  }
}

EdlTransform EdlTransform::identity(const std::uint32_t inputLength) {
  return EdlTransform{inputLength};
}

EdlTransform
EdlTransform::fromOp(const CompactOpNode &node, const std::uint32_t inputLength,
                     const std::vector<PrimediaSpan> &resolvedTranscludeSpans) {
  auto transform = identity(inputLength);
  switch (node.kind) {
  case OpKind::Insert:
    transform.insert(node.at, node.span());
    break;
  case OpKind::Delete:
    transform.remove(node.at, node.length);
    break;
  case OpKind::Rearrange:
    transform.rearrange(node.at, node.length, node.to);
    break;
  case OpKind::PageBreak:
    transform.insertBreak(node.at);
    break;
  case OpKind::Transclude:
    if (!node.span().empty()) {
      transform.insert(node.at, node.span());
    } else {
      transform.insertSpans(node.at, resolvedTranscludeSpans);
    }
    break;
  case OpKind::Link:
  case OpKind::Structure:
    break;
  }
  return transform;
}

std::size_t EdlTransform::splitAt(const std::uint32_t offset) {
  if (offset >= outputLength_) {
    return slices_.size();
  }
  std::uint32_t seen = 0;
  for (std::size_t i = 0; i < slices_.size(); ++i) {
    if (seen == offset) {
      return i;
    }
    const auto after = seen + slices_[i].length;
    if (offset < after) {
      const auto into = offset - seen;
      const auto tail = slices_[i].subSlice(into, slices_[i].length - into);
      slices_[i]      = slices_[i].subSlice(0, into);
      slices_.insert(slices_.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                     tail);
      return i + 1;
    }
    seen = after;
  }
  return slices_.size();
}

void EdlTransform::joinFollowing(const std::size_t index) {
  if (index + 1 < slices_.size() &&
      canJoin(slices_[index], slices_[index + 1])) {
    join(slices_[index], slices_[index + 1]);
    slices_.erase(slices_.begin() + static_cast<std::ptrdiff_t>(index) + 1);
  }
}

void EdlTransform::insertSlices(const std::uint32_t at,
                                const std::vector<EdlSlice> &slices) {
  std::vector<EdlSlice> wanted;
  wanted.reserve(slices.size());
  std::uint32_t added = 0;
  for (const auto &s : slices) {
    if (s.length == 0 && !s.isBreak()) {
      continue;
    }
    added += s.length;
    if (!wanted.empty() && canJoin(wanted.back(), s)) {
      join(wanted.back(), s);
      continue;
    }
    wanted.push_back(s);
  }
  if (wanted.empty()) {
    return;
  }

  const auto where = splitAt(std::min(at, outputLength_));
  slices_.insert(slices_.begin() + static_cast<std::ptrdiff_t>(where),
                 wanted.begin(), wanted.end());
  outputLength_ += added;
  joinFollowing(where + wanted.size() - 1);
  if (where > 0) {
    joinFollowing(where - 1);
  }
}

void EdlTransform::insert(const std::uint32_t at, const PrimediaSpan &span) {
  if (span.empty() && span.scroll != breakMarkerScroll) {
    return;
  }
  const EdlSlice slice{SliceKind::Primedia,
                       static_cast<std::uint32_t>(span.length), 0, span};
  insertSlices(at, {slice});
}

void EdlTransform::insertSpans(const std::uint32_t at,
                               const std::vector<PrimediaSpan> &spans) {
  std::vector<EdlSlice> slices;
  slices.reserve(spans.size());
  for (const auto &span : spans) {
    if (span.scroll == breakMarkerScroll) {
      slices.push_back(EdlSlice{SliceKind::Break, 0, 0, span});
    } else if (!span.empty()) {
      slices.push_back(EdlSlice{SliceKind::Primedia,
                                static_cast<std::uint32_t>(span.length), 0,
                                span});
    }
  }
  insertSlices(at, slices);
}

void EdlTransform::insertBreak(const std::uint32_t at) {
  const EdlSlice slice{SliceKind::Break, 0, 0,
                       PrimediaSpan{breakMarkerScroll, 0, 0}};
  insertSlices(at, {slice});
}

std::vector<EdlSlice> EdlTransform::remove(const std::uint32_t at,
                                           const std::uint32_t length) {
  if (0 == length || at >= outputLength_) {
    return {};
  }
  const auto count = std::min(length, outputLength_ - at);
  const auto first = splitAt(at);
  const auto last  = splitAt(at + count);
  const std::vector<EdlSlice> taken(
      slices_.begin() + static_cast<std::ptrdiff_t>(first),
      slices_.begin() + static_cast<std::ptrdiff_t>(last));
  slices_.erase(slices_.begin() + static_cast<std::ptrdiff_t>(first),
                slices_.begin() + static_cast<std::ptrdiff_t>(last));
  outputLength_ -= count;
  if (first > 0) {
    joinFollowing(first - 1);
  }
  return taken;
}

void EdlTransform::rearrange(const std::uint32_t at, const std::uint32_t length,
                             const std::uint32_t to) {
  if (0 == length || at >= outputLength_) {
    return;
  }
  const auto count = std::min(length, outputLength_ - at);
  if (to > at && to < at + count) {
    return;
  }
  const auto taken  = remove(at, count);
  const auto target = to > at ? to - count : to;
  insertSlices(target, taken);
}

EdlTransform EdlTransform::compose(const EdlTransform &earlier,
                                   const EdlTransform &later) {
  if (earlier.slices_.empty() && earlier.inputLength_ == 0) {
    return later;
  }
  if (later.slices_.empty() && later.inputLength_ == 0) {
    return earlier;
  }

  EdlTransform result;
  result.inputLength_  = earlier.inputLength_;
  result.outputLength_ = later.outputLength_;

  for (const auto &s : later.slices_) {
    if (s.isBreak()) {
      result.slices_.push_back(s);
      if (result.slices_.size() >= 2) {
        result.joinFollowing(result.slices_.size() - 2);
      }
      continue;
    }
    if (s.isPrimedia()) {
      result.slices_.push_back(s);
      if (result.slices_.size() >= 2) {
        result.joinFollowing(result.slices_.size() - 2);
      }
      continue;
    }

    // Source slice: samples earlier's output [from, to)
    const auto from    = s.sourceOffset;
    const auto to      = from + s.length;
    std::uint32_t seen = 0;

    for (const auto &e : earlier.slices_) {
      const auto after = seen + e.length;
      if (e.isBreak()) {
        if (seen >= from && seen <= to) {
          result.slices_.push_back(e);
          if (result.slices_.size() >= 2) {
            result.joinFollowing(result.slices_.size() - 2);
          }
        }
      } else if (after > from && seen < to) {
        const auto begin = std::max(seen, from) - seen;
        const auto count = std::min(after, to) - seen - begin;
        if (count > 0) {
          result.slices_.push_back(e.subSlice(begin, count));
          if (result.slices_.size() >= 2) {
            result.joinFollowing(result.slices_.size() - 2);
          }
        }
      }
      seen = after;
      if (seen >= to) {
        break;
      }
    }
  }

  return result;
}

Version EdlTransform::applyToVersion(const Version &base) const {
  Version out;
  for (const auto &s : slices_) {
    if (s.isBreak()) {
      out.insertBreak(out.length());
    } else if (s.isPrimedia()) {
      out.insert(out.length(), s.span);
    } else if (s.isSource()) {
      const auto from    = s.sourceOffset;
      const auto to      = from + s.length;
      std::uint32_t seen = 0;
      for (const auto &piece : base.pieces()) {
        const auto after = seen + static_cast<std::uint32_t>(piece.length);
        if (piece.scroll == breakMarkerScroll) {
          if (seen >= from && seen <= to) {
            out.insertBreak(out.length());
          }
        } else if (after > from && seen < to) {
          const auto begin = std::max(seen, from) - seen;
          const auto count = std::min(after, to) - seen - begin;
          if (count > 0) {
            out.insert(out.length(), piece.slice(begin, count));
          }
        }
        seen = after;
        if (seen >= to) {
          break;
        }
      }
    }
  }
  return out;
}

Version EdlTransform::materializeState0() const {
  Version out;
  for (const auto &s : slices_) {
    if (s.isBreak()) {
      out.insertBreak(out.length());
    } else if (s.isPrimedia()) {
      out.insert(out.length(), s.span);
    }
  }
  return out;
}

} // namespace xanadu::enfilade
