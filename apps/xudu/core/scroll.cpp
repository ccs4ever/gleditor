#include "scroll.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace xudu {

const ScrollSegment *Scroll::segmentAt(const std::uint64_t offset) const {
  for (const auto &segment : segments) {
    if (segment.covers(offset)) {
      return &segment;
    }
    if (segment.at > offset) {
      // Ordered, so nothing further along can cover it either: this offset
      // falls in a gap between seals, or before the first one.
      break;
    }
  }
  return nullptr;
}

void Scroll::addSegment(ScrollSegment segment) {
  if (0 == segment.length) {
    return;
  }
  // A segment covering the same stretch replaces it rather than joining it.
  // That is what a re-seal is: the same bytes at the same addresses, carried
  // by a different torrent.
  std::erase_if(segments, [&segment](const ScrollSegment &held) {
    return held.at == segment.at && held.length == segment.length;
  });
  const auto where =
      std::ranges::upper_bound(segments, segment.at, {}, &ScrollSegment::at);
  segments.insert(where, std::move(segment));
}

bool Scroll::sameContentAs(const Scroll &other) const {
  if (isNamed() || other.isNamed()) {
    return publisher == other.publisher && salt == other.salt;
  }
  if (segments.empty() || other.segments.empty()) {
    return segments.empty() && other.segments.empty();
  }
  const auto &mine = segments.front();
  const auto &them = other.segments.front();
  return mine.torrent == them.torrent && mine.fileIndex == them.fileIndex;
}

std::string Scroll::describe() const {
  if (isNamed()) {
    return "urn:btpk:" + publisher.hex() + (salt.empty() ? "" : "/" + salt);
  }
  if (segments.empty()) {
    return "an empty scroll";
  }
  const auto &first = segments.front();
  return first.torrent.hex() + "/" + std::to_string(first.fileIndex) +
         (first.path.empty() ? "" : " (" + first.path + ")") +
         (segments.size() > 1
              ? " and " + std::to_string(segments.size() - 1) + " more"
              : "");
}

Scroll Scroll::ofTorrentFile(const InfoHash &torrent,
                             const std::uint32_t fileIndex, std::string path,
                             const std::uint64_t fileOffset,
                             const std::uint64_t fileLength) {
  Scroll scroll;
  // One segment covering the whole file, so scroll offset and file offset are
  // the same number. That equivalence is why this change did not disturb any
  // existing reference: content that is one torrent file always was addressed
  // this way, and now it is a scroll that happens to have one segment.
  scroll.segments.push_back(ScrollSegment{0, fileLength, torrent, fileOffset,
                                          fileIndex, std::move(path)});
  return scroll;
}

} // namespace xudu
