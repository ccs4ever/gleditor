#pragma once

/**
 * @file lt_compat.hpp
 * @brief The two places libtorrent 2.1 differs from the 2.0 distributions
 *        ship, so the swarm builds against either.
 *
 * 2.1 is not packaged anywhere yet (Debian, Ubuntu, Fedora and Homebrew are
 * all on 2.0.x), and the pieces of it this tree reaches for -- the peer
 * connection behind a `peer_connection_handle`, and bdecode's string
 * accessors -- are exactly the two that moved. Neither shim costs anything on
 * 2.1: the alias resolves to the same type and `sv()` is a no-op conversion.
 */

#include <string_view>

#include <libtorrent/peer_connection_handle.hpp>
#include <libtorrent/string_view.hpp>
#include <libtorrent/version.hpp>

#if LIBTORRENT_VERSION_NUM >= 20100
#include <libtorrent/aux_/peer_connection.hpp>
#else
#include <libtorrent/peer_connection.hpp>
#endif

namespace xanadu::lt_compat {

/// 2.1 moved the internal connection type into `libtorrent::aux`. Only ever
/// named to build a `peer_connection_handle`, never dereferenced.
#if LIBTORRENT_VERSION_NUM >= 20100
using peer_connection = libtorrent::aux::peer_connection;
#else
using peer_connection = libtorrent::peer_connection;
#endif

/// A handle onto no connection, which is what a plugin under test holds: its
/// sends go to the test's frame sink rather than to a socket.
inline libtorrent::peer_connection_handle nullPeerConnection() {
  return libtorrent::peer_connection_handle(std::weak_ptr<peer_connection>{});
}

/// `lt::string_view` is `std::string_view` only from 2.1 on; before that it
/// is `boost::string_view`, which shares the interface but not the type, so
/// everything bdecode hands back needs converting on the way into this tree's
/// own `std::string_view` signatures.
inline std::string_view sv(const libtorrent::string_view view) {
  return {view.data(), view.size()};
}

/// The same conversion the other way, for the keys bdecode's lookups take.
inline libtorrent::string_view ltsv(const std::string_view view) {
  return {view.data(), view.size()};
}

} // namespace xanadu::lt_compat
