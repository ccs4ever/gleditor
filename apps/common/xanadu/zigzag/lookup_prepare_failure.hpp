#ifndef XANADU_ZIGZAG_LOOKUP_PREPARE_FAILURE_HPP
#define XANADU_ZIGZAG_LOOKUP_PREPARE_FAILURE_HPP

#include <cstdint>

namespace zigzag::detail {

enum class LookupPrepareFailure : std::uint8_t {
  AlreadyPresent,
  InvalidOperationIndex,
  CapacityOverflow,
  AllocationUnavailable,
};

} // namespace zigzag::detail

#endif // XANADU_ZIGZAG_LOOKUP_PREPARE_FAILURE_HPP
