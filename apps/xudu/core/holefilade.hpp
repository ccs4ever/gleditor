/**
 * @file holefilade.hpp
 * @brief Forwarding alias to apps/common/xanadu/enfilade/holefilade.hpp.
 */
#ifndef XUDU_CORE_HOLEFILADE_FORWARD_HPP
#define XUDU_CORE_HOLEFILADE_FORWARD_HPP

#include "common/xanadu/enfilade/holefilade.hpp"

namespace xudu {
namespace enfilade = ::xanadu::enfilade;
using enfilade::HoleCrum;
using enfilade::HoleDsp;
using enfilade::Holefilade;
using enfilade::HoleSlice;
using enfilade::HoleSpanEntry;
using enfilade::HoleWid;
using enfilade::PaymentReceipt;
using enfilade::PermascrollSpanState;
using enfilade::ScrollHolefilade;
using enfilade::SettlementLedger;
} // namespace xudu

#endif // XUDU_CORE_HOLEFILADE_FORWARD_HPP
