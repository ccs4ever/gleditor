#include "common/xanadu/vpl/graph_emit.hpp"

#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xanadu::vpl::emit {

namespace {

using zigzag::vortex::OpcodeKind;

// A label is a Nop outside the chain, so the VM cannot tell a branch to an
// unplaced label from a branch to the end of the program. Compile time can: the
// registry remembers which labels some branch targets, and a loop helper checks
// the labels minted inside its body once the body returns. Labels minted
// outside any loop are checked where the whole graph is assembled (see
// unplacedBranchTarget in execution.hpp).
struct LabelKey {
  const void *arena;
  CellRef label;
  bool operator==(const LabelKey &) const = default;
};

struct LabelKeyHash {
  std::size_t operator()(const LabelKey &key) const noexcept {
    return std::hash<const void *>{}(key.arena) ^
           (std::hash<CellRef>{}(key.label) * 0x9e3779b97f4a7c15ULL);
  }
};

struct LabelRegistry {
  std::unordered_map<LabelKey, bool, LabelKeyHash> targeted;
  std::vector<LabelKey> mintedInLoops;
  std::size_t loopDepth{0};
};

LabelRegistry &registry() {
  thread_local LabelRegistry instance;
  return instance;
}

const void *arenaKey(LowerContext &ctx) { return &ctx.core().arena(); }

bool isPlaced(LowerContext &ctx, CellRef label) {
  const auto &arena = ctx.core().arena();
  const auto spin   = ctx.core().dims().spin;
  return arena.linked(label, spin, zigzag::DimVector::NEG).has_value() ||
         arena.linked(label, spin, zigzag::DimVector::POS).has_value();
}

void noteTarget(LowerContext &ctx, CellRef target) {
  registry().targeted[LabelKey{arenaKey(ctx), target}] = true;
}

// Brackets a loop body: every label minted while it runs must be placed
// before it returns if anything branches to it, because the body cannot
// branch out of its loop.
class BodyScope {
public:
  explicit BodyScope(LowerContext &ctx)
      : ctx_(ctx), mark_(registry().mintedInLoops.size()) {
    ++registry().loopDepth;
  }
  ~BodyScope() {
    auto &state = registry();
    --state.loopDepth;
    state.mintedInLoops.resize(mark_);
  }
  BodyScope(const BodyScope &)            = delete;
  BodyScope &operator=(const BodyScope &) = delete;

  void verify() const {
    const auto &state = registry();
    for (std::size_t i = mark_; i < state.mintedInLoops.size(); ++i) {
      const auto &key = state.mintedInLoops[i];
      if (key.arena != arenaKey(ctx_)) continue;
      const auto found = state.targeted.find(key);
      if (found != state.targeted.end() && found->second &&
          !isPlaced(ctx_, key.label)) {
        throw std::invalid_argument(
            "VPL graph emit: a loop body branches to a label it never placed");
      }
    }
  }

private:
  LowerContext &ctx_;
  std::size_t mark_;
};

CellRef freshRegister(LowerContext &ctx) {
  return ctx.core().arena().makeCell();
}

std::int64_t asValue(DimRef dimension) {
  return static_cast<std::int64_t>(dimension);
}

std::int64_t asValue(DimVector direction) {
  return static_cast<std::int64_t>(zigzag::toSign(direction));
}

// An Add of two constants: the one way a counter is born as a plain integer.
void zero(LowerContext &ctx, CellRef counter, std::string_view label) {
  const auto op = ctx.emitOp(OpcodeKind::Add, label);
  ctx.emitInputConstant(op, std::int64_t{0});
  ctx.emitInputConstant(op, std::int64_t{0});
  ctx.emitOutput(op, counter);
}

void increment(LowerContext &ctx, CellRef counter, std::string_view label) {
  const auto op = ctx.emitOp(OpcodeKind::Add, label);
  ctx.emitInput(op, counter);
  ctx.emitInputConstant(op, std::int64_t{1});
  ctx.emitOutput(op, counter);
}

CellRef beginRankIterator(LowerContext &ctx, CellRef anchor, DimRef dimension,
                          DimVector direction) {
  const auto begin = ctx.emitOp(OpcodeKind::RankBegin, "#RANK_BEGIN");
  ctx.emitInput(begin, anchor);
  ctx.emitInputConstant(begin, asValue(dimension));
  ctx.emitInputConstant(begin, asValue(direction));
  const auto iterator = freshRegister(ctx);
  ctx.emitOutput(begin, iterator);
  return iterator;
}

// Anchor first, then the rank. The rank is snapshotted into a stream before
// the body runs, so a body that mints at the rank's tail cannot lengthen the
// walk it is part of.
CellRef beginWalk(LowerContext &ctx, CellRef anchor, DimRef dimension,
                  DimVector direction) {
  const auto rank  = freshRegister(ctx);
  const auto clear = ctx.emitOp(OpcodeKind::StreamClear, "#WALK_CLEAR");
  ctx.emitOutput(clear, rank);
  const auto cursor = beginRankIterator(ctx, anchor, dimension, direction);
  iteratorLoop(ctx, cursor, [&](CellRef item) {
    const auto append = ctx.emitOp(OpcodeKind::StreamAppend, "#WALK_YIELD");
    ctx.emitInput(append, item);
    ctx.emitOutput(append, rank);
  });
  const auto begin = ctx.emitOp(OpcodeKind::StreamBegin, "#WALK_BEGIN");
  ctx.emitInput(begin, anchor);
  ctx.emitInput(begin, rank);
  const auto iterator = freshRegister(ctx);
  ctx.emitOutput(begin, iterator);
  return iterator;
}

} // namespace

CellRef mintLabel(LowerContext &ctx, std::string_view label) {
  const auto op = ctx.mintOpcode(OpcodeKind::Nop, label);
  auto &state   = registry();
  const LabelKey key{arenaKey(ctx), op};
  state.targeted[key] = false;
  if (state.loopDepth > 0) state.mintedInLoops.push_back(key);
  return op;
}

void placeLabel(LowerContext &ctx, CellRef label) {
  if (isPlaced(ctx, label)) {
    throw std::invalid_argument("VPL graph emit: label placed twice");
  }
  ctx.appendOpcode(label);
}

void branchIfFalse(LowerContext &ctx, CellRef condition, CellRef target,
                   std::string_view label) {
  const auto branch = ctx.emitOp(OpcodeKind::BranchFalseCell, label);
  ctx.emitInput(branch, condition);
  ctx.emitInput(branch, target);
  noteTarget(ctx, target);
}

void branchAlways(LowerContext &ctx, CellRef target, std::string_view label) {
  const auto branch = ctx.emitOp(OpcodeKind::BranchFalseCell, label);
  ctx.emitInputConstant(branch, false);
  ctx.emitInput(branch, target);
  noteTarget(ctx, target);
}

void iteratorLoop(LowerContext &ctx, CellRef iterator,
                  gleditor::cpp26::function_ref<void(CellRef item)> body) {
  const auto next = ctx.emitOp(OpcodeKind::IteratorNext, "#ITERATOR_NEXT");
  ctx.emitInput(next, iterator);
  const auto item    = freshRegister(ctx);
  const auto present = freshRegister(ctx);
  ctx.emitOutput(next, item);
  ctx.emitOutput(next, present);
  const auto end = mintLabel(ctx, "#ITERATOR_END");
  branchIfFalse(ctx, present, end, "#ITERATOR_EXHAUSTED");
  {
    const BodyScope scope(ctx);
    body(item);
    scope.verify();
  }
  // The back-edge targets the IteratorNext op itself, which is in the chain.
  const auto repeat =
      ctx.emitOp(OpcodeKind::BranchFalseCell, "#ITERATOR_REPEAT");
  ctx.emitInputConstant(repeat, false);
  ctx.emitInput(repeat, next);
  placeLabel(ctx, end);
}

void walkRank(LowerContext &ctx, CellRef anchor, DimRef dimension,
              DimVector direction,
              gleditor::cpp26::function_ref<void(CellRef item)> body) {
  const auto iterator = beginWalk(ctx, anchor, dimension, direction);
  iteratorLoop(ctx, iterator, body);
}

void walkRankAfter(LowerContext &ctx, CellRef anchor, DimRef dimension,
                   DimVector direction,
                   gleditor::cpp26::function_ref<void(CellRef item)> body) {
  const auto iterator = beginRankIterator(ctx, anchor, dimension, direction);
  iteratorLoop(ctx, iterator, body);
}

void walkRanksLockstep(
    LowerContext &ctx, CellRef anchorA, DimRef dimensionA, DimVector directionA,
    CellRef anchorB, DimRef dimensionB, DimVector directionB,
    gleditor::cpp26::function_ref<void(CellRef itemA, CellRef itemB)> body) {
  const auto iteratorA = beginWalk(ctx, anchorA, dimensionA, directionA);
  const auto iteratorB = beginWalk(ctx, anchorB, dimensionB, directionB);

  const auto nextA = ctx.emitOp(OpcodeKind::IteratorNext, "#LOCKSTEP_NEXT_A");
  ctx.emitInput(nextA, iteratorA);
  const auto itemA    = freshRegister(ctx);
  const auto presentA = freshRegister(ctx);
  ctx.emitOutput(nextA, itemA);
  ctx.emitOutput(nextA, presentA);

  const auto nextB = ctx.emitOp(OpcodeKind::IteratorNext, "#LOCKSTEP_NEXT_B");
  ctx.emitInput(nextB, iteratorB);
  const auto itemB    = freshRegister(ctx);
  const auto presentB = freshRegister(ctx);
  ctx.emitOutput(nextB, itemB);
  ctx.emitOutput(nextB, presentB);

  const auto both    = freshRegister(ctx);
  const auto combine = ctx.emitOp(OpcodeKind::And, "#LOCKSTEP_BOTH");
  ctx.emitInput(combine, presentA);
  ctx.emitInput(combine, presentB);
  ctx.emitOutput(combine, both);

  const auto end = mintLabel(ctx, "#LOCKSTEP_END");
  branchIfFalse(ctx, both, end, "#LOCKSTEP_EXHAUSTED");
  {
    const BodyScope scope(ctx);
    body(itemA, itemB);
    scope.verify();
  }
  const auto repeat =
      ctx.emitOp(OpcodeKind::BranchFalseCell, "#LOCKSTEP_REPEAT");
  ctx.emitInputConstant(repeat, false);
  ctx.emitInput(repeat, nextA);
  placeLabel(ctx, end);
}

CellRef extreme(LowerContext &ctx, CellRef anchor, DimRef dimension,
                DimVector direction) {
  const auto result  = freshRegister(ctx);
  const auto initial = ctx.emitOp(OpcodeKind::CopyCellRefs, "#EXTREME_START");
  ctx.emitInput(initial, anchor);
  ctx.emitOutput(initial, result);
  const auto iterator = beginRankIterator(ctx, anchor, dimension, direction);
  iteratorLoop(ctx, iterator, [&](CellRef item) {
    const auto copy = ctx.emitOp(OpcodeKind::CopyCellRefs, "#EXTREME_NEXT");
    ctx.emitInput(copy, item);
    ctx.emitOutput(copy, result);
  });
  return result;
}

CellRef extentOf(LowerContext &ctx, CellRef anchor, DimRef dimension,
                 DimVector direction) {
  const auto count = freshRegister(ctx);
  zero(ctx, count, "#EXTENT_START");
  walkRank(ctx, anchor, dimension, direction,
           [&](CellRef) { increment(ctx, count, "#EXTENT_STEP"); });
  return count;
}

CellRef countedLoop(LowerContext &ctx, CellRef count,
                    gleditor::cpp26::function_ref<void(CellRef index)> body) {
  const auto index = freshRegister(ctx);
  zero(ctx, index, "#COUNT_START");
  // The test is the loop head and the back-edge target, so no Nop is needed.
  const auto more = freshRegister(ctx);
  const auto test = ctx.emitOp(OpcodeKind::Lt, "#COUNT_TEST");
  ctx.emitInput(test, index);
  ctx.emitInput(test, count);
  ctx.emitOutput(test, more);
  const auto end = mintLabel(ctx, "#COUNT_END");
  branchIfFalse(ctx, more, end, "#COUNT_EXHAUSTED");
  {
    const BodyScope scope(ctx);
    body(index);
    scope.verify();
  }
  increment(ctx, index, "#COUNT_STEP");
  const auto repeat = ctx.emitOp(OpcodeKind::BranchFalseCell, "#COUNT_REPEAT");
  ctx.emitInputConstant(repeat, false);
  ctx.emitInput(repeat, test);
  placeLabel(ctx, end);
  return index;
}

CellRef mintAfter(LowerContext &ctx, CellRef tail, DimRef dimension,
                  DimVector direction) {
  const auto mint = ctx.emitOp(OpcodeKind::New, "#MINT_CELL");
  ctx.emitInput(mint, tail);
  ctx.emitInputConstant(mint, asValue(dimension));
  ctx.emitInputConstant(mint, asValue(direction));

  // An output-less New mints on every execution; the Link read hands the cell
  // it just attached back as a reference register.
  const auto read = ctx.emitOp(OpcodeKind::Link, "#MINT_RESULT");
  ctx.emitInput(read, tail);
  ctx.emitInputConstant(read, asValue(dimension));
  ctx.emitInputConstant(read, asValue(direction));
  const auto fresh = freshRegister(ctx);
  ctx.emitOutput(read, fresh);

  const auto advance = ctx.emitOp(OpcodeKind::CopyCellRefs, "#MINT_ADVANCE");
  ctx.emitInput(advance, fresh);
  ctx.emitOutput(advance, tail);
  return fresh;
}

void copyValue(LowerContext &ctx, CellRef from, CellRef to) {
  const auto write = ctx.emitOp(OpcodeKind::Value, "#COPY_VALUE");
  ctx.emitInput(write, to);
  ctx.emitInputConstant(write, std::int64_t{0});
  ctx.emitInputConstant(write, std::int64_t{-1});
  ctx.emitInput(write, from);
}

CellRef aliasRegister(LowerContext &ctx, CellRef source) {
  const auto alias = freshRegister(ctx);
  const auto copy  = ctx.emitOp(OpcodeKind::CopyCellRefs, "#ALIAS");
  ctx.emitInput(copy, source);
  ctx.emitOutput(copy, alias);
  return alias;
}

void mergeRankInputDependencies(LowerContext &ctx, CellRef result,
                                std::span<const CellRef> inputs) {
  auto &merged = ctx.rankDependencies()[result];
  for (const auto input : inputs) {
    mergeDependencies(merged, ctx.rankDependenciesOf(input));
  }
}

} // namespace xanadu::vpl::emit
