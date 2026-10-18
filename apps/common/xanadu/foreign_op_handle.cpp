#include "foreign_op_handle.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

#include "binary_ops.hpp"
#include "publication.hpp"
#include "store.hpp"
#include "user_permascroll.hpp"
#include "zigzag/arena_manifold.hpp"
#include "zigzag/manifold.hpp"

namespace xanadu {
namespace {
constexpr std::size_t maximumScrollBytes     = 4096;
constexpr std::size_t maximumDescriptorBytes = 8192;

bool valid(const GlobalOpRef &target) {
  return !target.scroll.empty() && target.scroll.size() <= maximumScrollBytes &&
         !target.produces.isZero() &&
         std::ranges::none_of(target.scroll, [](unsigned char byte) {
           return byte < 0x20 || byte == 0x7f;
         });
}
} // namespace

std::string writeForeignOpHandle(const GlobalOpRef &target) {
  if (!valid(target))
    throw std::invalid_argument("Invalid foreign operation identity");
  std::ostringstream out;
  out.put('\x01');
  writeVarint(out, target.scroll.size());
  out.write(target.scroll.data(),
            static_cast<std::streamsize>(target.scroll.size()));
  const auto version = target.produces.str();
  if (version.size() > maximumScrollBytes) {
    throw std::invalid_argument("Foreign operation name exceeds budget");
  }
  writeVarint(out, version.size());
  out.write(version.data(), static_cast<std::streamsize>(version.size()));
  const auto descriptor = out.str();
  if (descriptor.size() > maximumDescriptorBytes) {
    throw std::invalid_argument("Foreign operation descriptor exceeds budget");
  }
  return descriptor;
}

std::expected<GlobalOpRef, ForeignOpHandleError>
readForeignOpHandle(std::string_view descriptor) {
  if (descriptor.empty() || descriptor.size() > maximumDescriptorBytes) {
    return std::unexpected(ForeignOpHandleError::InvalidDescriptor);
  }
  if (descriptor.front() != '\x01') {
    return std::unexpected(ForeignOpHandleError::UnsupportedVersion);
  }
  try {
    std::istringstream in(std::string{descriptor.substr(1)});
    const auto readText = [&]() {
      std::uint64_t length{};
      if (!readVarint(in, length) || length == 0 ||
          length > maximumScrollBytes || length > descriptor.size()) {
        throw std::invalid_argument(
            "Invalid foreign operation descriptor length");
      }
      std::string text(static_cast<std::size_t>(length), '\0');
      if (!in.read(text.data(), static_cast<std::streamsize>(length))) {
        throw std::invalid_argument("Truncated foreign operation descriptor");
      }
      return text;
    };
    const auto scroll  = readText();
    const auto version = readText();
    const GlobalOpRef target{scroll, MicroversionId::parse(version)};
    if (!valid(target) || version != target.produces.str() ||
        writeForeignOpHandle(target) != descriptor) {
      return std::unexpected(ForeignOpHandleError::InvalidDescriptor);
    }
    return target;
  } catch (const std::exception &) {
    return std::unexpected(ForeignOpHandleError::InvalidDescriptor);
  }
}

std::expected<GlobalOpRef, ForeignOpHandleError>
foreignOpTarget(const zigzag::Manifold &manifold, zigzag::CellRef handle,
                const SpanReader &reader) {
  if (manifold.valueKindOf(handle) != ValueKind::ForeignOpHandle) {
    return std::unexpected(ForeignOpHandleError::WrongKind);
  }
  const auto slot = manifold.slot(handle);
  if (!slot || slot->valueBits != 0) {
    return std::unexpected(ForeignOpHandleError::InvalidDescriptor);
  }
  return readForeignOpHandle(manifold.textOf(handle, reader));
}

bool isCellRelocationHandle(const zigzag::Manifold &manifold,
                            zigzag::CellRef handle, const SpanReader &reader) {
  if (manifold.valueKindOf(handle) != ValueKind::OpHandle) return false;
  return std::ranges::any_of(
      manifold.dimensionsOf(handle), [&](const auto &edge) {
        return edge.pos != zigzag::noCell &&
               manifold.textOf(edge.dim, reader) == cellRelocationDimension &&
               manifold.textOf(edge.pos, reader) == cellRelocationMarker;
      });
}

bool isCellRelocationHandle(const zigzag::ArenaManifold &arena,
                            zigzag::CellRef handle) {
  if (arena.valueKindOf(handle) != ValueKind::OpHandle) return false;
  return std::ranges::any_of(arena.dimensionsOf(handle), [&](const auto &edge) {
    return edge.pos != zigzag::noCell &&
           arena.textOf(edge.dim) == cellRelocationDimension &&
           arena.textOf(edge.pos) == cellRelocationMarker;
  });
}

std::expected<GlobalOpRef, ForeignOpHandleError>
foreignOpTarget(const zigzag::ArenaManifold &arena, zigzag::CellRef handle,
                const SpanReader *reader) {
  if (arena.valueKindOf(handle) != ValueKind::ForeignOpHandle) {
    return std::unexpected(ForeignOpHandleError::WrongKind);
  }
  const auto slot = arena.slot(handle);
  if (!slot || slot->valueBits != 0) {
    return std::unexpected(ForeignOpHandleError::InvalidDescriptor);
  }
  return readForeignOpHandle(arena.textOf(handle, reader));
}

std::expected<ResolvedForeignOperation, ForeignOpHandleError>
resolveForeignOperation(const GlobalOpRef &target, const Store &history,
                        const Scroll &sealedAs) {
  if (!valid(target))
    return std::unexpected(ForeignOpHandleError::InvalidDescriptor);
  if (scrollKey(sealedAs) != target.scroll) {
    return std::unexpected(ForeignOpHandleError::UnavailableHistory);
  }
  const auto index = localiseOpRef(history, target, sealedAs);
  if (!index || !history.getCompactOp(*index)) {
    return std::unexpected(ForeignOpHandleError::AbsentOperation);
  }
  return ResolvedForeignOperation{history, *index};
}

MicroversionId Store::makeForeignOpHandle(const MicroversionId &parent,
                                          const GlobalOpRef &target,
                                          const MicroversionId &context) {
  const auto descriptor = writeForeignOpHandle(target);
  Op op;
  op.kind    = OpKind::Structure;
  op.flags   = structureFlags(StructureVerb::MakeCell, false,
                              ValueKind::ForeignOpHandle);
  op.span    = userPermascroll_->append(descriptor);
  op.value   = 0;
  op.context = context;
  if (op.context.isZero() && sliceBirth_ != 0)
    op.context = opsSpool.idOf(sliceBirth_);
  return apply(parent, op);
}
} // namespace xanadu
