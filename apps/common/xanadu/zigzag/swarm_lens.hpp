/**
 * @file swarm_lens.hpp
 * @brief SwarmLens: the Merkle identity ledger as a projection.
 *
 * design/lens-candidates.md section 3.6, round one: the ledger half of
 * the swarm surface -- the append-only identity ledger's entries are a
 * natural rank with pk-strength sequence identity. The live halves
 * (peers, torrents, pieces) need the running libtorrent session and
 * the evented machinery; they are a later iteration on the same file.
 */
#ifndef COMMON_XANADU_ZIGZAG_SWARM_LENS_HPP
#define COMMON_XANADU_ZIGZAG_SWARM_LENS_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/zigzag/projection_lens.hpp"

namespace xanadu {
class MerkleLedger;
class SystemTorrentManager;
} // namespace xanadu

namespace zigzag {

/// A MerkleLedger's entries as a bounded enumeration projection; with
/// a @ref xanadu::SystemTorrentManager the same slice also lists the
/// managed torrents -- the live half of the swarm surface (peers,
/// state, progress per infohash, re-read per epoch).
class SwarmLens final : public ProjectionLens {
public:
  /// @p label names the key space (swarm:<label>:...); @p ledger is the
  /// append-only source -- re-read per refresh() so appended entries
  /// appear on the next epoch. @p manager, when set, adds the managed
  /// torrents rank under swarm:<label>:t:<infohash>.
  SwarmLens(std::string label, const xanadu::MerkleLedger &ledger,
            const xanadu::SystemTorrentManager *manager = nullptr);

  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override;
  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)> emit)
      const override;
  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)> emit)
      const override;
  bool forEachRoot(gleditor::cpp26::function_ref<void(const ExternalKey &)>
                       emit) const override;

private:
  std::string connKey_; ///< swarm:<label>
  const xanadu::MerkleLedger *ledger_;
  const xanadu::SystemTorrentManager *manager_{nullptr};
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_SWARM_LENS_HPP
