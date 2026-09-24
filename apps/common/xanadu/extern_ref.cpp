/**
 * @file extern_ref.cpp
 * @brief Persistent references to cells in other stores (§5.5).
 */
#include "extern_ref.hpp"

#include <sstream>

#include "binary_ops.hpp"
#include "publication.hpp"
#include "store.hpp"
#include "zigzag/manifold.hpp"

namespace xanadu {

std::string writeGlobalDocumentState(const GlobalDocumentState &state) {
  std::ostringstream oss;
  oss.put('\x01'); // Version 1
  writeVarint(oss, state.scroll.size());
  if (!state.scroll.empty()) {
    oss.write(state.scroll.data(),
              static_cast<std::streamsize>(state.scroll.size()));
  }
  const auto verStr = state.version.str();
  writeVarint(oss, verStr.size());
  if (!verStr.empty()) {
    oss.write(verStr.data(), static_cast<std::streamsize>(verStr.size()));
  }
  return oss.str();
}

std::optional<GlobalDocumentState>
readGlobalDocumentState(const std::string_view bytes) {
  if (bytes.empty()) {
    return std::nullopt;
  }
  std::istringstream iss{std::string(bytes)};
  char version = 0;
  if (!iss.get(version) || version != '\x01') {
    return std::nullopt;
  }

  std::uint64_t scrollLen = 0;
  if (!readVarint(iss, scrollLen)) {
    return std::nullopt;
  }
  std::string scroll(scrollLen, '\0');
  if (scrollLen > 0 &&
      !iss.read(scroll.data(), static_cast<std::streamsize>(scrollLen))) {
    return std::nullopt;
  }

  std::uint64_t verLen = 0;
  if (!readVarint(iss, verLen)) {
    return std::nullopt;
  }
  std::string verStr(verLen, '\0');
  if (verLen > 0 &&
      !iss.read(verStr.data(), static_cast<std::streamsize>(verLen))) {
    return std::nullopt;
  }

  char extra = 0;
  if (iss.get(extra)) {
    return std::nullopt; // Trailing garbage
  }

  try {
    const auto ver = MicroversionId::parse(verStr);
    return GlobalDocumentState{.scroll = std::move(scroll), .version = ver};
  } catch (...) {
    return std::nullopt;
  }
}

ExternResolution resolveExternCell(const Store &localStore,
                                   const zigzag::CellRef placeholder,
                                   const Store &foreignStore,
                                   const Scroll &sealedAs,
                                   const zigzag::Manifold *foreignFold) {
  const auto targetOpt = localStore.externTarget(placeholder);
  if (!targetOpt.has_value()) {
    return ExternResolution{.status = ExternResolutionStatus::Unintelligible};
  }
  const auto &target   = *targetOpt;
  const auto scrollRec = localStore.scrollRegistry().findRecord(target.scroll);
  if (!scrollRec || scrollRec->globalKey.empty()) {
    return ExternResolution{.status = ExternResolutionStatus::Absent};
  }
  const GlobalOpRef globalRef{.scroll   = scrollRec->globalKey,
                              .produces = target.produces};
  const auto opIndexOpt = localiseOpRef(foreignStore, globalRef, sealedAs);
  if (!opIndexOpt.has_value()) {
    return ExternResolution{.status = ExternResolutionStatus::Absent};
  }
  const auto opIndex     = *opIndexOpt;
  const auto *const node = foreignStore.getCompactOp(opIndex);
  if (nullptr == node) {
    return ExternResolution{.status = ExternResolutionStatus::Absent};
  }
  if (node->kind != OpKind::Structure ||
      structureVerbOf(node->flags) != StructureVerb::MakeCell) {
    return ExternResolution{.status = ExternResolutionStatus::Unintelligible};
  }
  if (nullptr != foreignFold && !foreignFold->contains(opIndex)) {
    return ExternResolution{.status = ExternResolutionStatus::Absent};
  }
  return ExternResolution{
      .status  = ExternResolutionStatus::Resolved,
      .cell    = opIndex,
      .opIndex = opIndex,
  };
}

} // namespace xanadu
