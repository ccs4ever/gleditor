/**
 * @file lens_uri.hpp
 * @brief openLensUri: one URI scheme -> its lens.
 *
 * The host-facing entry the lens family converges on:
 *   sql:<backend>:<target>  SqlLens   (sqlite today, pg via the probe)
 *   cfg:<path>              ConfigLens (JSON config)
 *   log:<path>              LogLens    (line-ordinal windows)
 *   git:<path>              GitLens    (refs + commit DAG)
 *   dir:<path>, or a bare   RouterLens (filesystem view that mounts
 *   directory path          delegate lenses under its cells)
 *   <bare file path>        FilesystemLens
 *
 * Meta/VqlResult/Swarm lenses need in-process handles (a manifold, an
 * engine, a manager) rather than paths -- they open in code, not via
 * URI. This is the dispatch the xuzz --lens flag and any future host
 * share.
 */
#ifndef COMMON_XANADU_ZIGZAG_LENS_URI_HPP
#define COMMON_XANADU_ZIGZAG_LENS_URI_HPP

#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

/// Open the lens @p uri names. Refuses with unexpected on an unknown
/// scheme or a constructor-time error (a SqlLens constructor reports
/// the backend's open error).
[[nodiscard]] std::expected<std::unique_ptr<ProjectionLens>, std::string>
openLensUri(std::string_view uri);

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_LENS_URI_HPP
