/**
 * @file logic_relations.cpp
 * @brief Card P3: the RelationNext machinery (retry-loop emitter, VM provider
 * adapter) and the stdlib-registry and store providers. Stubs that refuse until
 * P3 fills them in.
 */
#include "common/xanadu/vortex/logic_compile.hpp"

namespace zigzag::vortex::logic {

namespace {
constexpr std::string_view kCard = "P3";
} // namespace

void registerStdlibRelations(RelationRegistry &, VortexStdLib &) {}

void emitRelationGoal(CompileContext &, FrameScope &, Chain &,
                      const PredicateKey &key, CellRef) {
  refuseUnlowered(kCard, "relation " + key.display());
}

void installRelationProvider(CompileContext &) {
  refuseUnlowered(kCard, "installRelationProvider");
}

} // namespace zigzag::vortex::logic
