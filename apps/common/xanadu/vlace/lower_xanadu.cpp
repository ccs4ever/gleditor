/**
 * @file lower_xanadu.cpp
 * @brief Card L7: the pure `std:xanadu` arena verbs.
 */
#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vlace/kernel.hpp"
#include "common/xanadu/vlace/lower_cards.hpp"
#include "common/xanadu/vlace/parser.hpp"

namespace xanadu::vlace::lower {

namespace {

constexpr std::string_view xanaduBuiltinNames[] = {
    "makeText", "makeDim",     "dimensionNamed", "setLink",        "cellText",
    "linked",   "childrenAll", "words",          "__vlace_char_at"};

class BlockSwap {
public:
  explicit BlockSwap(LowerContext &ctx)
      : ctx_(ctx), saved_(ctx.swapBlock(Block{})) {}
  BlockSwap(const BlockSwap &)            = delete;
  BlockSwap &operator=(const BlockSwap &) = delete;
  ~BlockSwap() { ctx_.swapBlock(std::move(saved_)); }
  Block take() { return ctx_.swapBlock(Block{}); }

private:
  LowerContext &ctx_;
  Block saved_;
};

template <typename T = Operand>
Lowered<T> internalError(std::string message, SourceLocation loc = {}) {
  return fail(CompileError::UnexpectedToken, "vlace: " + std::move(message),
              loc);
}

inline Lowered<Operand> makeTerm(LowerContext &ctx, std::string_view tag,
                                 std::initializer_list<Operand> fields,
                                 SourceLocation loc = {}) {
  std::vector<Operand> v(fields);
  return makeTerm(ctx, tag, std::span<const Operand>{v}, loc);
}

zigzag::CellRef emit(LowerContext &ctx, zigzag::vortex::OpcodeKind kind,
                     std::string_view label,
                     std::initializer_list<zigzag::CellRef> inputs,
                     std::initializer_list<zigzag::CellRef> outputs = {}) {
  const auto op = ctx.emitOp(kind, label);
  for (const auto cell : inputs) ctx.emitInput(op, cell);
  for (const auto cell : outputs) ctx.emitOutput(op, cell);
  return op;
}

/// Fails the run, naming the verb, unless the (already forced) name operand is
/// non-empty text. The VM refuses an empty name too, but with its own message
/// (about DimByName) and no VLace code.
void requireDimensionName(LowerContext &c, const Operand &name,
                          std::string_view verb, SourceLocation loc) {
  const auto empty = c.newRegister();
  emit(c, zigzag::vortex::OpcodeKind::Eq, "#dim_name_empty",
       {name.cell,
        c.emitConstant(zigzag::vortex::CellValue{std::string{}}).cell},
       {empty.cell});
  const auto ok = c.mintLabel("#dim_name_ok");
  c.branchIfFalse(empty, ok);
  c.emitRuntimeFailure(CompileError::InvalidArgument, loc,
                       std::string(verb) +
                           ": dimension name must not be empty");
  c.placeLabel(ok);
}

Operand emitLinkRead(LowerContext &ctx, zigzag::CellRef subject,
                     zigzag::DimRef dim) {
  const auto out = ctx.newRegister();
  emit(ctx, zigzag::vortex::OpcodeKind::Link, "#vlace_link_read",
       {subject,
        ctx.emitConstant(
               zigzag::vortex::CellValue{static_cast<std::int64_t>(dim)})
            .cell,
        ctx.emitConstant(zigzag::vortex::CellValue{std::int64_t{1}}).cell},
       {out.cell});
  return out;
}

Lowered<Operand> unwrapCell(LowerContext &ctx, Operand wrapped,
                            SourceLocation loc) {
  auto forced = forceOperand(ctx, wrapped, loc);
  if (!forced) return forced;
  const auto header = emitLinkRead(ctx, forced->cell, ctx.core().dims().grab);
  const auto slot   = emitLinkRead(ctx, header.cell, ctx.core().dims().step);
  auto slotForced   = forceOperand(ctx, slot, loc);
  if (!slotForced) return slotForced;
  auto op = ctx.emitOp(zigzag::vortex::OpcodeKind::Value, "#vlace_unwrap_cell");
  ctx.emitInput(op, slotForced->cell);
  auto out = ctx.newRegister();
  ctx.emitOutput(op, out.cell);
  return Operand(out.cell, Evaluation::Whnf);
}

using NativeBody =
    std::function<Lowered<Operand>(LowerContext &, std::span<const Operand>)>;

Lowered<zigzag::CellRef> buildFrameLevel(LowerContext &ctx, std::size_t arity,
                                         std::size_t currentArgIndex,
                                         const NativeBody &body,
                                         SourceLocation loc) {
  const auto reserved = ctx.vm().reserveFrameDescriptor();
  if (!reserved)
    return internalError<zigzag::CellRef>(
        "frame descriptor: " + reserved.error().message, loc);

  BlockSwap swap(ctx);
  std::vector<zigzag::vortex::FramePort> ports;
  std::vector<zigzag::vortex::ClosureFrameInput> inputs;

  std::vector<Operand> innerArgs;
  innerArgs.reserve(currentArgIndex + 1);
  if (currentArgIndex > 0) {
    const auto portCap = ctx.newRegister();
    const auto iterCap = ctx.newRegister();
    emit(ctx, zigzag::vortex::OpcodeKind::StreamBegin, "#vlace_unpack_caps",
         {portCap.cell}, {iterCap.cell});
    for (std::size_t i = 0; i < currentArgIndex; ++i) {
      const auto capReg = ctx.newRegister();
      const auto pres   = ctx.newRegister();
      emit(ctx, zigzag::vortex::OpcodeKind::IteratorNext, "#vlace_unpack_cap",
           {iterCap.cell}, {capReg.cell, pres.cell});
      innerArgs.push_back(capReg);
    }
    ports.emplace_back(portCap.cell, zigzag::vortex::FramePortKind::Stream);
    inputs.push_back(
        {.source = zigzag::vortex::ClosureInputSource::Capture, .index = 0});
  }

  const auto portArg = ctx.newRegister();
  const auto iterArg = ctx.newRegister();
  emit(ctx, zigzag::vortex::OpcodeKind::StreamBegin, "#vlace_unpack_arg",
       {portArg.cell}, {iterArg.cell});
  const auto argReg  = ctx.newRegister();
  const auto presArg = ctx.newRegister();
  emit(ctx, zigzag::vortex::OpcodeKind::IteratorNext, "#vlace_unpack_arg_item",
       {iterArg.cell}, {argReg.cell, presArg.cell});
  innerArgs.push_back(argReg);
  ports.emplace_back(portArg.cell, zigzag::vortex::FramePortKind::Stream);
  inputs.push_back(
      {.source = zigzag::vortex::ClosureInputSource::Applied, .index = 0U});

  const auto returned = ctx.newRegister();
  if (currentArgIndex + 1 == arity) {
    auto result = body(ctx, innerArgs);
    if (!result) return std::unexpected(result.error());
    emitCopyReference(ctx, *result, returned);
  } else {
    auto nextDesc = buildFrameLevel(ctx, arity, currentArgIndex + 1, body, loc);
    if (!nextDesc) return std::unexpected(nextDesc.error());

    const auto capStream = ctx.newRegister();
    const auto append    = ctx.emitOp(zigzag::vortex::OpcodeKind::StreamAppend,
                                      "#vlace_cap_stream");
    for (const auto &arg : innerArgs) ctx.emitInput(append, arg.cell);
    ctx.emitOutput(append, capStream.cell);

    const auto closureHandle = ctx.newRegister();
    const auto create =
        ctx.emitOp(zigzag::vortex::OpcodeKind::CreateClosure, "#vlace_closure");
    ctx.emitInput(create, *nextDesc);
    ctx.emitInput(create, capStream.cell);
    ctx.emitOutput(create, closureHandle.cell);

    emitCopyReference(ctx, closureHandle, returned);
  }

  emit(ctx, zigzag::vortex::OpcodeKind::ReturnFrame, "#vlace_return",
       {*reserved});
  const Block bodyBlock = swap.take();
  if (!bodyBlock.entry)
    return internalError<zigzag::CellRef>("frame emitted nothing", loc);

  std::vector<zigzag::CellRef> distinctLocals;
  distinctLocals.reserve(bodyBlock.locals.size());
  for (const auto c : bodyBlock.locals) {
    if (std::ranges::find(distinctLocals, c) == distinctLocals.end())
      distinctLocals.push_back(c);
  }

  const auto done = ctx.vm().finalizeFrameDescriptor(
      *reserved, zigzag::vortex::FrameDescriptorSpec{
                     .entry         = bodyBlock.entry,
                     .locals        = std::move(distinctLocals),
                     .argumentPorts = std::move(ports),
                     .resultPorts   = {zigzag::vortex::FramePort{
                         returned.cell, zigzag::vortex::FramePortKind::Cell}}});
  if (!done)
    return internalError<zigzag::CellRef>(
        "finalizeFrame: " + done.error().message, loc);

  const auto closure =
      ctx.vm().makeClosureDescriptor(zigzag::vortex::ClosureDescriptorSpec{
          .frameDescriptor    = *reserved,
          .captureStreamCount = currentArgIndex > 0 ? 1U : 0U,
          .maxFormalCount     = 1U,
          .frameInputs        = std::move(inputs)});
  if (!closure)
    return internalError<zigzag::CellRef>(
        "makeClosure: " + closure.error().message, loc);
  return *closure;
}

Lowered<Operand> buildNativeClosure(LowerContext &ctx, std::size_t arity,
                                    const NativeBody &body,
                                    SourceLocation loc) {
  auto desc = buildFrameLevel(ctx, arity, 0, body, loc);
  if (!desc) return std::unexpected(desc.error());

  const auto handle = ctx.newRegister();
  const auto create = ctx.emitOp(zigzag::vortex::OpcodeKind::CreateClosure,
                                 "#vlace_native_closure");
  ctx.emitInput(create, *desc);
  ctx.emitOutput(create, handle.cell);
  return Operand(handle.cell, Evaluation::Whnf);
}

} // namespace

std::optional<Lowered<Operand>> lowerXanaduBuiltin(LowerContext &ctx,
                                                   std::string_view name,
                                                   SourceLocation loc) {
  if (!isOneOf(name, xanaduBuiltinNames)) return std::nullopt;

  if (name == "makeText") {
    return buildNativeClosure(
        ctx, 1,
        [loc](LowerContext &c,
              std::span<const Operand> args) -> Lowered<Operand> {
          auto sForced = forceOperand(c, args[0], loc);
          if (!sForced) return sForced;
          auto op = c.emitOp(zigzag::vortex::OpcodeKind::MakeTerm, "#makeText");
          c.emitInput(op, sForced->cell);
          auto cellRefReg = c.newRegister();
          c.emitOutput(op, cellRefReg.cell);
          return makeTerm(c, "Cell", {cellRefReg}, loc);
        },
        loc);
  }

  if (name == "makeDim") {
    return buildNativeClosure(
        ctx, 1,
        [loc](LowerContext &c,
              std::span<const Operand> args) -> Lowered<Operand> {
          auto nameForced = forceOperand(c, args[0], loc);
          if (!nameForced) return nameForced;
          requireDimensionName(c, *nameForced, "makeDim", loc);
          auto op = c.emitOp(zigzag::vortex::OpcodeKind::DimByName, "#makeDim");
          c.emitInput(op, nameForced->cell);
          c.emitInput(op, c.emitConstant(zigzag::vortex::CellValue{true}).cell);
          auto dimRefReg = c.newRegister();
          c.emitOutput(op, dimRefReg.cell);
          return makeTerm(c, "Dim", {dimRefReg}, loc);
        },
        loc);
  }

  if (name == "dimensionNamed") {
    return buildNativeClosure(
        ctx, 1,
        [loc](LowerContext &c,
              std::span<const Operand> args) -> Lowered<Operand> {
          auto nameForced = forceOperand(c, args[0], loc);
          if (!nameForced) return nameForced;
          requireDimensionName(c, *nameForced, "dimensionNamed", loc);
          auto op = c.emitOp(zigzag::vortex::OpcodeKind::DimByName,
                             "#dimensionNamed");
          c.emitInput(op, nameForced->cell);
          c.emitInput(op,
                      c.emitConstant(zigzag::vortex::CellValue{false}).cell);
          auto dimRefReg = c.newRegister();
          c.emitOutput(op, dimRefReg.cell);

          // The VM reports an absent dimension as cell 0, which is never a
          // cell (op index 0 is state zero), so 0 cannot be a real answer.
          const auto isPresent = c.newRegister();
          emit(
              c, zigzag::vortex::OpcodeKind::Neq, "#dim_check",
              {dimRefReg.cell,
               c.emitConstant(zigzag::vortex::CellValue{std::int64_t{0}}).cell},
              {isPresent.cell});
          const auto elseArm = c.mintLabel("#dim_none");
          const auto done    = c.mintLabel("#dim_done");
          c.branchIfFalse(isPresent, elseArm);

          auto dimTerm = makeTerm(c, "Dim", {dimRefReg}, loc);
          if (!dimTerm) return dimTerm;
          auto someTerm = makeTerm(c, "Some", {*dimTerm}, loc);
          if (!someTerm) return someTerm;
          const auto result = c.newRegister();
          emitCopyReference(c, *someTerm, result);
          c.jumpTo(done);

          c.placeLabel(elseArm);
          auto noneTerm = makeTerm(c, "None", {}, loc);
          if (!noneTerm) return noneTerm;
          emitCopyReference(c, *noneTerm, result);

          c.placeLabel(done);
          return Operand(result.cell, Evaluation::Whnf);
        },
        loc);
  }

  if (name == "cellText") {
    return buildNativeClosure(
        ctx, 1,
        [loc](LowerContext &c,
              std::span<const Operand> args) -> Lowered<Operand> {
          auto inner = unwrapCell(c, args[0], loc);
          if (!inner) return inner;
          auto op =
              c.emitOp(zigzag::vortex::OpcodeKind::VqlString, "#cellText");
          c.emitInput(op, inner->cell);
          auto out = c.newRegister();
          c.emitOutput(op, out.cell);
          return Operand(out.cell, Evaluation::Whnf);
        },
        loc);
  }

  if (name == "setLink") {
    return buildNativeClosure(
        ctx, 3,
        [loc](LowerContext &c,
              std::span<const Operand> args) -> Lowered<Operand> {
          auto fromInner = unwrapCell(c, args[0], loc);
          if (!fromInner) return fromInner;
          auto dimInner = unwrapCell(c, args[1], loc);
          if (!dimInner) return dimInner;
          auto toInner = unwrapCell(c, args[2], loc);
          if (!toInner) return toInner;

          auto op = c.emitOp(zigzag::vortex::OpcodeKind::Link, "#setLink");
          c.emitInput(op, fromInner->cell);
          c.emitInput(op, dimInner->cell);
          c.emitInput(
              op,
              c.emitConstant(zigzag::vortex::CellValue{std::int64_t{1}}).cell);
          c.emitInput(op, toInner->cell);
          auto linkOut = c.newRegister();
          c.emitOutput(op, linkOut.cell);

          return makeTerm(c, "Unit", {}, loc);
        },
        loc);
  }

  if (name == "linked") {
    return buildNativeClosure(
        ctx, 2,
        [loc](LowerContext &c,
              std::span<const Operand> args) -> Lowered<Operand> {
          auto fromInner = unwrapCell(c, args[0], loc);
          if (!fromInner) return fromInner;
          auto dimInner = unwrapCell(c, args[1], loc);
          if (!dimInner) return dimInner;

          auto op = c.emitOp(zigzag::vortex::OpcodeKind::Link, "#linked");
          c.emitInput(op, fromInner->cell);
          c.emitInput(op, dimInner->cell);
          c.emitInput(
              op,
              c.emitConstant(zigzag::vortex::CellValue{std::int64_t{1}}).cell);
          auto neighbor = c.newRegister();
          c.emitOutput(op, neighbor.cell);

          // A rank with no neighbour reads as cell 0, which is never a cell
          // (op index 0 is state zero; arena refs carry ephemeralBit), so the
          // comparison cannot mistake a real neighbour for absence.
          const auto isPresent = c.newRegister();
          emit(
              c, zigzag::vortex::OpcodeKind::Neq, "#linked_check",
              {neighbor.cell,
               c.emitConstant(zigzag::vortex::CellValue{std::int64_t{0}}).cell},
              {isPresent.cell});
          const auto elseArm = c.mintLabel("#linked_none");
          const auto done    = c.mintLabel("#linked_done");
          c.branchIfFalse(isPresent, elseArm);

          auto cellTerm = makeTerm(c, "Cell", {neighbor}, loc);
          if (!cellTerm) return cellTerm;
          auto someTerm = makeTerm(c, "Some", {*cellTerm}, loc);
          if (!someTerm) return someTerm;
          const auto result = c.newRegister();
          emitCopyReference(c, *someTerm, result);
          c.jumpTo(done);

          c.placeLabel(elseArm);
          auto noneTerm = makeTerm(c, "None", {}, loc);
          if (!noneTerm) return noneTerm;
          emitCopyReference(c, *noneTerm, result);

          c.placeLabel(done);
          return Operand(result.cell, Evaluation::Whnf);
        },
        loc);
  }

  if (name == "__vlace_char_at") {
    return buildNativeClosure(
        ctx, 2,
        [loc](LowerContext &c,
              std::span<const Operand> args) -> Lowered<Operand> {
          auto sForced = forceOperand(c, args[0], loc);
          if (!sForced) return sForced;
          auto offForced = forceOperand(c, args[1], loc);
          if (!offForced) return offForced;

          auto op =
              c.emitOp(zigzag::vortex::OpcodeKind::Value, "#vlace_char_at");
          c.emitInput(op, sForced->cell);
          c.emitInput(op, offForced->cell);
          c.emitInput(
              op,
              c.emitConstant(zigzag::vortex::CellValue{std::int64_t{1}}).cell);
          auto out = c.newRegister();
          c.emitOutput(op, out.cell);
          return Operand(out.cell, Evaluation::Whnf);
        },
        loc);
  }

  if (name == "childrenAll") {
    static constexpr const char *kChildrenAllSrc = R"VLACE(
fn parent =>
  case dimensionNamed "d.contains" of
    None => [] ;
    Some dContains =>
      (case dimensionNamed "d.seq" of
         None => (case linked parent dContains of
                    None => [] ;
                    Some first => [first]) ;
         Some dSeq =>
           letrec walk = fn cur =>
             case cur of
               None => [] ;
               Some c => c : walk (linked c dSeq)
           in walk (linked parent dContains))
)VLACE";
    Parser p(kChildrenAllSrc);
    auto expr = p.parseExpressionOnly();
    if (!expr)
      return fail(CompileError::UnexpectedToken, "childrenAll parse", loc);
    return lowerExpr(ctx, **expr);
  }

  if (name == "words") {
    static constexpr const char *kWordsSrc = R"VLACE(
fn s =>
  let isWs = fn ch => ch == " " || ch == "\t" || ch == "\n" || ch == "\r" in
  letrec loop = fn off => fn cur =>
    let ch = __vlace_char_at s off in
    if ch == "" then
      (if cur == "" then [] else [cur])
    else if isWs ch then
      (if cur == "" then loop (off + 1) ""
       else cur : loop (off + 1) "")
    else
      loop (off + 1) (cur ++ ch)
  in loop 0 ""
)VLACE";
    Parser p(kWordsSrc);
    auto expr = p.parseExpressionOnly();
    if (!expr) return fail(CompileError::UnexpectedToken, "words parse", loc);
    return lowerExpr(ctx, **expr);
  }

  return std::nullopt;
}

} // namespace xanadu::vlace::lower
