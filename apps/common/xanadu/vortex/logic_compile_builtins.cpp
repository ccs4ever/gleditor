/**
 * @file logic_compile_builtins.cpp
 * @brief Card P2: control and arithmetic builtins (is, comparisons, \\+, \\=,
 * cut, true, fail). Stubs that refuse until P2 fills them in.
 */
#include "common/xanadu/vortex/logic_compile.hpp"

namespace zigzag::vortex::logic {

namespace {
constexpr std::string_view kCard = "P2";
} // namespace

bool isControlBuiltin(const PredicateKey &) { return false; }

void emitControlBuiltin(CompileContext &, FrameScope &, Chain &,
                        const PredicateKey &key, CellRef) {
  refuseUnlowered(kCard, "builtin " + key.display());
}

} // namespace zigzag::vortex::logic
