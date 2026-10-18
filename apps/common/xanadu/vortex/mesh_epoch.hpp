#ifndef COMMON_XANADU_VORTEX_MESH_EPOCH_HPP
#define COMMON_XANADU_VORTEX_MESH_EPOCH_HPP

#include <expected>
#include <filesystem>
#include <string>

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/mutable_link.hpp"
#include "common/xanadu/vortex/spool_mesh.hpp"

namespace zigzag::vortex::mesh {

/// Reserve a fresh incarnation in an existing private deployment directory.
/// The append-only device journal is locked across processes and synced before
/// returning. Interrupted or malformed writes refuse rather than reset epochs.
/// This state is deployment metadata, never a Store or a permascroll.
/// POSIX filesystems must support flock and durable fsync; other platforms
/// explicitly refuse until an equivalent durable locking backend is available.
[[nodiscard]] std::expected<NodeId, std::string>
reserveNodeId(const std::filesystem::path &deploymentDirectory,
              const xanadu::identity::Fingerprint &author,
              const xanadu::PublicKey &device);

} // namespace zigzag::vortex::mesh
#endif
