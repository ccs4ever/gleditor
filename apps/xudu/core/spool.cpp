#include "spool.hpp"
#include "segmented_primedia_spool.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace xudu {

class PrimediaSpool::Impl {
public:
  SegmentedPrimediaSpool spool;
};

PrimediaSpool::PrimediaSpool() : impl(std::make_unique<Impl>()) {}
PrimediaSpool::~PrimediaSpool() = default;

PrimediaSpan PrimediaSpool::append(const std::string_view bytes) {
  return impl->spool.append(bytes);
}

std::string PrimediaSpool::read(const PrimediaSpan &span) const {
  if (!span.isLocal()) {
    throw std::runtime_error("primedia spool: asked for a span into scroll " +
                             std::to_string(span.scroll) +
                             ", which is not the local spool");
  }
  return impl->spool.read(span);
}

std::string_view
PrimediaSpool::readView(const PrimediaSpan &span) const {
  return impl->spool.readView(span);
}

std::uint64_t PrimediaSpool::size() const { return impl->spool.size(); }

std::string_view PrimediaSpool::bytes() const {
  return impl->spool.bytes();
}

void PrimediaSpool::adopt(const std::string_view stored) {
  impl->spool.adopt(stored);
}

} // namespace xudu
