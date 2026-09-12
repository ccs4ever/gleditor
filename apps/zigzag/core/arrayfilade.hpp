/**
 * @file arrayfilade.hpp
 * @brief Forwarding alias to apps/common/xanadu/enfilade/arrayfilade.hpp.
 */
#ifndef ZIGZAG_CORE_ARRAYFILADE_FORWARD_HPP
#define ZIGZAG_CORE_ARRAYFILADE_FORWARD_HPP

#include "common/xanadu/enfilade/arrayfilade.hpp"

namespace zigzag {
namespace enfilade = ::xanadu::enfilade;
using enfilade::ArrayCellEntry;
using enfilade::ArrayDsp;
using enfilade::Arrayfilade;
using enfilade::ArrayWid;
using enfilade::Predicate;
using enfilade::PredicateOp;
using enfilade::QueryPlanStats;
} // namespace zigzag

#endif // ZIGZAG_CORE_ARRAYFILADE_FORWARD_HPP
