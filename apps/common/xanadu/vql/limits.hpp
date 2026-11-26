#ifndef XANADU_VQL_LIMITS_HPP
#define XANADU_VQL_LIMITS_HPP

#include <cstddef>

namespace xanadu::vql {

/// The longest text a `join` may build by default: 1 MiB, the default bound of
/// one write (`FileReadHostOptions::defaultWriteBytes`; script_runner.cpp
/// asserts they agree), so a joined text always fits one write. A host that
/// raises its write bound raises this with it, through
/// `QueryExecutionOptions::maxJoinedTextBytes`.
inline constexpr std::size_t defaultJoinedTextBytes = 1'048'576;

} // namespace xanadu::vql

#endif
