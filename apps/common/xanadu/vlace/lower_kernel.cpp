/**
 * @file lower_kernel.cpp
 * @brief Card L2: the thunk / force / apply kernel, `let`, lambda, application,
 *        letrec and top-level declarations, lowered to Vortex graphs.
 *
 * Representation (every cell below is an arena cell; nothing is host state):
 *  - A *function* is a closure value (CreateClosure) over a frame. Closures are
 *    curried and unary, so `fn x y => b` is a closure whose frame returns a
 *    second closure, and a partial application is just the inner closure.
 *  - A *thunk* is a fresh cell T whose scalar content is its state
 *    (suspended / running / done) and whose `d.thunk` edge names the closure to
 *    run. Forcing runs the closure once, then repoints `d.thunk` at a one-
 *    capture "box" closure that returns the result. The result is never linked
 *    to directly: a link evicts whatever the far end held, so two thunks that
 *    memoise the same value cell would steal it from each other. A box closure
 *    is the one persistent place a cell identity can be stored without an edge.
 *  - A top-level declaration is a global thunk cell, lowered on first use.
 */
#include <algorithm>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "common/xanadu/vlace/kernel.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vlace::lower {

namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::CellValue;
using zigzag::vortex::ClosureDescriptorSpec;
using zigzag::vortex::ClosureFrameInput;
using zigzag::vortex::ClosureInputSource;
using zigzag::vortex::FrameDescriptorSpec;
using zigzag::vortex::FramePort;
using zigzag::vortex::FramePortKind;
using zigzag::vortex::OpcodeKind;

/// A thunk cell's scalar content. Zero first so a freshly boxed cell, whose
/// content is the constant 0, is a suspended thunk without a write.
constexpr std::int64_t thunkSuspended = 0;
constexpr std::int64_t thunkRunning   = 1;
constexpr std::int64_t thunkDone      = 2;

/// Link direction operand for the Link opcode: +1 reads/writes posward.
constexpr std::int64_t linkPosward = 1;

/// The VM refuses to create more than 65,536 closure values (and a million
/// environment cells) in total, a figure sized for a handful of callbacks. A
/// lazy program makes two closures per thunk, so an ordinary loop would hit
/// it. The cycle and frame-depth budgets are what bound a VLace run; these
/// ceilings only keep the VM's own safety limits from firing first.
constexpr std::size_t closureValueCeiling       = 1U << 24;
constexpr std::size_t closureEnvironmentCeiling = 1U << 28;

/// Name prefix for a capture the frame records; the name is never looked up
/// (captureOperand receives a ScopeHit, which carries no name).
constexpr std::string_view captureNamePrefix = "#capture:";

// -- Small emission helpers
// -----------------------------------------------------

std::unexpected<EvalError> internalError(std::string detail,
                                         SourceLocation loc = {}) {
  return fail(CompileError::UnexpectedToken, "vlace: " + std::move(detail),
              loc);
}

CellRef emit(LowerContext &ctx, OpcodeKind kind, std::string_view label,
             std::initializer_list<CellRef> inputs,
             std::initializer_list<CellRef> outputs = {}) {
  const auto op = ctx.emitOp(kind, label);
  for (const auto cell : inputs) ctx.emitInput(op, cell);
  for (const auto cell : outputs) ctx.emitOutput(op, cell);
  return op;
}

CellRef intConstant(LowerContext &ctx, std::int64_t value) {
  return ctx.emitConstant(CellValue{value}).cell;
}

/// `out := Link(subject, dim, +1)`: the cell @p subject links to posward on
/// @p dim, or a null reference when it has no such edge.
Operand emitLinkRead(LowerContext &ctx, CellRef subject, zigzag::DimRef dim) {
  const auto out = ctx.newRegister();
  emit(ctx, OpcodeKind::Link, "#vlace_link_read",
       {subject, intConstant(ctx, static_cast<std::int64_t>(dim)),
        intConstant(ctx, linkPosward)},
       {out.cell});
  return out;
}

void emitLinkWrite(LowerContext &ctx, CellRef subject, zigzag::DimRef dim,
                   CellRef target) {
  emit(ctx, OpcodeKind::Link, "#vlace_link_write",
       {subject, intConstant(ctx, static_cast<std::int64_t>(dim)),
        intConstant(ctx, linkPosward), target});
}

/// Overwrite the scalar content of the cell @p subject names (the Value opcode
/// resolves a reference register to its target, an output would not).
void emitStateWrite(LowerContext &ctx, CellRef subject, std::int64_t state) {
  const auto zero = intConstant(ctx, 0);
  emit(ctx, OpcodeKind::Value, "#vlace_thunk_state",
       {subject, zero, zero, intConstant(ctx, state)});
}

std::vector<CellRef> distinct(std::vector<CellRef> cells) {
  std::unordered_set<CellRef> seen;
  std::erase_if(cells, [&seen](CellRef c) { return !seen.insert(c).second; });
  return cells;
}

/// Swap in an empty Block for the extent of a scope and swap the caller's
/// back, so a throw (the cell budget) cannot leave a half-built frame as the
/// context's current block.
class BlockSwap {
public:
  explicit BlockSwap(LowerContext &ctx)
      : ctx_(ctx), saved_(ctx.swapBlock(Block{})) {}
  BlockSwap(const BlockSwap &)            = delete;
  BlockSwap &operator=(const BlockSwap &) = delete;
  ~BlockSwap() { ctx_.swapBlock(std::move(saved_)); }
  /// The block emitted so far; the current block becomes empty again.
  Block take() { return ctx_.swapBlock(Block{}); }

private:
  LowerContext &ctx_;
  Block saved_;
};

// -- Copying a declaration out of its Program
// -----------------------------------------

/// A top-level declaration, copied out of the Program because the Program only
/// lives for one evaluation and a Compiler outlives it.
struct DeclCopy {
  std::vector<Pattern> params;
  Expr body;
  SourceLocation loc;
};

Pattern clonePattern(const PatternNode &p);
Expr cloneExpr(const ExprNode &e);

Pattern clonePattern(const PatternNode &p) {
  auto out  = std::make_unique<PatternNode>();
  out->loc  = p.loc;
  out->node = std::visit(
      [](const auto &n) -> decltype(PatternNode::node) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, PConstructor>) {
          PConstructor c{.ctor = n.ctor, .args = {}};
          for (const auto &a : n.args) c.args.push_back(clonePattern(*a));
          return c;
        } else if constexpr (std::is_same_v<T, PTuple>) {
          PTuple t;
          for (const auto &a : n.elems) t.elems.push_back(clonePattern(*a));
          return t;
        } else if constexpr (std::is_same_v<T, PRecord>) {
          PRecord r{.ctor = n.ctor, .fields = {}};
          for (const auto &[name, a] : n.fields)
            r.fields.emplace_back(name, clonePattern(*a));
          return r;
        } else {
          return n; // wildcard, variable and literal patterns copy by value
        }
      },
      p.node);
  return out;
}

Expr cloneExpr(const ExprNode &e) {
  auto out  = std::make_unique<ExprNode>();
  out->loc  = e.loc;
  out->node = std::visit(
      [](const auto &n) -> decltype(ExprNode::node) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ELet>) {
          ELet l{.recursive = n.recursive, .bindings = {}, .body = nullptr};
          for (const auto &b : n.bindings)
            l.bindings.push_back(Binding{.pattern = clonePattern(*b.pattern),
                                         .value   = cloneExpr(*b.value)});
          l.body = cloneExpr(*n.body);
          return l;
        } else if constexpr (std::is_same_v<T, EIf>) {
          return EIf{.cond = cloneExpr(*n.cond),
                     .then = cloneExpr(*n.then),
                     .elze = cloneExpr(*n.elze)};
        } else if constexpr (std::is_same_v<T, ECase>) {
          ECase c{.scrutinee = cloneExpr(*n.scrutinee), .alts = {}};
          for (const auto &a : n.alts) {
            Alt alt{.pattern = clonePattern(*a.pattern),
                    .guard   = std::nullopt,
                    .body    = cloneExpr(*a.body)};
            if (a.guard) alt.guard = cloneExpr(**a.guard);
            c.alts.push_back(std::move(alt));
          }
          return c;
        } else if constexpr (std::is_same_v<T, ELambda>) {
          ELambda l{.params = {}, .body = cloneExpr(*n.body)};
          for (const auto &p : n.params) l.params.push_back(clonePattern(*p));
          return l;
        } else if constexpr (std::is_same_v<T, EApp>) {
          return EApp{.fn = cloneExpr(*n.fn), .arg = cloneExpr(*n.arg)};
        } else if constexpr (std::is_same_v<T, EBinOp>) {
          return EBinOp{
              .op = n.op, .lhs = cloneExpr(*n.lhs), .rhs = cloneExpr(*n.rhs)};
        } else if constexpr (std::is_same_v<T, EList>) {
          EList l;
          for (const auto &x : n.elems) l.elems.push_back(cloneExpr(*x));
          return l;
        } else if constexpr (std::is_same_v<T, ETuple>) {
          ETuple t;
          for (const auto &x : n.elems) t.elems.push_back(cloneExpr(*x));
          return t;
        } else if constexpr (std::is_same_v<T, ERecordUpdate>) {
          ERecordUpdate r{.record = cloneExpr(*n.record), .fields = {}};
          for (const auto &[name, x] : n.fields)
            r.fields.emplace_back(name, cloneExpr(*x));
          return r;
        } else {
          return n; // variables, constructors and literals copy by value
        }
      },
      e.node);
  return out;
}

// -- Per-Compiler kernel state
// ----------------------------------------------------------

/// One variable a closure frame under construction captured from outside.
struct CaptureRecord {
  CellRef outer;     ///< the register in the frame `depth` frames out
  std::size_t depth; ///< Frame scopes between the use and the binding
  Evaluation state;  ///< what lowering knew about the outer register
  Operand inner;     ///< the register the frame reads it from
};

struct FrameBuild {
  std::vector<CaptureRecord> captures;
};

enum class Phase : std::uint8_t { Declared, Lowering, Ready, Failed };

struct TopLevel {
  CellRef cell; ///< the global thunk cell
  Phase phase{Phase::Declared};
  std::shared_ptr<DeclCopy> decl;
  std::optional<EvalError> failure;
  std::optional<CellRef>
      function; ///< the closure root, once a function is built
};

struct KernelState {
  std::optional<CellRef> forceFrame;
  std::optional<CellRef> boxClosure;
  bool limitsRaised{false};
  std::unordered_map<std::string, TopLevel> top;
  std::vector<FrameBuild> frames;
};

KernelState &kernel(LowerContext &ctx) {
  return ctx.state().cardState<KernelState>(Card::Kernel);
}

void ensureClosureHeadroom(LowerContext &ctx) {
  auto &k = kernel(ctx);
  if (k.limitsRaised) return;
  auto limits = ctx.vm().closureLimits();
  limits.maxValuesCreated =
      std::max(limits.maxValuesCreated, closureValueCeiling);
  limits.maxEnvironmentCellsCreated =
      std::max(limits.maxEnvironmentCellsCreated, closureEnvironmentCeiling);
  ctx.vm().setClosureLimits(limits);
  k.limitsRaised = true;
}

/// Finalise a reserved frame descriptor from a finished block.
Lowered<void> finalizeFrame(LowerContext &ctx, CellRef reserved,
                            const Block &block,
                            std::vector<FramePort> arguments,
                            FramePort result) {
  if (!block.entry) return internalError("a frame body emitted no operations");
  FrameDescriptorSpec spec{.entry         = block.entry,
                           .locals        = distinct(block.locals),
                           .argumentPorts = std::move(arguments),
                           .resultPorts   = {result}};
  const auto done = ctx.vm().finalizeFrameDescriptor(reserved, spec);
  if (!done) return internalError("frame descriptor: " + done.error().message);
  return {};
}

Lowered<CellRef> makeClosureDescriptor(LowerContext &ctx, CellRef frame,
                                       std::size_t captureStreams,
                                       std::size_t appliedStreams,
                                       std::vector<ClosureFrameInput> inputs) {
  const auto made = ctx.vm().makeClosureDescriptor(
      ClosureDescriptorSpec{.frameDescriptor    = frame,
                            .captureStreamCount = captureStreams,
                            .appliedStreamCount = appliedStreams,
                            .frameInputs        = std::move(inputs)});
  if (!made)
    return internalError("closure descriptor: " + made.error().message);
  return *made;
}

/// `dst := the first cell of a stream port`: StreamBegin + IteratorNext.
void emitUnpackFirst(LowerContext &ctx, CellRef streamPort, CellRef dst) {
  const auto iterator = ctx.newRegister();
  const auto present  = ctx.newRegister();
  emit(ctx, OpcodeKind::StreamBegin, "#vlace_unpack", {streamPort},
       {iterator.cell});
  emit(ctx, OpcodeKind::IteratorNext, "#vlace_unpack_next", {iterator.cell},
       {dst, present.cell});
}

// -- Shared frames: the identity box and force
// --------------------------------------------

/// The closure descriptor of the memo box: one captured cell, returned when
/// applied.
Lowered<CellRef> boxClosureDescriptor(LowerContext &ctx) {
  auto &k = kernel(ctx);
  if (k.boxClosure) return *k.boxClosure;
  ensureClosureHeadroom(ctx);
  const auto reserved = ctx.vm().reserveFrameDescriptor();
  if (!reserved)
    return internalError("frame descriptor: " + reserved.error().message);
  BlockSwap swap(ctx);
  const auto port = ctx.newRegister();
  const auto item = ctx.newRegister();
  const auto res  = ctx.newRegister();
  emitUnpackFirst(ctx, port.cell, item.cell);
  emitCopyReference(ctx, item, res);
  emit(ctx, OpcodeKind::ReturnFrame, "#vlace_return", {*reserved});
  const Block body = swap.take();
  if (auto done = finalizeFrame(ctx, *reserved, body,
                                {FramePort{port.cell, FramePortKind::Stream}},
                                FramePort{res.cell, FramePortKind::Cell});
      !done)
    return std::unexpected(done.error());
  auto closure = makeClosureDescriptor(
      ctx, *reserved, 1, 0,
      {{.source = ClosureInputSource::Capture, .index = 0}});
  if (closure) k.boxClosure = *closure;
  return closure;
}

/// The shared `force` frame. Argument: any cell. Result: its value in WHNF.
///   not a thunk -> the cell itself
///   running     -> run-time failure (`<<loop>>`)
///   done        -> apply the box that `d.thunk` now names
///   suspended   -> mark running, run the code closure, box the result,
///                  repoint `d.thunk` at the box, mark done
Lowered<CellRef> forceFrameDescriptor(LowerContext &ctx) {
  auto &k = kernel(ctx);
  if (k.forceFrame) return *k.forceFrame;
  const auto box = boxClosureDescriptor(ctx);
  if (!box) return std::unexpected(box.error());
  const auto reserved = ctx.vm().reserveFrameDescriptor();
  if (!reserved)
    return internalError("frame descriptor: " + reserved.error().message);
  const auto thunkDim = ctx.state().kernelDims(ctx.core()).thunk;

  BlockSwap swap(ctx);
  const auto subject = ctx.newRegister();
  const auto result  = ctx.newRegister();
  const auto zero    = intConstant(ctx, thunkSuspended);
  const auto one     = intConstant(ctx, thunkRunning);
  const auto two     = intConstant(ctx, thunkDone);
  const auto plain   = ctx.mintLabel("#vlace_force_plain");
  const auto notRun  = ctx.mintLabel("#vlace_force_not_running");
  const auto compute = ctx.mintLabel("#vlace_force_compute");
  const auto ret     = ctx.mintLabel("#vlace_force_return");

  const auto code    = emitLinkRead(ctx, subject.cell, thunkDim);
  const auto isThunk = ctx.newRegister();
  emit(ctx, OpcodeKind::Neq, "#vlace_is_thunk", {code.cell, zero},
       {isThunk.cell});
  ctx.branchIfFalse(isThunk, plain);

  const auto state = ctx.newRegister();
  emit(ctx, OpcodeKind::Add, "#vlace_thunk_read", {subject.cell}, {state.cell});
  const auto isRunning = ctx.newRegister();
  emit(ctx, OpcodeKind::Eq, "#vlace_is_running", {state.cell, one},
       {isRunning.cell});
  ctx.branchIfFalse(isRunning, notRun);
  ctx.emitRuntimeFailure(
      CompileError::BudgetExhausted, {},
      "evaluation budget exhausted: <<loop>> (a thunk demanded its own value)");
  ctx.placeLabel(notRun);

  const auto isDone = ctx.newRegister();
  emit(ctx, OpcodeKind::Eq, "#vlace_is_done", {state.cell, two}, {isDone.cell});
  ctx.branchIfFalse(isDone, compute);
  const auto remembered = ctx.newRegister();
  emit(ctx, OpcodeKind::ApplyClosure, "#vlace_memo_apply", {code.cell},
       {remembered.cell});
  emitCopyReference(ctx, remembered, result);
  ctx.jumpTo(ret);

  ctx.placeLabel(compute);
  emitStateWrite(ctx, subject.cell, thunkRunning);
  const auto value = ctx.newRegister();
  emit(ctx, OpcodeKind::ApplyClosure, "#vlace_thunk_run", {code.cell},
       {value.cell});
  const auto stream = ctx.newRegister();
  emit(ctx, OpcodeKind::StreamAppend, "#vlace_box_capture", {value.cell},
       {stream.cell});
  const auto boxed = ctx.newRegister();
  emit(ctx, OpcodeKind::CreateClosure, "#vlace_box", {*box, stream.cell},
       {boxed.cell});
  emitLinkWrite(ctx, subject.cell, thunkDim, boxed.cell);
  emitStateWrite(ctx, subject.cell, thunkDone);
  emitCopyReference(ctx, value, result);
  ctx.jumpTo(ret);

  ctx.placeLabel(plain);
  emitCopyReference(ctx, subject, result);
  ctx.placeLabel(ret);
  emit(ctx, OpcodeKind::ReturnFrame, "#vlace_return", {*reserved});

  const Block body = swap.take();
  if (auto done = finalizeFrame(ctx, *reserved, body,
                                {FramePort{subject.cell, FramePortKind::Cell}},
                                FramePort{result.cell, FramePortKind::Cell});
      !done)
    return std::unexpected(done.error());
  k.forceFrame = *reserved;
  return *reserved;
}

// -- Closure frames
// ----------------------------------------------------------------------

/// A closure whose frame is built: its descriptor and what it captures.
struct BuiltClosure {
  CellRef descriptor;
  std::vector<CaptureRecord> captures; ///< in capture-stream order
};

/// Keeps the kernel's frame stack, the lexical scope and the context's current
/// block balanced across the early returns and throws of a frame under
/// construction.
class FrameScope {
public:
  explicit FrameScope(LowerContext &ctx)
      : ctx_(ctx), swap_(ctx), scope_(ctx.pushScope(ScopeKind::Frame)) {
    kernel(ctx).frames.emplace_back();
  }
  FrameScope(const FrameScope &)            = delete;
  FrameScope &operator=(const FrameScope &) = delete;
  ~FrameScope() { kernel(ctx_).frames.pop_back(); }
  Block take() { return swap_.take(); }
  [[nodiscard]] std::vector<CaptureRecord> captures() const {
    return kernel(ctx_).frames.back().captures;
  }

private:
  LowerContext &ctx_;
  BlockSwap swap_;
  LowerContext::ScopeGuard scope_;
};

using BodyFn =
    std::function<Lowered<Operand>(LowerContext &, std::optional<Operand>)>;

Lowered<Operand> toWhnf(LowerContext &ctx, Operand v, SourceLocation loc) {
  if (v.state == Evaluation::Whnf) return v;
  return forceOperand(ctx, v, loc);
}

/// Build a closure frame. @p takesArgument adds one applied stream, whose
/// single cell is handed to @p body as its Operand; @p body returns the
/// frame's result expression, already in WHNF.
Lowered<BuiltClosure> buildClosure(LowerContext &ctx, bool takesArgument,
                                   const BodyFn &body, SourceLocation loc) {
  ensureClosureHeadroom(ctx);
  const auto reserved = ctx.vm().reserveFrameDescriptor();
  if (!reserved)
    return internalError("frame descriptor: " + reserved.error().message, loc);

  FrameScope frame(ctx);
  std::optional<Operand> item;
  if (takesArgument) item = ctx.newRegister();
  auto result = body(ctx, item);
  if (!result) return std::unexpected(result.error());
  const auto returned = ctx.newRegister();
  emitCopyReference(ctx, *result, returned);
  emit(ctx, OpcodeKind::ReturnFrame, "#vlace_return", {*reserved});

  const auto captures   = frame.captures();
  const Block bodyBlock = frame.take();

  // Unpacking the streams has to run before the body, but which variables the
  // body captures is only known once it is lowered: emit it into a block of
  // its own and chain the two.
  std::vector<FramePort> ports;
  std::vector<ClosureFrameInput> inputs;
  if (!captures.empty()) {
    const auto port     = ctx.newRegister();
    const auto iterator = ctx.newRegister();
    emit(ctx, OpcodeKind::StreamBegin, "#vlace_unpack_captures", {port.cell},
         {iterator.cell});
    for (const auto &capture : captures) {
      const auto present = ctx.newRegister();
      emit(ctx, OpcodeKind::IteratorNext, "#vlace_unpack_capture",
           {iterator.cell}, {capture.inner.cell, present.cell});
    }
    ports.emplace_back(port.cell, FramePortKind::Stream);
    inputs.push_back({.source = ClosureInputSource::Capture, .index = 0});
  }
  if (item) {
    const auto port = ctx.newRegister();
    emitUnpackFirst(ctx, port.cell, item->cell);
    ports.emplace_back(port.cell, FramePortKind::Stream);
    inputs.push_back({.source = ClosureInputSource::Applied, .index = 0});
  }
  const Block prologue = frame.take();
  Block whole          = bodyBlock;
  if (prologue.entry) {
    if (bodyBlock.entry) {
      zigzag::expectWritten(ctx.arena().link(
          *prologue.tail, ctx.core().dims().spin, false, *bodyBlock.entry));
    }
    whole.entry = prologue.entry;
    whole.locals.insert(whole.locals.end(), prologue.locals.begin(),
                        prologue.locals.end());
  }
  if (auto done = finalizeFrame(ctx, *reserved, whole, std::move(ports),
                                FramePort{returned.cell, FramePortKind::Cell});
      !done)
    return std::unexpected(done.error());
  auto descriptor =
      makeClosureDescriptor(ctx, *reserved, captures.empty() ? 0 : 1,
                            takesArgument ? 1 : 0, std::move(inputs));
  if (!descriptor) return std::unexpected(descriptor.error());
  return BuiltClosure{.descriptor = *descriptor, .captures = captures};
}

/// Emit the creation of a built closure in the current frame; the result is a
/// register naming the closure.
Lowered<Operand> emitClosureCreation(LowerContext &ctx,
                                     const BuiltClosure &built,
                                     SourceLocation loc) {
  std::vector<CellRef> sources;
  sources.reserve(built.captures.size());
  for (const auto &capture : built.captures) {
    if (capture.depth <= 1) {
      sources.push_back(capture.outer);
      continue;
    }
    // The binding lives further out than the frame that creates this closure,
    // so that frame has to capture it first.
    auto hop = captureOperand(
        ctx,
        ScopeHit{.operand    = Operand(capture.outer, capture.state),
                 .frameDepth = capture.depth - 1},
        loc);
    if (!hop) return hop;
    sources.push_back(hop->cell);
  }
  std::optional<Operand> stream;
  if (!sources.empty()) {
    stream = ctx.newRegister();
    const auto append =
        ctx.emitOp(OpcodeKind::StreamAppend, "#vlace_capture_stream");
    for (const auto cell : sources) ctx.emitInput(append, cell);
    ctx.emitOutput(append, stream->cell);
  }
  const auto handle = ctx.newRegister();
  const auto create = ctx.emitOp(OpcodeKind::CreateClosure, "#vlace_closure");
  ctx.emitInput(create, built.descriptor);
  if (stream) ctx.emitInput(create, stream->cell);
  ctx.emitOutput(create, handle.cell);
  return Operand(handle.cell, Evaluation::Whnf);
}

Lowered<BuiltClosure> buildThunkClosure(LowerContext &ctx, const ExprNode &e) {
  return buildClosure(
      ctx, false,
      [&e](LowerContext &c, std::optional<Operand>) -> Lowered<Operand> {
        auto value = lowerExpr(c, e);
        if (!value) return value;
        return toWhnf(c, *value, e.loc);
      },
      e.loc);
}

/// Bind a lambda parameter to the cell the closure was applied to. A variable
/// binds directly; any other pattern is matched, and a mismatch fails at run
/// time (spike: the arity of a lambda is not statically a match).
Lowered<void> bindParameter(LowerContext &ctx, const PatternNode &p,
                            Operand item) {
  if (const auto *var = std::get_if<PVar>(&p.node)) {
    ctx.bind(var->name, item);
    return {};
  }
  if (std::holds_alternative<PWildcard>(p.node)) return {};
  PatternSite site{.scrutinee = item,
                   .join      = ctx.mintLabel("#vlace_param_mismatch"),
                   .branches  = {}};
  if (auto matched = lowerPattern(ctx, p, site); !matched) return matched;
  if (site.branches.empty()) return {};
  const auto ok = ctx.mintLabel("#vlace_param_matched");
  ctx.jumpTo(ok);
  ctx.placeLabel(site.join);
  ctx.emitRuntimeFailure(CompileError::NonExhaustiveMatch, p.loc,
                         "lambda argument did not match its parameter pattern");
  ctx.placeLabel(ok);
  return {};
}

/// The closure for parameters `index..` of a lambda: it binds one parameter
/// and either returns the closure for the rest or evaluates the body.
Lowered<BuiltClosure> buildLambdaClosure(LowerContext &ctx,
                                         const std::vector<Pattern> &params,
                                         std::size_t index,
                                         const ExprNode &body,
                                         SourceLocation loc) {
  return buildClosure(
      ctx, true,
      [&params, index, &body,
       loc](LowerContext &c, std::optional<Operand> item) -> Lowered<Operand> {
        if (auto bound = bindParameter(c, *params[index], *item); !bound)
          return std::unexpected(bound.error());
        if (index + 1 < params.size()) {
          auto rest = buildLambdaClosure(c, params, index + 1, body, loc);
          if (!rest) return std::unexpected(rest.error());
          return emitClosureCreation(c, *rest, loc);
        }
        auto value = lowerExpr(c, body);
        if (!value) return value;
        return toWhnf(c, *value, body.loc);
      },
      loc);
}

// -- Thunks
// ----------------------------------------------------------------------------

/// A fresh cell to be a thunk. VqlBoxScalar mints a new cell per execution
/// holding the scalar read from its input, which is how a frame that runs many
/// times gets a distinct thunk identity each time.
Operand freshThunkCell(LowerContext &ctx) {
  const auto cell = ctx.newRegister();
  emit(ctx, OpcodeKind::VqlBoxScalar, "#vlace_thunk_cell",
       {intConstant(ctx, thunkSuspended)}, {cell.cell});
  return cell;
}

/// Give the thunk cell @p cell the code for @p e.
Lowered<void> installThunk(LowerContext &ctx, Operand cell, const ExprNode &e) {
  auto built = buildThunkClosure(ctx, e);
  if (!built) return std::unexpected(built.error());
  auto code = emitClosureCreation(ctx, *built, e.loc);
  if (!code) return std::unexpected(code.error());
  emitLinkWrite(ctx, cell.cell, ctx.state().kernelDims(ctx.core()).thunk,
                code->cell);
  return {};
}

/// The constructor and arguments when @p e is a constructor applied to exactly
/// its arity, which builds a term directly instead of calling a builder.
struct SaturatedCtor {
  std::string name;
  std::vector<const ExprNode *> args;
};

std::optional<SaturatedCtor> saturatedConstructor(LowerContext &ctx,
                                                  const EApp &e) {
  std::vector<const ExprNode *> reversed{e.arg.get()};
  const ExprNode *head = e.fn.get();
  while (const auto *inner = std::get_if<EApp>(&head->node)) {
    reversed.push_back(inner->arg.get());
    head = inner->fn.get();
  }
  const auto *ctor = std::get_if<ECtor>(&head->node);
  if (ctor == nullptr || ctor->name == "True" || ctor->name == "False")
    return std::nullopt;
  const auto info = ctx.state().ctors().find(ctor->name);
  if (!info || info->arity != reversed.size()) return std::nullopt;
  return SaturatedCtor{.name = ctor->name,
                       .args = std::vector<const ExprNode *>(reversed.rbegin(),
                                                             reversed.rend())};
}

/// Forms whose value is a constructor, lambda or literal: already in WHNF, so
/// no thunk is needed to delay them (their own fields are lazy).
bool isStaticWhnf(LowerContext &ctx, const ExprNode &e) {
  return std::visit(
      [&ctx](const auto &n) -> bool {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ELiteral> ||
                      std::is_same_v<T, ELambda> || std::is_same_v<T, ECtor> ||
                      std::is_same_v<T, ETuple> || std::is_same_v<T, EList>) {
          return true;
        } else if constexpr (std::is_same_v<T, EBinOp>) {
          return n.op == ":";
        } else if constexpr (std::is_same_v<T, EApp>) {
          return saturatedConstructor(ctx, n).has_value();
        } else if constexpr (std::is_same_v<T, ERecordUpdate>) {
          return std::holds_alternative<ECtor>(n.record->node);
        } else {
          return false;
        }
      },
      e.node);
}

Lowered<Operand> applyTo(LowerContext &ctx, const ExprNode &fn,
                         const ExprNode &arg, SourceLocation loc) {
  auto callee = lowerExpr(ctx, fn);
  if (!callee) return callee;
  auto function = toWhnf(ctx, *callee, loc);
  if (!function) return function;
  auto argument = lowerLazy(ctx, arg);
  if (!argument) return argument;
  return applyOperand(ctx, *function, *argument, loc);
}

} // namespace

void emitCopyReference(LowerContext &ctx, Operand src, Operand dst) {
  emit(ctx, OpcodeKind::CopyCellRefs, "#vlace_copy_ref", {src.cell},
       {dst.cell});
}

Lowered<Operand> lowerLet(LowerContext &ctx, const ELet &e, SourceLocation) {
  // The type checker and the evaluator this replaces both treat `let` as a
  // recursive group, so every right-hand side sees every name; `recursive` is
  // not consulted.
  for (const auto &b : e.bindings) {
    if (!std::holds_alternative<PVar>(b.pattern->node)) {
      return fail(CompileError::NonExhaustiveMatch,
                  "let binds a simple variable (destructuring let is a later "
                  "stage)",
                  b.pattern->loc);
    }
  }
  const auto scope = ctx.pushScope(ScopeKind::Block);
  std::vector<std::optional<Operand>> thunks;
  thunks.reserve(e.bindings.size());
  for (const auto &b : e.bindings) {
    const auto &name = std::get<PVar>(b.pattern->node).name;
    if (std::holds_alternative<ELiteral>(b.value->node)) {
      // A literal cannot refer to any name, so it needs no thunk.
      auto constant = lowerExpr(ctx, *b.value);
      if (!constant) return constant;
      ctx.bind(name, *constant);
      thunks.emplace_back(std::nullopt);
      continue;
    }
    const auto cell = freshThunkCell(ctx);
    ctx.bind(name, cell);
    thunks.emplace_back(cell);
  }
  for (std::size_t i = 0; i < e.bindings.size(); ++i) {
    if (!thunks[i]) continue;
    if (auto installed = installThunk(ctx, *thunks[i], *e.bindings[i].value);
        !installed)
      return std::unexpected(installed.error());
  }
  auto body = lowerExpr(ctx, *e.body);
  if (!body) return body;
  return toWhnf(ctx, *body, e.body->loc);
}

Lowered<Operand> lowerLambda(LowerContext &ctx, const ELambda &e,
                             SourceLocation loc) {
  if (e.params.empty())
    return internalError("a lambda without parameters", loc);
  auto built = buildLambdaClosure(ctx, e.params, 0, *e.body, loc);
  if (!built) return std::unexpected(built.error());
  return emitClosureCreation(ctx, *built, loc);
}

Lowered<Operand> lowerApp(LowerContext &ctx, const EApp &e,
                          SourceLocation loc) {
  if (const auto ctor = saturatedConstructor(ctx, e)) {
    std::vector<Operand> fields;
    fields.reserve(ctor->args.size());
    for (const auto *arg : ctor->args) {
      auto field = lowerLazy(ctx, *arg);
      if (!field) return field;
      fields.push_back(*field);
    }
    return makeTerm(ctx, ctor->name, fields, loc); // L3
  }
  return applyTo(ctx, *e.fn, *e.arg, loc);
}

Lowered<Operand> lowerDollar(LowerContext &ctx, const EBinOp &e,
                             SourceLocation loc) {
  return applyTo(ctx, *e.lhs, *e.rhs, loc);
}

Lowered<void> lowerValueDecl(LowerContext &ctx, const ValueDecl &d,
                             SourceLocation loc) {
  auto copy = std::make_shared<DeclCopy>();
  for (const auto &p : d.params) copy->params.push_back(clonePattern(*p));
  copy->body = cloneExpr(*d.body);
  copy->loc  = loc;
  // A redeclaration gets a new cell; code lowered against the old one keeps it.
  kernel(ctx).top.insert_or_assign(
      d.name, TopLevel{.cell     = ctx.arena().makeScalarCell(thunkSuspended),
                       .phase    = Phase::Declared,
                       .decl     = std::move(copy),
                       .failure  = std::nullopt,
                       .function = std::nullopt});
  return {};
}

namespace {

Lowered<void> linkThunkTo(LowerContext &ctx, CellRef thunk, CellRef target) {
  zigzag::expectWritten(ctx.arena().link(
      thunk, ctx.state().kernelDims(ctx.core()).thunk, DimVector::POS, target));
  return {};
}

/// Lower a declaration in a context of its own (so no local of the referring
/// code is visible to it) and wire its global cell: a function becomes an
/// already-forced thunk over a box of its closure, a value an unforced thunk.
Lowered<void> materialize(LowerContext &ctx, TopLevel &entry) {
  LowerContext sub(ctx.compiler());
  const DeclCopy &decl = *entry.decl;
  const std::span<const std::vector<CellRef>> noCaptures;
  if (decl.params.empty()) {
    auto built = buildThunkClosure(sub, *decl.body);
    if (!built) return std::unexpected(built.error());
    if (!built->captures.empty())
      return internalError("a top-level value captured a local", decl.loc);
    const auto code = sub.vm().makeClosureValue(built->descriptor, noCaptures);
    if (!code) return internalError("closure: " + code.error().message);
    return linkThunkTo(sub, entry.cell, *code);
  }
  auto built = buildLambdaClosure(sub, decl.params, 0, *decl.body, decl.loc);
  if (!built) return std::unexpected(built.error());
  if (!built->captures.empty())
    return internalError("a top-level function captured a local", decl.loc);
  const auto function =
      sub.vm().makeClosureValue(built->descriptor, noCaptures);
  if (!function) return internalError("closure: " + function.error().message);
  auto box = boxClosureDescriptor(sub);
  if (!box) return std::unexpected(box.error());
  const std::vector<std::vector<CellRef>> boxed{{*function}};
  const auto memo = sub.vm().makeClosureValue(*box, boxed);
  if (!memo) return internalError("closure: " + memo.error().message);
  sub.core().writeValue(entry.cell, 0, std::nullopt, CellValue{thunkDone});
  entry.function = *function;
  return linkThunkTo(sub, entry.cell, *memo);
}

} // namespace

namespace {

/// The declared name's operand as it stands: the closure root of a built
/// function, otherwise the global thunk cell, unforced.
std::optional<Lowered<Operand>> topLevelOperand(LowerContext &ctx,
                                                std::string_view name) {
  auto &top        = kernel(ctx).top;
  const auto found = top.find(std::string(name));
  if (found == top.end()) return std::nullopt;
  auto &entry = found->second;
  if (entry.phase == Phase::Declared) {
    entry.phase     = Phase::Lowering;
    const auto fail = [&entry](EvalError error) {
      entry.phase   = Phase::Failed;
      entry.failure = std::move(error);
      entry.decl.reset();
    };
    try {
      if (auto built = materialize(ctx, entry); !built) {
        fail(built.error());
        return std::unexpected(*entry.failure);
      }
    } catch (const LoweringBudgetExceeded &budget) {
      fail(budget.error());
      throw;
    }
    entry.phase = Phase::Ready;
    entry.decl.reset();
  }
  switch (entry.phase) {
  case Phase::Failed:
    return std::unexpected(*entry.failure);
  case Phase::Ready:
    if (entry.function) return Operand(*entry.function, Evaluation::Whnf);
    [[fallthrough]];
  default:
    return Operand(entry.cell, Evaluation::Unknown);
  }
}

} // namespace

std::optional<Lowered<Operand>>
lookupTopLevel(LowerContext &ctx, std::string_view name, SourceLocation loc) {
  auto operand = topLevelOperand(ctx, name);
  if (!operand || !*operand) return operand;
  // The caller (lowerVar) hands this back as the value of the expression, so
  // it has to be in WHNF; lowerLazy asks for the unforced operand instead.
  return toWhnf(ctx, **operand, loc);
}

Lowered<Operand> captureOperand(LowerContext &ctx, const ScopeHit &hit,
                                SourceLocation loc) {
  auto &frames = kernel(ctx).frames;
  if (frames.empty())
    return internalError("a variable outside every closure frame needs capture",
                         loc);
  auto &build = frames.back();
  for (const auto &capture : build.captures) {
    if (capture.outer == hit.operand.cell)
      return Operand(capture.inner.cell, capture.state);
  }
  const auto inner = ctx.newRegister();
  build.captures.push_back(CaptureRecord{.outer = hit.operand.cell,
                                         .depth = hit.frameDepth,
                                         .state = hit.operand.state,
                                         .inner = inner});
  ctx.addCapture(std::string(captureNamePrefix) +
                     std::to_string(build.captures.size()),
                 hit.operand, inner);
  return Operand(inner.cell, hit.operand.state);
}

Lowered<Operand> lowerLazy(LowerContext &ctx, const ExprNode &e) {
  if (const auto *var = std::get_if<EVar>(&e.node)) {
    // A variable names an existing thunk or value; delaying it again would
    // only add a layer.
    const auto hit = ctx.lookup(var->name);
    if (!hit) {
      if (auto global = topLevelOperand(ctx, var->name)) return *global;
      return resolveGlobalName(ctx, var->name, e.loc);
    }
    if (hit->frameDepth > 0) return captureOperand(ctx, *hit, e.loc);
    return hit->operand;
  }
  if (isStaticWhnf(ctx, e)) return lowerExpr(ctx, e);
  const auto cell = freshThunkCell(ctx);
  if (auto installed = installThunk(ctx, cell, e); !installed)
    return std::unexpected(installed.error());
  return cell;
}

Lowered<Operand> forceOperand(LowerContext &ctx, Operand value,
                              SourceLocation) {
  if (value.state == Evaluation::Whnf) return value;
  const auto frame = forceFrameDescriptor(ctx);
  if (!frame) return std::unexpected(frame.error());
  const auto out = ctx.newRegister();
  emit(ctx, OpcodeKind::InvokeFrame, "#vlace_force", {*frame, value.cell},
       {out.cell});
  return Operand(out.cell, Evaluation::Whnf);
}

Lowered<Operand> applyOperand(LowerContext &ctx, Operand fn, Operand arg,
                              SourceLocation loc) {
  auto function = toWhnf(ctx, fn, loc);
  if (!function) return function;
  const auto out = ctx.newRegister();
  emit(ctx, OpcodeKind::ApplyClosure, "#vlace_apply",
       {function->cell, arg.cell}, {out.cell});
  return Operand(out.cell, Evaluation::Whnf);
}

std::expected<ValuePtr, EvalError> forceValueOnVm(Compiler &compiler,
                                                  ValuePtr value) {
  if (!value || !value->cell || value->kind != Value::Kind::Thunk) return value;
  LowerContext ctx(compiler);
  try {
    auto forced = forceOperand(ctx, Operand(*value->cell), {});
    if (!forced) return std::unexpected(forced.error());
    auto ran = runBlock(ctx, ctx.swapBlock(Block{}), *forced);
    if (!ran) return std::unexpected(ran.error());
    return readbackCell(compiler, ran->cell);
  } catch (const LoweringBudgetExceeded &budget) {
    return std::unexpected(budget.error());
  } catch (const std::exception &error) {
    if (auto budget = rewordBudgetFailure(error.what()))
      return std::unexpected(*budget);
    return internalError(error.what());
  }
}

std::optional<Lowered<ValuePtr>> readbackKernel(Compiler &compiler,
                                                CellRef cell) {
  auto &vm             = compiler.vm();
  auto &core           = compiler.core();
  const auto &arena    = core.arena();
  const auto &dims     = compiler.lowerState().kernelDims(core);
  const auto makeValue = [cell](Value::Kind kind) {
    auto v  = std::make_shared<Value>();
    v->kind = kind;
    v->cell = cell;
    return v;
  };
  // A register that names another cell stands for it.
  if (const auto target = vm.cellReferenceOf(cell); target && *target != cell)
    return readbackCell(compiler, *target);
  if (const auto code = arena.linked(cell, dims.thunk, DimVector::POS)) {
    if (arena.asInt64(cell) == thunkDone) {
      // The box holds the result as its one capture.
      if (const auto boxed = vm.closureValue(*code);
          boxed && boxed->captures.size() == 1 &&
          boxed->captures.front().size() == 1)
        return readbackCell(compiler, boxed->captures.front().front());
    }
    return makeValue(Value::Kind::Thunk);
  }
  if (vm.closureValue(cell)) return makeValue(Value::Kind::Closure);
  return std::nullopt;
}

} // namespace xanadu::vlace::lower
