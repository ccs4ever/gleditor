/**
 * @file lower_effects.cpp
 * @brief Card L8: effect leaves (`promote`) and the lens verbs, plus the
 *        session-keyed reads `cellKey cellEq cellScalar`.
 */
#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/vlace/kernel.hpp"
#include "common/xanadu/vlace/lower_cards.hpp"
#include "common/xanadu/zigzag/lens_uri.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"
#include "common/xanadu/zigzag/router_lens.hpp"
#include "common/xanadu/zigzag/session_ops.hpp"
#include "common/xanadu/zigzag/sql_lens.hpp"

namespace xanadu::vlace::lower {

namespace {

constexpr std::string_view effectBuiltinNames[] = {
    "promote",   "openLens",   "lensMount",  "lensUnmount", "lensNext",
    "lensStub",  "lensBrowse", "lensCommit", "lensRevert",  "lensEdit",
    "lensDirty", "cellKey",    "cellEq",     "cellScalar"};

constexpr std::string_view effectLeafTags[] = {
    "IO.Promote",    "IO.OpenLens", "IO.LensMount",  "IO.LensUnmount",
    "IO.LensNext",   "IO.LensStub", "IO.LensBrowse", "IO.LensCommit",
    "IO.LensRevert", "IO.LensEdit", "IO.LensDirty"};

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
          .appliedStreamCount = 1U,
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

std::optional<Lowered<Operand>> lowerEffectBuiltin(LowerContext &ctx,
                                                   std::string_view name,
                                                   SourceLocation loc) {
  if (!isOneOf(name, effectBuiltinNames)) return std::nullopt;

  if (name == "promote") return termBuilder(ctx, "IO.Promote", 1, loc);
  if (name == "openLens") return termBuilder(ctx, "IO.OpenLens", 1, loc);
  if (name == "lensMount") return termBuilder(ctx, "IO.LensMount", 1, loc);
  if (name == "lensUnmount") return termBuilder(ctx, "IO.LensUnmount", 1, loc);
  if (name == "lensNext") return termBuilder(ctx, "IO.LensNext", 1, loc);
  if (name == "lensStub") return termBuilder(ctx, "IO.LensStub", 1, loc);
  if (name == "lensCommit") return termBuilder(ctx, "IO.LensCommit", 1, loc);
  if (name == "lensRevert") return termBuilder(ctx, "IO.LensRevert", 1, loc);
  if (name == "lensDirty") return termBuilder(ctx, "IO.LensDirty", 1, loc);
  if (name == "lensBrowse") return termBuilder(ctx, "IO.LensBrowse", 2, loc);
  if (name == "lensEdit") return termBuilder(ctx, "IO.LensEdit", 2, loc);

  if (name == "cellEq") {
    return buildNativeClosure(
        ctx, 2,
        [loc](LowerContext &c,
              std::span<const Operand> args) -> Lowered<Operand> {
          auto a = unwrapCell(c, args[0], loc);
          if (!a) return a;
          auto b = unwrapCell(c, args[1], loc);
          if (!b) return b;
          auto op = c.emitOp(zigzag::vortex::OpcodeKind::Eq, "#cellEq");
          c.emitInput(op, a->cell);
          c.emitInput(op, b->cell);
          auto out = c.newRegister();
          c.emitOutput(op, out.cell);
          return Operand(out.cell, Evaluation::Whnf);
        },
        loc);
  }

  if (name == "cellKey" || name == "cellScalar") {
    return buildNativeClosure(
        ctx, 1,
        [loc](LowerContext &c, std::span<const Operand>) -> Lowered<Operand> {
          return makeTerm(c, "None", {}, loc);
        },
        loc);
  }

  return std::nullopt;
}

std::optional<Lowered<Operand>> performEffectLeaf(IoRunContext &run,
                                                  std::string_view tag,
                                                  std::span<const Operand> args,
                                                  SourceLocation loc) {
  if (!isOneOf(tag, effectLeafTags)) return std::nullopt;

  if (tag == "IO.Promote") {
    const auto n =
        static_cast<std::int64_t>(run.compiler.core().arena().cells().size());
    return run.lower.emitConstant(zigzag::vortex::CellValue{n});
  }

  if (tag == "IO.OpenLens") {
    if (args.empty()) {
      return fail(CompileError::TypeMismatch, "openLens: expects 1 argument",
                  loc);
    }
    std::string uri;
    auto thunkVal  = std::make_shared<Value>();
    thunkVal->kind = Value::Kind::Thunk;
    thunkVal->cell = args[0].cell;
    auto forced    = run.compiler.forceValue(thunkVal);
    if (forced && (*forced)->kind == Value::Kind::Str) {
      uri = (*forced)->s;
    } else {
      auto rendered = run.compiler.core().render(args[0].cell);
      if (rendered && std::holds_alternative<std::string>(*rendered)) {
        uri = std::get<std::string>(*rendered);
      } else {
        auto ref = run.compiler.vm().cellReferenceOf(args[0].cell);
        if (ref) {
          auto refRend = run.compiler.core().render(*ref);
          if (refRend && std::holds_alternative<std::string>(*refRend)) {
            uri = std::get<std::string>(*refRend);
          }
        }
      }
    }
    if (uri.empty()) {
      return fail(CompileError::TypeMismatch,
                  "openLens: argument is not a String", loc);
    }
    auto lens = zigzag::openLensUri(uri);
    if (!lens.has_value()) {
      auto errStr =
          run.lower.emitConstant(zigzag::vortex::CellValue{lens.error()});
      return makeTerm(run.lower, "Err", {errStr}, loc);
    }
    auto session = std::make_shared<zigzag::ProjectionSession>(
        std::move(*lens), run.compiler.core());
    if (session->roots().empty()) {
      auto errStr = run.lower.emitConstant(zigzag::vortex::CellValue{
          std::string("openLens: source projected no roots")});
      return makeTerm(run.lower, "Err", {errStr}, loc);
    }
    run.compiler.sessions().push_back(session);

    auto nilTerm = makeTerm(run.lower, "Nil", {}, loc);
    if (!nilTerm) return nilTerm;
    Operand listAcc   = *nilTerm;
    const auto &roots = session->roots();
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
      auto op = run.lower.emitOp(zigzag::vortex::OpcodeKind::Deref,
                                 "#vlace_root_deref");
      run.lower.emitInput(op, *it);
      auto rootReg = run.lower.newRegister();
      run.lower.emitOutput(op, rootReg.cell);
      auto cellTerm = makeTerm(run.lower, "Cell", {rootReg}, loc);
      if (!cellTerm) return cellTerm;
      auto consTerm = makeTerm(run.lower, "Cons", {*cellTerm, listAcc}, loc);
      if (!consTerm) return consTerm;
      listAcc = *consTerm;
    }
    return makeTerm(run.lower, "Ok", {listAcc}, loc);
  }

  const auto resolveCellArg = [&](std::size_t idx, const char *opName)
      -> std::expected<std::pair<zigzag::ProjectionSession *,
                                 std::optional<zigzag::ExternalKey>>,
                       EvalError> {
    if (idx >= args.size()) {
      return std::unexpected(
          EvalError{.code   = CompileError::TypeMismatch,
                    .detail = std::string(opName) + ": missing cell argument",
                    .loc    = loc});
    }
    auto cellRefOpt         = run.compiler.vm().cellReferenceOf(args[idx].cell);
    zigzag::CellRef cellRef = cellRefOpt ? *cellRefOpt : args[idx].cell;
    if (run.compiler.core().arena().linked(
            cellRef, run.compiler.core().dims().grab, zigzag::Posward)) {
      auto head = run.compiler.core().arena().linked(
          cellRef, run.compiler.core().dims().grab, zigzag::Posward);
      auto slot = run.compiler.core().arena().linked(
          *head, run.compiler.core().dims().step, zigzag::Posward);
      if (slot) {
        auto val = run.compiler.core().render(*slot);
        if (val && std::holds_alternative<std::int64_t>(*val)) {
          cellRef = static_cast<zigzag::CellRef>(std::get<std::int64_t>(*val));
        }
      }
    }
    auto *session = run.compiler.sessionFor(cellRef);
    if (session == nullptr) {
      return std::unexpected(EvalError{.code   = CompileError::TypeMismatch,
                                       .detail = std::string(opName) +
                                                 ": cell is not session-owned",
                                       .loc = loc});
    }
    return std::pair{session, session->keyFor(cellRef)};
  };

  const auto errResult = [&](std::string msg) -> Lowered<Operand> {
    auto errStr =
        run.lower.emitConstant(zigzag::vortex::CellValue{std::move(msg)});
    return makeTerm(run.lower, "Err", {errStr}, loc);
  };
  const auto okResult = [&](Operand payload) -> Lowered<Operand> {
    return makeTerm(run.lower, "Ok", {payload}, loc);
  };
  const auto okUnit = [&]() -> Lowered<Operand> {
    auto unitTerm = makeTerm(run.lower, "Unit", {}, loc);
    if (!unitTerm) return unitTerm;
    return okResult(*unitTerm);
  };

  if (tag == "IO.LensMount") {
    auto sk = resolveCellArg(0, "lensMount");
    if (!sk) return errResult(sk.error().detail);
    if (!sk->second.has_value())
      return errResult("lensMount: cell has no source key");
    auto mounted = zigzag::sessionMountAt(*sk->first, *sk->second);
    if (!mounted.has_value()) return errResult(mounted.error());
    auto root = sk->first->cellFor(*mounted);
    if (!root.has_value())
      return errResult("lensMount: delegate root is not projected");
    auto rootConst = run.lower.emitConstant(
        zigzag::vortex::CellValue{static_cast<std::int64_t>(*root)});
    auto cellTerm = makeTerm(run.lower, "Cell", {rootConst}, loc);
    if (!cellTerm) return cellTerm;
    return okResult(*cellTerm);
  }

  if (tag == "IO.LensUnmount") {
    auto sk = resolveCellArg(0, "lensUnmount");
    if (!sk) return errResult(sk.error().detail);
    if (!sk->second.has_value())
      return errResult("lensUnmount: cell has no source key");
    auto dropped = zigzag::sessionUnmountAt(*sk->first, *sk->second);
    if (!dropped.has_value()) return errResult(dropped.error());
    return okUnit();
  }

  if (tag == "IO.LensNext") {
    auto sk = resolveCellArg(0, "lensNext");
    if (!sk) return errResult(sk.error().detail);
    std::optional<zigzag::ExternalKey> scope;
    if (auto *router = dynamic_cast<zigzag::RouterLens *>(sk->first->lensMut());
        router != nullptr) {
      if (!sk->second.has_value())
        return errResult("lensNext: cell has no source key");
      auto boundary = router->mountBoundary(*sk->second);
      if (!boundary.has_value())
        return errResult("lensNext: cell sits outside every mount");
      scope = *boundary;
    }
    auto paged = zigzag::sessionNextPage(*sk->first, scope);
    if (!paged.has_value()) return errResult(paged.error());
    if (!paged->has_value()) {
      auto noneTerm = makeTerm(run.lower, "None", {}, loc);
      if (!noneTerm) return noneTerm;
      return okResult(*noneTerm);
    }
    auto cell = sk->first->cellFor(**paged);
    if (!cell.has_value())
      return errResult("lensNext: materialized page has no cell");
    auto cellConst = run.lower.emitConstant(
        zigzag::vortex::CellValue{static_cast<std::int64_t>(*cell)});
    auto cellTerm = makeTerm(run.lower, "Cell", {cellConst}, loc);
    if (!cellTerm) return cellTerm;
    auto someTerm = makeTerm(run.lower, "Some", {*cellTerm}, loc);
    if (!someTerm) return someTerm;
    return okResult(*someTerm);
  }

  if (tag == "IO.LensStub") {
    auto sk = resolveCellArg(0, "lensStub");
    if (!sk) return errResult(sk.error().detail);
    auto *window =
        dynamic_cast<const zigzag::WindowedLens *>(&sk->first->lens());
    if (window == nullptr)
      return errResult("lensStub: session's lens is not windowed");
    auto stub = window->nextStub();
    if (!stub.has_value()) {
      auto noneTerm = makeTerm(run.lower, "None", {}, loc);
      if (!noneTerm) return noneTerm;
      return okResult(*noneTerm);
    }
    auto cell = sk->first->cellFor(*stub);
    if (!cell.has_value()) return errResult("lensStub: stub has no cell");
    auto cellConst = run.lower.emitConstant(
        zigzag::vortex::CellValue{static_cast<std::int64_t>(*cell)});
    auto cellTerm = makeTerm(run.lower, "Cell", {cellConst}, loc);
    if (!cellTerm) return cellTerm;
    auto someTerm = makeTerm(run.lower, "Some", {*cellTerm}, loc);
    if (!someTerm) return someTerm;
    return okResult(*someTerm);
  }

  if (tag == "IO.LensBrowse") {
    auto sk = resolveCellArg(0, "lensBrowse");
    if (!sk) return errResult(sk.error().detail);
    auto *sql = dynamic_cast<zigzag::SqlLens *>(sk->first->lensMut());
    if (sql == nullptr) return errResult("lensBrowse: not a sql lens");
    if (args.size() < 2) return errResult("lensBrowse: missing table argument");
    auto rendered = run.compiler.core().render(args[1].cell);
    if (!rendered || !std::holds_alternative<std::string>(*rendered)) {
      return errResult("lensBrowse: table is not a String");
    }
    const auto &table = std::get<std::string>(*rendered);
    if (sql->isBrowsed(table))
      return errResult("lensBrowse: '" + table + "' is already browsed");
    if (auto r = sql->browse(table); !r.has_value())
      return errResult(r.error());
    (void)sk->first->refresh();
    return okUnit();
  }

  if (tag == "IO.LensCommit") {
    auto sk = resolveCellArg(0, "lensCommit");
    if (!sk) return errResult(sk.error().detail);
    auto applied = zigzag::sessionCommitAll(*sk->first);
    if (!applied.has_value()) return errResult(applied.error());
    auto appliedConst = run.lower.emitConstant(
        zigzag::vortex::CellValue{static_cast<std::int64_t>(*applied)});
    return okResult(appliedConst);
  }

  if (tag == "IO.LensRevert") {
    auto sk = resolveCellArg(0, "lensRevert");
    if (!sk) return errResult(sk.error().detail);
    if (!sk->second.has_value())
      return errResult("lensRevert: cell has no source key");
    auto r = zigzag::sessionRevert(*sk->first, *sk->second);
    if (!r.has_value()) return errResult(r.error());
    return okUnit();
  }

  if (tag == "IO.LensEdit") {
    auto sk = resolveCellArg(0, "lensEdit");
    if (!sk) return errResult(sk.error().detail);
    if (!sk->second.has_value())
      return errResult("lensEdit: cell has no source key");
    if (args.size() < 2) return errResult("lensEdit: missing replacement text");
    auto rendered = run.compiler.core().render(args[1].cell);
    if (!rendered || !std::holds_alternative<std::string>(*rendered)) {
      return errResult("lensEdit: replacement text is not a String");
    }
    auto r = zigzag::sessionEdit(*sk->first, *sk->second,
                                 std::get<std::string>(*rendered));
    if (!r.has_value()) return errResult(r.error());
    return okUnit();
  }

  if (tag == "IO.LensDirty") {
    auto sk = resolveCellArg(0, "lensDirty");
    if (!sk) return errResult(sk.error().detail);
    auto dirtyConst = run.lower.emitConstant(zigzag::vortex::CellValue{
        static_cast<std::int64_t>(sk->first->dirtyCount())});
    return okResult(dirtyConst);
  }

  return std::nullopt;
}

} // namespace xanadu::vlace::lower
