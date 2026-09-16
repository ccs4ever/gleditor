/**
 * @file crum_node.hpp
 * @brief Forwarding alias to gleditor/enfilade/crum_node.hpp.
 *
 * The generic enfilade primitives (the Dsp/Wid monoid concepts and CrumNode
 * itself) have no xanadu-specific content and live in the core library; this
 * stays as the include spelling the other filades here (spanfilade,
 * chronofilade, holefilade, arrayfilade) already use.
 */
#ifndef XANADU_ENFILADE_CRUM_NODE_HPP
#define XANADU_ENFILADE_CRUM_NODE_HPP

#include <gleditor/enfilade/crum_node.hpp>

namespace xanadu::enfilade {
using namespace ::gleditor::enfilade;
} // namespace xanadu::enfilade

#endif // XANADU_ENFILADE_CRUM_NODE_HPP
