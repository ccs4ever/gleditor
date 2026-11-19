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

void VQLCompiler::emitCallRoutine(const CellRef entry) {
  const auto call = emitOp(OpcodeKind::Call, "#VIEW_CALL");
  emitInputConstant(call, static_cast<std::int64_t>(entry));
}

void VQLCompiler::emitWrite(const CellRef slot, const CellValue &value) {
  const auto write = emitOp(OpcodeKind::Value, "#VIEW_ARGUMENT");
  emitInput(write, slot);
  emitInputConstant(write, std::int64_t{0});
  emitInputConstant(write, std::int64_t{-1});
  emitInputConstant(write, value);
}

CellRef VQLCompiler::emitRawCopy(const CellRef value) {
  const CellRef copy = core_.arena().makeCell();
  const auto add     = emitOp(OpcodeKind::Add, "#VIEW_COPY_FLAG");
  emitInput(add, value);
  emitOutput(add, copy);
  return copy;
}

CellRef VQLCompiler::emitReferenceCopy(const CellRef reference) {
  const CellRef copy = core_.arena().makeCell();
  const auto cp      = emitOp(OpcodeKind::CopyCellRefs, "#VIEW_COPY_REF");
  emitInput(cp, reference);
  emitOutput(cp, copy);
  return copy;
}

const VQLCompiler::ViewRoutines &
VQLCompiler::viewRoutines(const ViewDimensions &dims) {
  if (viewRoutines_) return *viewRoutines_;
  // The fold, the handle wrap and the handle's resolution do not depend on the
  // step that calls them, so each is emitted once, as a subroutine on a chain
  // of its own, and a step site writes its arguments into fixed slots and
  // calls it. A site's cost is then its dispatch and its calls, not a copy of
  // the fold. The slots are the program's own cells and a routine never
  // calls another step, so nothing calls one while it runs.
  auto &arena             = core_.arena();
  const auto savedEntry   = entryOp_;
  const auto savedCurrent = currentOp_;
  const bool savedFrame   = compilingFrameDefinition_;
  entryOp_.reset();
  currentOp_.reset();
  compilingFrameDefinition_ = false;
  ViewRoutines routines;
  const auto slot   = [&] { return arena.makeCell(); };
  const auto finish = [&] {
    emitOp(OpcodeKind::Return, "#VIEW_RETURN");
    // Inside a function body the VM requires every op but a jump or a frame
    // return to have a successor; the Return never falls through to it.
    emitOp(OpcodeKind::Nop, "#VIEW_RETURN_END");
    const CellRef entry = *entryOp_;
    entryOp_.reset();
    currentOp_.reset();
    return entry;
  };
  const auto numberOf = [&](const CellRef cell, const CellRef target) {
    const auto convert = emitOp(OpcodeKind::IntOfCell, "#VIEW_NUMBER");
    emitInput(convert, cell);
    emitOutput(convert, target);
    return target;
  };
  const auto box = [&](const CellRef scalar, const CellRef target) {
    const auto make = emitOp(OpcodeKind::VqlBoxScalar, "#VIEW_BOX");
    emitInput(make, scalar);
    emitOutput(make, target);
    return target;
  };
  const auto cellOf = [&](const CellRef reference, const CellRef target,
                          const std::string &gone) {
    const auto convert = emitOp(OpcodeKind::CellOfInt, "#VIEW_CELL");
    emitInput(convert, reference);
    emitOutput(convert, target);
    emitRequire(emitCellPresence(target), gone);
  };
  // The graph's own comparison of a cell's epoch now with the one recorded.
  const auto epochUnchanged = [&](const CellRef cell, const CellRef recorded,
                                  const std::string &reused) {
    const CellRef now  = arena.makeCell();
    const auto observe = emitOp(OpcodeKind::EpochOfCell, "#VIEW_EPOCH");
    emitInput(observe, cell);
    emitOutput(observe, now);
    emitRequire(emitBinary(OpcodeKind::Eq, now, recorded, "#VIEW_EPOCH_SAME"),
                reused);
  };
  const CellRef falseConst = emitConstant(CellValue(false));

  // -- resolve: a handle to its source, its view and the view's number.
  routines.resolveHandle = slot();
  routines.resolveSource = slot();
  routines.resolveView   = slot();
  routines.resolveNumber = slot();
  {
    // Link answers the Int64 box's neighbour, and a CellOfInt of it is a
    // reference to the cell the box names, or the VM's own absent-cell
    // reference when there is none: presence, not a number, is what is asked.
    const CellRef marker = emitLinkRead(routines.resolveHandle, dims.marker);
    cellOf(marker, routines.resolveSource,
           "VQL a dynamic view handle names a source that is no longer a "
           "cell: the cell it was made over was released");
    const CellRef ofBox = emitLinkRead(routines.resolveHandle, dims.of);
    cellOf(ofBox, routines.resolveView,
           "VQL a dynamic view handle names a view that is no longer a cell: "
           "the cell it was made with was released");
    numberOf(routines.resolveView, routines.resolveNumber);

    // The numbers just read are only as good as the epochs recorded beside
    // them: a cell released and its number minted again answers a later
    // epoch. The witness is the rank the host writes when a run settles
    // (source's epoch, then the view's). A handle without one was made by the
    // running program itself, and nothing releases beneath a running program,
    // so its numbers cannot have been reused yet; the wrap step writes no
    // witness, since nothing may be written onto the view or the source and
    // the host records it once the handle is a result or a binding.
    const CellRef sourceEpoch =
        emitLinkRead(routines.resolveHandle, dims.epoch);
    const CellRef unwitnessed =
        mintOpcode(OpcodeKind::Nop, "#VIEW_UNWITNESSED");
    // The presence ops are emitted before the branch that reads them.
    const CellRef witnessed = emitCellPresence(sourceEpoch);
    const auto skipCheck =
        emitOp(OpcodeKind::BranchFalseCell, "#VIEW_WITNESS_TEST");
    emitInput(skipCheck, witnessed);
    emitInput(skipCheck, unwitnessed);
    const CellRef viewEpoch = emitLinkRead(sourceEpoch, dims.epoch);
    epochUnchanged(routines.resolveSource, sourceEpoch,
                   viewNumberReusedReason("source"));
    epochUnchanged(routines.resolveView, viewEpoch,
                   viewNumberReusedReason("view"));
    appendOpcode(unwitnessed);
    routines.resolveEntry = finish();
  }

  // -- wrap: a reached cell to a handle of the view whose number is given.
  routines.wrapSource = slot();
  routines.wrapNumber = slot();
  routines.wrapHandle = slot();
  {
    const CellRef sourceBox =
        box(numberOf(routines.wrapSource, slot()), slot());
    const CellRef viewBox = box(routines.wrapNumber, slot());
    // The handle's own text is a display copy taken now; a step reads the
    // live source through the box, never this copy.
    const CellRef copy = slot();
    const auto read    = emitOp(OpcodeKind::VqlString, "#VIEW_COPY");
    emitInput(read, routines.wrapSource);
    emitOutput(read, copy);
    box(copy, routines.wrapHandle);
    for (const auto &[dimension, target] :
         {std::pair{dims.marker, sourceBox}, std::pair{dims.of, viewBox}}) {
      const auto link = emitOp(OpcodeKind::Link, "#VIEW_NAME");
      emitInput(link, routines.wrapHandle);
      emitInputConstant(link, static_cast<std::int64_t>(dimension));
      emitInputConstant(link, forward);
      emitInput(link, target);
    }
    routines.wrapEntry = finish();
  }

  // -- fold: the view's rules, in source order, over one step.
  routines.foldView      = slot();
  routines.foldName      = slot();
  routines.foldDirection = slot();
  routines.foldMint      = slot();
  routines.foldAllowed   = slot();
  routines.foldPhysical  = slot();
  routines.foldComputed  = slot();
  {
    const auto text = [&](const std::string_view value) {
      return emitConstant(CellValue(std::string{value}));
    };
    const auto notOf = [&](const CellRef value) {
      const CellRef result = slot();
      const auto negate    = emitOp(OpcodeKind::Not, "#VIEW_FOLD_NOT");
      emitInput(negate, value);
      emitOutput(negate, result);
      return result;
    };
    const auto binary = [&](const OpcodeKind kind, const CellRef left,
                            const CellRef right) {
      return emitBinary(kind, left, right, "#VIEW_FOLD");
    };
    const auto copyInto = [&](const CellRef value, const CellRef target) {
      const auto add = emitOp(OpcodeKind::Add, "#VIEW_COPY_FLAG");
      emitInput(add, value);
      emitOutput(add, target);
    };
    // Resolved the way a rule's own name is, so the two compare as the same
    // kind of value whatever the VM does with a dimension number.
    const CellRef stepNumber = slot();
    const auto stepResolve   = emitOp(OpcodeKind::DimByName, "#VIEW_STEP_DIM");
    emitInput(stepResolve, routines.foldName);
    emitInputConstant(stepResolve, std::int64_t{0});
    emitOutput(stepResolve, stepNumber);
    // `allowed` starts from the view's base flag and each rule, in source
    // order, shows or hides the step; a pivot on the step's own dimension also
    // redirects it.
    copyInto(emitLinkRead(routines.foldView, dims.base), routines.foldAllowed);
    copyInto(stepNumber, routines.foldPhysical);
    copyInto(falseConst, routines.foldComputed);
    const auto rules = emitOp(OpcodeKind::RankBegin, "#VIEW_RULES");
    emitInput(rules, routines.foldView);
    emitInputConstant(rules, static_cast<std::int64_t>(dims.rule));
    emitInputConstant(rules, forward);
    const CellRef iterator = slot();
    emitOutput(rules, iterator);
    const CellRef allText      = text(viewKindAll);
    const CellRef noneText     = text(viewKindNone);
    const CellRef maskText     = text(viewKindMask);
    const CellRef pivotText    = text(viewKindPivot);
    const CellRef computedText = text(viewKindComputed);
    emitIteratorLoop(iterator, [&](const CellRef rule) {
      const CellRef fromDim = slot();
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
          binary(OpcodeKind::Eq, direction, routines.foldDirection);
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
      const CellRef hides = binary(OpcodeKind::Or, isNone,
                                   binary(OpcodeKind::And, maskHit, negated));
      const CellRef kept =
          binary(OpcodeKind::And, routines.foldAllowed, notOf(hides));
      const auto setAllowed = emitOp(OpcodeKind::Or, "#VIEW_ALLOWED");
      emitInput(setAllowed, shows);
      emitInput(setAllowed, kept);
      emitOutput(setAllowed, routines.foldAllowed);
      const auto setComputed = emitOp(OpcodeKind::Or, "#VIEW_COMPUTED");
      emitInput(setComputed, routines.foldComputed);
      emitInput(setComputed, computedHit1);
      emitOutput(setComputed, routines.foldComputed);
      const CellRef skip = mintOpcode(OpcodeKind::Nop, "#VIEW_NO_PIVOT");
      const auto notPivot =
          emitOp(OpcodeKind::BranchFalseCell, "#VIEW_PIVOT_TEST");
      emitInput(notPivot, pivotHit);
      emitInput(notPivot, skip);
      const auto redirect = emitOp(OpcodeKind::DimByName, "#VIEW_PIVOT_DIM");
      emitInput(redirect, target);
      emitInput(redirect, routines.foldMint);
      emitOutput(redirect, routines.foldPhysical);
      appendOpcode(skip);
    });
    routines.foldEntry = finish();
  }

  entryOp_                  = savedEntry;
  currentOp_                = savedCurrent;
  compilingFrameDefinition_ = savedFrame;
  viewRoutines_             = routines;
  return *viewRoutines_;
}

CellRef VQLCompiler::emitViewHandle(const CellRef source,
                                    const CellRef viewNumber,
                                    const ViewDimensions &dims) {
  const ViewRoutines &routines = viewRoutines(dims);
  // One handle per reach, never looked up and reused: handles are minted
  // inside the program and released with it, no VQL operation exposes cell
  // identity, and a lookup would be a search over the view's handles for a
  // difference nothing can observe. Two reaches of one source are two cells.
  const auto arguments = emitOp(OpcodeKind::CopyCellRefs, "#VIEW_WRAP_SOURCE");
  emitInput(arguments, source);
  emitOutput(arguments, routines.wrapSource);
  const auto number = emitOp(OpcodeKind::Add, "#VIEW_WRAP_NUMBER");
  emitInput(number, viewNumber);
  emitOutput(number, routines.wrapNumber);
  emitCallRoutine(routines.wrapEntry);
  // The slot is the routine's and is overwritten by the next wrap, so the
  // caller appends what it holds before wrapping again.
  return routines.wrapHandle;
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

  const ViewRoutines &routines = viewRoutines(dims);
  // The VM answers a Link that finds no neighbour with its absent-cell
  // reference, the one representation it has for "nothing there"; the
  // comparison is that probe and nothing else, and no number of ours is ever
  // given that meaning.
  const CellRef absentCell =
      emitConstant(CellValue(static_cast<std::int64_t>(zigzag::noCell)));
  const CellRef falseConst = emitConstant(CellValue(false));

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
    const auto copyIn = emitOp(OpcodeKind::CopyCellRefs, "#VIEW_RESOLVE_ARG");
    emitInput(copyIn, current);
    emitOutput(copyIn, routines.resolveHandle);
    emitCallRoutine(routines.resolveEntry);
    const CellRef source = emitReferenceCopy(routines.resolveSource);
    if (refusal) {
      emitRequire(falseConst, *refusal);
    } else if (!dimension) {
      // value() and count() leave the view: they answer about the source.
      emitAppendStream(out, compileSelector(step, source, hasContext));
    } else {
      const CellRef viewNumber = emitRawCopy(routines.resolveNumber);
      emitWrite(routines.foldName, CellValue(dimension->dimName));
      emitWrite(routines.foldDirection,
                CellValue(static_cast<std::int64_t>(
                    zigzag::toSign(dimension->direction))));
      emitWrite(routines.foldMint,
                CellValue(std::int64_t{dimension->creates.empty() ? 0 : 1}));
      const auto viewIn = emitOp(OpcodeKind::CopyCellRefs, "#VIEW_FOLD_VIEW");
      emitInput(viewIn, routines.resolveView);
      emitOutput(viewIn, routines.foldView);
      emitCallRoutine(routines.foldEntry);
      // Copied out of the routine's slots: a creation value or a computed rule
      // below may step a view of its own and call the fold again.
      const CellRef allowed     = emitRawCopy(routines.foldAllowed);
      const CellRef physical    = emitRawCopy(routines.foldPhysical);
      const CellRef computedHit = emitRawCopy(routines.foldComputed);

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
            emitBinary(OpcodeKind::Eq, viewNumber,
                       emitConstant(CellValue(candidate.view)), "#VIEW_FOLD");
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
