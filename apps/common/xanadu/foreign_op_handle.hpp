#ifndef COMMON_XANADU_FOREIGN_OP_HANDLE_HPP
#define COMMON_XANADU_FOREIGN_OP_HANDLE_HPP

#include <expected>
#include <string>
#include <string_view>

#include "extern_ref.hpp"

namespace zigzag {
class ArenaManifold;
}
namespace xanadu {
class SpanReader;

enum class ForeignOpHandleError {
  WrongKind,
  InvalidDescriptor,
  UnsupportedVersion,
  UnavailableHistory,
  AbsentOperation,
};

inline constexpr std::string_view cellRelocationDimension = "d.mesh.relocation";
inline constexpr std::string_view cellRelocationMarker    = "cell";
[[nodiscard]] bool isCellRelocationHandle(const zigzag::Manifold &manifold,
                                          zigzag::CellRef handle,
                                          const SpanReader &reader);
[[nodiscard]] bool isCellRelocationHandle(const zigzag::ArenaManifold &arena,
                                          zigzag::CellRef handle);

/// Versioned, bounded descriptor. No local operation index appears in it.
[[nodiscard]] std::string writeForeignOpHandle(const GlobalOpRef &target);
[[nodiscard]] std::expected<GlobalOpRef, ForeignOpHandleError>
readForeignOpHandle(std::string_view descriptor);

[[nodiscard]] std::expected<GlobalOpRef, ForeignOpHandleError>
foreignOpTarget(const zigzag::Manifold &manifold, zigzag::CellRef handle,
                const SpanReader &reader);
[[nodiscard]] std::expected<GlobalOpRef, ForeignOpHandleError>
foreignOpTarget(const zigzag::ArenaManifold &arena, zigzag::CellRef handle,
                const SpanReader *reader = nullptr);

/// The operation's owning history is inseparable from its local index. A
/// resolved Insert/SetValue/etc. is an operation, never a cell placeholder.
struct ResolvedForeignOperation {
  const Store &store;
  std::uint32_t index;
};
[[nodiscard]] std::expected<ResolvedForeignOperation, ForeignOpHandleError>
resolveForeignOperation(const GlobalOpRef &target, const Store &history,
                        const Scroll &sealedAs);
} // namespace xanadu
#endif
