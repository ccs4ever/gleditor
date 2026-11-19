// V12: a step on a stream that may hold dynamic view handles. A plain cell is
// stepped as ever; a handle is stepped by folding its view's rules (cells,
// read by the graph) over its source. Nothing here is known while compiling
// except the step: which view, which source and which rule fires are cells.
#include "common/xanadu/vql/compiler.hpp"

#include <algorithm>
#include <array>
#include <string>

#include <gleditor/logging.hpp>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

namespace {

constexpr std::int64_t forward = 1;

} // namespace

CellRef VQLCompiler::emitLinkRead(const CellRef from, const DimRef dimension) {
  const CellRef result = core_.arena().makeCell();
  const auto read      = emitOp(OpcodeKind::Link, "#VIEW_READ");
  emitInput(read, from);
  emitInputConstant(read, static_cast<std::int64_t>(dimension));
  emitInputConstant(read, forward);
  emitOutput(read, result);
  return result;
}

CellRef VQLCompiler::emitBinary(const OpcodeKind kind, const CellRef left,
                                const CellRef right,
                                const std::string_view label) {
  const CellRef result = core_.arena().makeCell();
  const auto op        = emitOp(kind, label);
  emitInput(op, left);
  emitInput(op, right);
  emitOutput(op, result);
  return result;
}

void VQLCompiler::emitRequire(const CellRef condition,
                              const std::string &message) {
  const auto require = emitOp(OpcodeKind::Assert, "#VIEW_REQUIRE");
  emitInput(require, condition);
  emitInputConstant(require, message);
}

void VQLCompiler::emitJump(const CellRef target) {
  const auto jump = emitOp(OpcodeKind::BranchFalseCell, "#VIEW_JUMP");
  emitInputConstant(jump, false);
  emitInput(jump, target);
}

CellRef VQLCompiler::emitViewHandle(const CellRef source,
                                    const CellRef viewNumber,
                                    const ViewDimensions &dims) {
  auto &arena         = core_.arena();
  const auto numberOf = [&](const CellRef cell) {
    const CellRef number = arena.makeCell();
    const auto convert   = emitOp(OpcodeKind::IntOfCell, "#VIEW_NUMBER");
    emitInput(convert, cell);
    emitOutput(convert, number);
    return number;
  };
  const auto box = [&](const CellRef scalar) {
    const CellRef boxed = arena.makeCell();
    const auto make     = emitOp(OpcodeKind::VqlBoxScalar, "#VIEW_BOX");
    emitInput(make, scalar);
    emitOutput(make, boxed);
    return boxed;
  };
  const CellRef sourceBox = box(numberOf(source));
  const CellRef viewBox   = box(viewNumber);
  // The handle's own text is a display copy taken now; a step reads the live
  // source through the box, never this copy.
  const CellRef copy = arena.makeCell();
  const auto read    = emitOp(OpcodeKind::VqlString, "#VIEW_COPY");
  emitInput(read, source);
  emitOutput(read, copy);
  const CellRef handle = box(copy);
  for (const auto &[dimension, target] :
       {std::pair{dims.marker, sourceBox}, std::pair{dims.of, viewBox}}) {
    const auto link = emitOp(OpcodeKind::Link, "#VIEW_NAME");
    emitInput(link, handle);
    emitInputConstant(link, static_cast<std::int64_t>(dimension));
    emitInputConstant(link, forward);
    emitInput(link, target);
  }
  return handle;
}

std::vector<CellRef> VQLCompiler::emitViewStep(
    const PathStep &step, const std::span<const CellRef> stream,
    const bool hasContext, const std::span<const PathStep> rest,
    const ViewDimensions &dims) {
  const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
  // Attributes a handle cannot take are named when one arrives, not when the
  // step is compiled: whether a cell is a handle is a fact of the run.
  std::optional<std::string> refusal = viewStepRefusal(step);
  if (dimension && !refusal) {
    if (dimension->placement != Placement::Default)
      refusal = "VQL a placement modifier on a step through a dynamic view is "
                "not compiled to Vortex; only dimension steps, then value() or "
                "count(), follow a view";
    else if (mounts_.contains(dimension->dimName))
      refusal = "VQL mounted dimension '" + dimension->dimName +
                "' on a step through a dynamic view is not compiled to Vortex";
  }

  // The computed rules of the views this compile can see: their expressions
  // are compiled here, once per view, and picked by the view's number.
  struct Candidate {
    std::int64_t view{};
    const VirtualDimensionDefRule *rule{};
  };
  std::vector<Candidate> candidates;
  if (dimension) {
    // Named while compiling, as the plain step does, so the fold's dimension
    // lookup finds it.
    (void)resolveDimension(dimension->dimName);
    for (const CellRef cell : stream) {
      const auto parts = handleParts(core_.arena(), dims, cell);
      if (!parts) continue;
      const auto found = shapeHandles_.find(parts->view);
      if (found == shapeHandles_.end()) continue;
      const auto *computed = std::get_if<ComputedViewRules>(&found->second);
      if (!computed) continue;
      const VirtualDimensionDefRule *match = nullptr;
      for (const VirtualDimensionDefRule &rule : computed->rules)
        if (rule.dimName == dimension->dimName &&
            rule.direction == dimension->direction)
          match = &rule;
      const auto number = static_cast<std::int64_t>(parts->view);
      if (match && std::ranges::none_of(candidates, [&](const auto &seen) {
            return seen.view == number;
          }))
        candidates.push_back({.view = number, .rule = match});
    }
  }

  const CellRef absentCell =
      emitConstant(CellValue(static_cast<std::int64_t>(zigzag::noCell)));
  const CellRef falseConst = emitConstant(CellValue(false));
  const auto text          = [&](const std::string_view value) {
    return emitConstant(CellValue(std::string{value}));
  };

  return emitMap(stream, [&](const CellRef current) {
    const CellRef out    = emitStreamClear();
    const CellRef marker = emitLinkRead(current, dims.marker);
    const CellRef plain =
        emitBinary(OpcodeKind::Eq, marker, absentCell, "#VIEW_IS_PLAIN");
    const CellRef handleLabel = mintOpcode(OpcodeKind::Nop, "#VIEW_HANDLE");
    const CellRef end         = mintOpcode(OpcodeKind::Nop, "#VIEW_STEP_END");
    const auto toHandle = emitOp(OpcodeKind::BranchFalseCell, "#VIEW_DISPATCH");
    emitInput(toHandle, plain);
    emitInput(toHandle, handleLabel);

    // A plain cell: the step as it was always compiled.
    emitAppendStream(out, compileSelector(step, current, hasContext));
    emitJump(end);

    appendOpcode(handleLabel);
    const auto cellOf = [&](const CellRef box, const std::string &gone) {
      const CellRef cell = core_.arena().makeCell();
      const auto convert = emitOp(OpcodeKind::CellOfInt, "#VIEW_CELL");
      emitInput(convert, box);
      emitOutput(convert, cell);
      emitRequire(emitCellPresence(cell), gone);
      return cell;
    };
    const CellRef source = cellOf(
        marker, "VQL a dynamic view handle names a source that is no longer a "
                "cell: the cell it was made over was released");
    if (refusal) {
      emitRequire(falseConst, *refusal);
    } else if (!dimension) {
      // value() and count() leave the view: they answer about the source.
      emitAppendStream(out, compileSelector(step, source, hasContext));
    } else {
      const CellRef ofBox = emitLinkRead(current, dims.of);
      const CellRef view  = cellOf(
          ofBox, "VQL a dynamic view handle names a view that is no longer a "
                  "cell: the cell it was made with was released");
      const CellRef viewNumber = core_.arena().makeCell();
      const auto numberOp      = emitOp(OpcodeKind::IntOfCell, "#VIEW_NUMBER");
      emitInput(numberOp, view);
      emitOutput(numberOp, viewNumber);

      // The fold. `allowed` starts from the view's base flag and each rule, in
      // source order, shows or hides the step; a pivot on the step's own
      // dimension also redirects it. Every name here but the rule's own is a
      // constant of the compile.
      const auto copyOf = [&](const CellRef value) {
        const CellRef copy = core_.arena().makeCell();
        const auto add     = emitOp(OpcodeKind::Add, "#VIEW_COPY_FLAG");
        emitInput(add, value);
        emitOutput(add, copy);
        return copy;
      };
      const auto notOf = [&](const CellRef value) {
        const CellRef result = core_.arena().makeCell();
        const auto negate    = emitOp(OpcodeKind::Not, "#VIEW_FOLD_NOT");
        emitInput(negate, value);
        emitOutput(negate, result);
        return result;
      };
      const auto binary = [&](const OpcodeKind kind, const CellRef left,
                              const CellRef right) {
        return emitBinary(kind, left, right, "#VIEW_FOLD");
      };
      // Resolved the way a rule's own name is, so the two compare as the
      // same kind of value whatever the VM does with a dimension number.
      const CellRef stepNumber = core_.arena().makeCell();
      const auto stepResolve = emitOp(OpcodeKind::DimByName, "#VIEW_STEP_DIM");
      emitInput(stepResolve, text(dimension->dimName));
      emitInputConstant(stepResolve, std::int64_t{0});
      emitOutput(stepResolve, stepNumber);
      const CellRef stepDirection   = emitConstant(CellValue(
          static_cast<std::int64_t>(zigzag::toSign(dimension->direction))));
      const CellRef allowed         = copyOf(emitLinkRead(view, dims.base));
      const CellRef physical        = copyOf(stepNumber);
      const CellRef computedHit     = copyOf(falseConst);
      const std::int64_t mintTarget = dimension->creates.empty() ? 0 : 1;

      const auto rules = emitOp(OpcodeKind::RankBegin, "#VIEW_RULES");
      emitInput(rules, view);
      emitInputConstant(rules, static_cast<std::int64_t>(dims.rule));
      emitInputConstant(rules, forward);
      const CellRef iterator = core_.arena().makeCell();
      emitOutput(rules, iterator);
      const CellRef allText      = text(viewKindAll);
      const CellRef noneText     = text(viewKindNone);
      const CellRef maskText     = text(viewKindMask);
      const CellRef pivotText    = text(viewKindPivot);
      const CellRef computedText = text(viewKindComputed);
      emitIteratorLoop(iterator, [&](const CellRef rule) {
        const CellRef fromDim = core_.arena().makeCell();
        const auto resolve    = emitOp(OpcodeKind::DimByName, "#VIEW_RULE_DIM");
        emitInput(resolve, rule);
        emitInputConstant(resolve, std::int64_t{0});
        emitOutput(resolve, fromDim);
        const CellRef kind       = emitLinkRead(rule, dims.kind);
        const CellRef target     = emitLinkRead(rule, dims.to);
        const CellRef negated    = emitLinkRead(rule, dims.neg);
        const CellRef bidir      = emitLinkRead(rule, dims.bidir);
        const CellRef direction  = emitLinkRead(rule, dims.dir);
        const CellRef isAll      = binary(OpcodeKind::Eq, kind, allText);
        const CellRef isNone     = binary(OpcodeKind::Eq, kind, noneText);
        const CellRef isMask     = binary(OpcodeKind::Eq, kind, maskText);
        const CellRef isPivot    = binary(OpcodeKind::Eq, kind, pivotText);
        const CellRef isComputed = binary(OpcodeKind::Eq, kind, computedText);
        const CellRef sameDim    = binary(OpcodeKind::Eq, fromDim, stepNumber);
        const CellRef sameWay =
            binary(OpcodeKind::Eq, direction, stepDirection);
        const CellRef maskHit =
            binary(OpcodeKind::And, isMask,
                   binary(OpcodeKind::And, sameDim,
                          binary(OpcodeKind::Or, bidir, sameWay)));
        const CellRef pivotHit = binary(OpcodeKind::And, isPivot, sameDim);
        const CellRef computedHit1 =
            binary(OpcodeKind::And, isComputed,
                   binary(OpcodeKind::And, sameDim, sameWay));
        const CellRef shows = binary(
            OpcodeKind::Or, isAll,
            binary(OpcodeKind::Or, pivotHit,
                   binary(OpcodeKind::Or, computedHit1,
                          binary(OpcodeKind::And, maskHit, notOf(negated)))));
        const CellRef hides   = binary(OpcodeKind::Or, isNone,
                                       binary(OpcodeKind::And, maskHit, negated));
        const CellRef kept    = binary(OpcodeKind::And, allowed, notOf(hides));
        const auto setAllowed = emitOp(OpcodeKind::Or, "#VIEW_ALLOWED");
        emitInput(setAllowed, shows);
        emitInput(setAllowed, kept);
        emitOutput(setAllowed, allowed);
        const auto setComputed = emitOp(OpcodeKind::Or, "#VIEW_COMPUTED");
        emitInput(setComputed, computedHit);
        emitInput(setComputed, computedHit1);
        emitOutput(setComputed, computedHit);
        const CellRef skip = mintOpcode(OpcodeKind::Nop, "#VIEW_NO_PIVOT");
        const auto notPivot =
            emitOp(OpcodeKind::BranchFalseCell, "#VIEW_PIVOT_TEST");
        emitInput(notPivot, pivotHit);
        emitInput(notPivot, skip);
        const auto redirect = emitOp(OpcodeKind::DimByName, "#VIEW_PIVOT_DIM");
        emitInput(redirect, target);
        emitInputConstant(redirect, mintTarget);
        emitOutput(redirect, physical);
        appendOpcode(skip);
      });

      // A hidden step reaches nothing and creates nothing.
      const auto hidden = emitOp(OpcodeKind::BranchFalseCell, "#VIEW_HIDDEN");
      emitInput(hidden, allowed);
      emitInput(hidden, end);

      const CellRef notComputed =
          mintOpcode(OpcodeKind::Nop, "#VIEW_NOT_COMPUTED");
      const auto toPlainRule =
          emitOp(OpcodeKind::BranchFalseCell, "#VIEW_COMPUTED_TEST");
      emitInput(toPlainRule, computedHit);
      emitInput(toPlainRule, notComputed);
      for (const Candidate &candidate : candidates) {
        const CellRef thisView =
            binary(OpcodeKind::Eq, viewNumber,
                   emitConstant(CellValue(candidate.view)));
        const CellRef next = mintOpcode(OpcodeKind::Nop, "#VIEW_NEXT_VIEW");
        const auto other =
            emitOp(OpcodeKind::BranchFalseCell, "#VIEW_WHICH_VIEW");
        emitInput(other, thisView);
        emitInput(other, next);
        const VirtualDimensionDefRule &rule = *candidate.rule;
        std::optional<std::string> why;
        if (!dimension->creates.empty())
          why = "VQL computed dimension '" + rule.dimName +
                "' of a dynamic view cannot be written (%): it has no put "
                "transform";
        else if (dimension->isBidir)
          why = "VQL computed dimension '" + rule.dimName +
                "' of a dynamic view does not take a bidirectional or "
                "placement modifier";
        else
          why = computedResultEndsRefusal(rule, rest);
        if (why) {
          emitRequire(falseConst, *why);
        } else {
          const std::array<CellRef, 1> sources{source};
          emitAppendStream(out, compileComputedDimension(rule, sources));
        }
        emitJump(end);
        appendOpcode(next);
      }
      emitRequire(
          falseConst,
          "VQL computed dimension '" + dimension->dimName +
              "' of a dynamic view handle this session cannot resolve is not "
              "compiled to Vortex: a computed rule's expression is compiled "
              "where the view is stepped, so only a view this session holds "
              "can answer it");
      appendOpcode(notComputed);

      const DimOperand target = DimOperand::fromSlot(physical);
      const auto wrapInto     = [&](const CellRef reached) {
        const std::array<CellRef, 1> handle{
            emitViewHandle(reached, viewNumber, dims)};
        emitAppendStream(out, handle);
      };
      if (!dimension->creates.empty()) {
        for (const CellRef created :
             compileDimensionCreation(source, *dimension, target))
          wrapInto(created);
      } else {
        const auto walk = [&](const DimVector way) {
          const auto begin = emitOp(OpcodeKind::RankBegin, "#VIEW_WALK");
          emitInput(begin, source);
          emitInput(begin, physical);
          emitInputConstant(begin,
                            static_cast<std::int64_t>(zigzag::toSign(way)));
          const CellRef walkIterator = core_.arena().makeCell();
          emitOutput(begin, walkIterator);
          emitIteratorLoop(walkIterator, wrapInto);
        };
        if (dimension->isBidir) {
          walk(DimVector::POS);
          walk(DimVector::NEG);
        } else {
          walk(dimension->direction);
        }
      }
    }
    appendOpcode(end);
    return std::vector<CellRef>{out};
  });
}

} // namespace xanadu::vql
