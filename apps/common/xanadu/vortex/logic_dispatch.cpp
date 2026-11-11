/**
 * @file logic_dispatch.cpp
 * @brief Card P0 (final): the goal dispatch and the logic compile context.
 *
 * Nothing here is filled in by a later card. It only decides who owns a goal.
 */
#include <functional>
#include <string>

#include "common/xanadu/vortex/logic_compile.hpp"

namespace zigzag::vortex::logic {

void refuseUnlowered(std::string_view ownerCard, std::string_view what) {
  std::string message(kLogicUnavailable);
  message += " (";
  message += ownerCard;
  message += ": ";
  message += what;
  message += ")";
  throw LogicRefusal(message);
}

std::string PredicateKey::display() const {
  return name + "/" + std::to_string(arity);
}

namespace {
// 2^64 / golden ratio: the usual hash_combine mixer, so name and arity hashes
// do not cancel when one is small.
constexpr std::size_t kGoldenRatioMix = 0x9e3779b97f4a7c15ULL;
constexpr unsigned kHashShift         = 6;
} // namespace

std::size_t PredicateKeyHash::operator()(const PredicateKey &k) const noexcept {
  const std::size_t nameHash = std::hash<std::string>{}(k.name);
  return nameHash ^ (std::hash<std::size_t>{}(k.arity) + kGoldenRatioMix +
                     (nameHash << kHashShift));
}

CompileContext makeCompileContext(VortexCore &core, VortexVM &vm,
                                  VortexStdLib &stdlib,
                                  std::span<const CellRef> customPredicates,
                                  std::size_t maxCycles) {
  CompileContext ctx{core, vm, stdlib};
  ctx.store               = stdlib.boundStore();
  ctx.maxCycles           = maxCycles;
  ctx.candidatePredicates = stdlib.stdlibPredicates();
  ctx.candidatePredicates.insert(ctx.candidatePredicates.end(),
                                 customPredicates.begin(),
                                 customPredicates.end());
  registerStdlibRelations(ctx.relations, stdlib);
  registerArenaRelations(ctx.relations, stdlib);
  return ctx;
}

void emitGoal(CompileContext &ctx, FrameScope &scope, Chain &chain,
              CellRef goal) {
  const auto functor = ctx.stdlib.functorOf(goal);
  if (!functor) {
    emitNonTermGoal(ctx, scope, chain, goal);
    return;
  }
  const PredicateKey key{*functor, ctx.stdlib.argumentsOf(goal).size()};

  if (hasUserClauses(ctx, key)) {
    emitUserCall(ctx, scope, chain, key, goal);
  } else if (isControlBuiltin(key)) {
    emitControlBuiltin(ctx, scope, chain, key, goal);
  } else if (ctx.relations.find(key) != nullptr) {
    emitRelationGoal(ctx, scope, chain, key, goal);
  } else {
    emitUnknownPredicate(ctx, scope, chain, key);
  }
}

} // namespace zigzag::vortex::logic
