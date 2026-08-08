#include "spool.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

namespace xudu {

PrimediaSpan PrimediaSpool::append(const std::string_view bytes) {
  const PrimediaSpan span{localScroll,
                          static_cast<std::uint64_t>(contents.size()),
                          static_cast<std::uint64_t>(bytes.size())};
  contents.append(bytes);
  return span;
}

std::string PrimediaSpool::read(const PrimediaSpan &span) const {
  // A span into a torrent cannot be answered from here, and answering with
  // whatever happens to sit at that offset locally would be worse than not
  // answering: a caller cannot tell substituted bytes from the real thing,
  // which is the failure content addressing exists to prevent. The store is
  // the reader that knows about every scroll.
  if (!span.isLocal()) {
    throw std::runtime_error(
        "primedia spool: asked for a span into scroll " +
        std::to_string(span.scroll) + ", which is not the local spool");
  }
  // Clamped rather than checked, because a span can outlive the run that
  // produced it only by being read from a store written by an older version --
  // and returning what is there beats refusing to open the document.
  const auto from =
      std::min<std::size_t>(static_cast<std::size_t>(span.start), contents.size());
  const auto to = std::min<std::size_t>(static_cast<std::size_t>(span.end()),
                                        contents.size());
  return contents.substr(from, to - from);
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:
