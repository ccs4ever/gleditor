#ifndef COMMON_XANADU_VORTEX_MESH_CLI_HPP
#define COMMON_XANADU_VORTEX_MESH_CLI_HPP

#include <optional>

#include "common/xanadu/vortex/mesh_evaluator.hpp"

namespace zigzag::vortex::mesh {
/// Handle an explicit --mesh-* command before the frontend's ordinary parser.
/// Deployment configuration supplies real delegated identities and GPG signing.
[[nodiscard]] std::optional<int> runMeshCommand(Frontend frontend, int argc,
                                                char **argv);
} // namespace zigzag::vortex::mesh
#endif
