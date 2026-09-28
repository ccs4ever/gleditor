/**
 * @file result_store.hpp
 * @brief A query's answer written as a slice of its own, to open in xuzz.
 */
#ifndef XANADU_VQL_RESULT_STORE_HPP
#define XANADU_VQL_RESULT_STORE_HPP

#include <cstddef>
#include <span>
#include <string_view>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {
class Store;
}

namespace xanadu::vql {

class MultiStoreCoordinator;

/// What writeResultStore() wrote.
struct ResultStoreStats {
  std::size_t cells{};
  /// Cells whose content quotes the result's own addresses.
  std::size_t quoted{};
  /// Cells whose content had to be written out as text: scratch the query
  /// minted, a scalar's rendering, or a span in another store's scroll.
  std::size_t copied{};
};

/**
 * @brief Write @p results as a new slice in the empty store @p out.
 *
 * The results hang off the home cell in order along d.1 and the query along
 * d.2: the two dimensions ZigZag's default view shows, so the answer is on
 * screen when the store is opened. A result's content is quoted -- its addresses,
 * not a copy -- wherever it is in the shared permascroll, so the answer
 * transcludes what it found; only content with no address to share is
 * written as text. Nothing else is copied: the answer is what was asked for,
 * not the workspace it was found in.
 *
 * @throws std::invalid_argument if @p out already holds operations.
 */
ResultStoreStats writeResultStore(const MultiStoreCoordinator &coordinator,
                                  std::string_view query,
                                  std::span<const zigzag::CellRef> results,
                                  Store &out);

} // namespace xanadu::vql

#endif // XANADU_VQL_RESULT_STORE_HPP
