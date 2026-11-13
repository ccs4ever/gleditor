/**
 * @file lower_data.cpp
 * @brief Card L3: constructors, tuples, lists, patterns, `case`, the built-in
 *        constructors, and the read-back and deep rendering of constructed
 *        values.
 *
 * Representation. A constructed value is a *term*: a root cell whose content
 * is the constructor's tag, with a header cell on `+d.grab` and the fields
 * chained after the header on `+d.step` (the Vlog term shape the spec's §4.1
 * names). The header holds the arity plus one, so
 *  - a pattern tests tag and arity with two `Eq`s and never reads a field of a
 *    String or Int that merely looks like the tag (type checking can be off),
 *  - a nullary term still has a `+d.grab`, which is what tells read-back it is
 *    a term and not the string of the same name.
 * Every field is a *slot* cell of its own, never the field's value cell:
 * `MakeTerm` links its arguments into one chain, so a value cell reachable from
 * two terms (a literal constant, a shared variable) would be stolen by the
 * second. A slot is one of
 *  - a fresh thunk the field's own expression just created (used as is),
 *  - a fresh copy of a scalar literal,
 *  - a fresh term root built for a nested constructor expression, or
 *  - a *forwarder*: a new thunk whose code forces the cell it stands for, so
 *    laziness and sharing both survive (a pattern variable bound to a slot is
 *    forced through the same kernel `force` as any other thunk).
 * A nullary constructor is one canonical atom per tag, made at lowering time.
 */
#include <array>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "common/xanadu/vlace/kernel.hpp"
#include "common/xanadu/vlace/lower_cards.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vlace::lower {

namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::CellValue;
using zigzag::vortex::ClosureDescriptorSpec;
using zigzag::vortex::ClosureInputSource;
using zigzag::vortex::FrameDescriptorSpec;
using zigzag::vortex::FramePort;
using zigzag::vortex::FramePortKind;
using zigzag::vortex::OpcodeKind;

/// Link direction operand for the Link opcode: +1 reads/writes posward.
constexpr std::int64_t linkPosward = 1;

/// The runtime-owned constructor tags and their arities (the prelude ADTs and
/// the scalar wrappers). `Tuple` has no entry: its arity is the tuple's own.
/// IO.* tags are declared by the card that builds them, on first use.
struct BuiltinCtor {
  std::string_view tag;
  std::size_t arity;
};
constexpr std::array<BuiltinCtor, 11> builtinCtors{{
    {"Nil", 0},
    {"Cons", 2},
    {"None", 0},
    {"Some", 1},
    {"Ok", 1},
    {"Err", 1},
    {"True", 0},
    {"False", 0},
    {"SInt", 1},
    {"SFloat", 1},
    {"SBool", 1},
}};

/// The opaque tags a program may match on but never construct. They are kept
/// out of the constructor table on purpose: an application of a tabled
/// constructor to its arity is built directly by the kernel, without passing
/// lowerCtor's refusal, so a tabled `Cell` would be forgeable.
constexpr std::array<BuiltinCtor, 3> opaqueCtors{{
    {"Cell", 1},
    {"Dim", 1},
    {"Unit", 0},
}};

constexpr std::string_view nilTag   = "Nil";
constexpr std::string_view consTag  = "Cons";
constexpr std::string_view tupleTag = "Tuple";

/// How deeply `case` and nested constructor expressions may nest before
/// lowering is refused. Lowering recurses on the host stack and the parser does
/// not bound nesting; the figure matches the kernel's expression budget.
constexpr std::size_t maxLoweringDepth = 1000;

thread_local std::size_t loweringDepth = 0;

class DepthGuard {
public:
  DepthGuard() {
    if (++loweringDepth > maxLoweringDepth) {
      --loweringDepth;
      throw LoweringBudgetExceeded(EvalError{
          .code   = CompileError::BudgetExhausted,
          .detail = "Vlace evaluation budget exhausted: expression nesting",
          .loc    = {}});
    }
  }
  DepthGuard(const DepthGuard &)            = delete;
  DepthGuard &operator=(const DepthGuard &) = delete;
  ~DepthGuard() { --loweringDepth; }
};

/// What survives from one evaluation to the next.
struct DataState {
  /// One canonical atom per nullary tag.
  std::unordered_map<std::string, CellRef> atoms;
  /// The closure descriptor of a forwarder's code.
  std::optional<CellRef> forwarder;
  /// Variables bound so far by the pattern whose mismatch label is the key,
  /// for the compile-time linearity check. Cleared per `case` alternative; a
  /// lambda parameter pattern leaves its (small) entry behind.
  std::unordered_map<CellRef, std::unordered_set<std::string>> bound;
};

DataState &data(LowerContext &ctx) {
  return ctx.state().cardState<DataState>(Card::Data);
}

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

Operand emitBinary(LowerContext &ctx, OpcodeKind kind, CellRef lhs,
                   CellRef rhs) {
  const auto out = ctx.newRegister();
  emit(ctx, kind, "#vlace_data_binop", {lhs, rhs}, {out.cell});
  return Operand(out.cell, Evaluation::Whnf);
}

/// `out := Link(subject, dim, +1)`: the cell @p subject links to posward on
/// @p dim, or the null reference (which reads as 0) when it has none.
Operand emitLinkRead(LowerContext &ctx, CellRef subject, zigzag::DimRef dim) {
  const auto out = ctx.newRegister();
  emit(ctx, OpcodeKind::Link, "#vlace_data_link",
       {subject, intConstant(ctx, static_cast<std::int64_t>(dim)),
        intConstant(ctx, linkPosward)},
       {out.cell});
  return out;
}

void emitLinkWrite(LowerContext &ctx, CellRef subject, zigzag::DimRef dim,
                   CellRef target) {
  emit(ctx, OpcodeKind::Link, "#vlace_data_link_write",
       {subject, intConstant(ctx, static_cast<std::int64_t>(dim)),
        intConstant(ctx, linkPosward), target});
}

/// A fresh cell holding a copy of the scalar in @p constant (VqlBoxScalar mints
/// a new cell on every execution, so a frame that runs often gets a distinct
/// one each time).
Operand emitFreshScalar(LowerContext &ctx, Operand constant) {
  const auto out = ctx.newRegister();
  emit(ctx, OpcodeKind::VqlBoxScalar, "#vlace_data_fresh", {constant.cell},
       {out.cell});
  return out;
}

std::vector<CellRef> distinct(std::vector<CellRef> cells) {
  std::unordered_set<CellRef> seen;
  std::erase_if(cells, [&seen](CellRef c) { return !seen.insert(c).second; });
  return cells;
}

/// Swap in an empty Block for a frame under construction and swap the caller's
/// back on every exit, so a throw cannot leave a half-built frame as current.
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

Lowered<Operand> toWhnf(LowerContext &ctx, Operand value, SourceLocation loc) {
  if (value.state == Evaluation::Whnf) return value;
  return forceOperand(ctx, value, loc); // L2
}

// -- Atoms and term roots

/// The canonical cell of the nullary constructor @p tag: the tag as content and
/// a header holding one (arity 0 plus one) on `+d.grab`. Built at lowering
/// time, shared by every use, never mutated.
Operand atomFor(LowerContext &ctx, std::string_view tag) {
  auto &atoms = data(ctx).atoms;
  if (const auto found = atoms.find(std::string(tag)); found != atoms.end())
    return Operand(found->second, Evaluation::Whnf);
  auto &arena        = ctx.arena();
  const CellRef root = arena.makeCell(tag);
  const CellRef head = arena.makeScalarCell(std::int64_t{1});
  zigzag::expectWritten(
      arena.link(root, ctx.core().dims().grab, DimVector::POS, head));
  atoms.emplace(std::string(tag), root);
  return Operand(root, Evaluation::Whnf);
}

/// The header value of a term of @p arity: its rank length.
std::int64_t headerOf(std::size_t arity) {
  return static_cast<std::int64_t>(arity) + 1;
}

/// `MakeTerm tag header slot...` in the current block. The slots must each be a
/// cell that no other chain holds (see the file comment).
Operand emitTerm(LowerContext &ctx, std::string_view tag,
                 std::span<const CellRef> slots) {
  const auto header =
      emitFreshScalar(ctx, ctx.emitConstant(CellValue{headerOf(slots.size())}));
  const auto out = ctx.newRegister();
  const auto op  = ctx.emitOp(OpcodeKind::MakeTerm, "#vlace_term");
  ctx.emitInput(op, ctx.emitConstant(CellValue{std::string(tag)}).cell);
  ctx.emitInput(op, header.cell);
  for (const auto slot : slots) ctx.emitInput(op, slot);
  ctx.emitOutput(op, out.cell);
  return Operand(out.cell, Evaluation::Whnf);
}

// -- Forwarders: a slot for a cell that something else also holds

/// The shared frame behind every forwarder: one captured cell in, that cell
/// forced out.
Lowered<CellRef> forwarderDescriptor(LowerContext &ctx) {
  auto &state = data(ctx);
  if (state.forwarder) return *state.forwarder;
  const auto reserved = ctx.vm().reserveFrameDescriptor();
  if (!reserved)
    return internalError("frame descriptor: " + reserved.error().message);
  BlockSwap swap(ctx);
  const auto port     = ctx.newRegister();
  const auto iterator = ctx.newRegister();
  const auto present  = ctx.newRegister();
  const auto item     = ctx.newRegister();
  emit(ctx, OpcodeKind::StreamBegin, "#vlace_data_unpack", {port.cell},
       {iterator.cell});
  emit(ctx, OpcodeKind::IteratorNext, "#vlace_data_unpack_next",
       {iterator.cell}, {item.cell, present.cell});
  const auto forced = forceOperand(ctx, item, {}); // L2
  if (!forced) return std::unexpected(forced.error());
  const auto result = ctx.newRegister();
  emitCopyReference(ctx, *forced, result);
  emit(ctx, OpcodeKind::ReturnFrame, "#vlace_return", {*reserved});
  const Block body = swap.take();
  if (!body.entry) return internalError("a forwarder frame emitted nothing");
  const auto done = ctx.vm().finalizeFrameDescriptor(
      *reserved,
      FrameDescriptorSpec{
          .entry         = body.entry,
          .locals        = distinct(body.locals),
          .argumentPorts = {FramePort{port.cell, FramePortKind::Stream}},
          .resultPorts   = {FramePort{result.cell, FramePortKind::Cell}}});
  if (!done) return internalError("frame descriptor: " + done.error().message);
  const auto closure = ctx.vm().makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor    = *reserved,
      .captureStreamCount = 1,
      .appliedStreamCount = 0,
      .frameInputs = {{.source = ClosureInputSource::Capture, .index = 0}}});
  if (!closure)
    return internalError("closure descriptor: " + closure.error().message);
  state.forwarder = *closure;
  return *closure;
}

/// A new thunk cell that evaluates to the forced value of @p target.
Lowered<Operand> emitForwarder(LowerContext &ctx, Operand target) {
  const auto descriptor = forwarderDescriptor(ctx);
  if (!descriptor) return std::unexpected(descriptor.error());
  const auto cell =
      emitFreshScalar(ctx, ctx.emitConstant(CellValue{thunkSuspended}));
  const auto stream = ctx.newRegister();
  emit(ctx, OpcodeKind::StreamAppend, "#vlace_data_capture", {target.cell},
       {stream.cell});
  const auto code = ctx.newRegister();
  emit(ctx, OpcodeKind::CreateClosure, "#vlace_data_forwarder",
       {*descriptor, stream.cell}, {code.cell});
  emitLinkWrite(ctx, cell.cell, ctx.state().kernelDims(ctx.core()).thunk,
                code.cell);
  return cell;
}

// -- Field slots

bool isConsExpr(const ExprNode &e) {
  const auto *op = std::get_if<EBinOp>(&e.node);
  return op != nullptr && op->op == ":";
}

/// A slot for the field @p e. The slot is lazy: nothing in @p e runs until the
/// slot is forced.
Lowered<Operand> fieldSlot(LowerContext &ctx, const ExprNode &e) {
  if (const auto *lit = std::get_if<ELiteral>(&e.node)) {
    const auto constant = lowerLiteral(ctx, *lit, e.loc); // L1
    if (!constant) return constant;
    return emitFreshScalar(ctx, *constant);
  }
  if (const auto *ctor = std::get_if<ECtor>(&e.node);
      ctor != nullptr && (ctor->name == "True" || ctor->name == "False")) {
    return emitFreshScalar(ctx,
                           ctx.emitConstant(CellValue{ctor->name == "True"}));
  }
  const auto *list = std::get_if<EList>(&e.node);
  if (std::holds_alternative<ETuple>(e.node) || isConsExpr(e) ||
      (list != nullptr && !list->elems.empty())) {
    // A nested constructor expression builds a fresh root nobody else holds.
    return lowerExpr(ctx, e);
  }
  auto lazy = lowerLazy(ctx, e); // L2
  if (!lazy) return lazy;
  // Only the forms lowerLazy always thunks hand back a fresh cell; a variable,
  // an atom, a closure and the rest are cells something else may also hold.
  const bool alwaysThunked = std::holds_alternative<ELet>(e.node) ||
                             std::holds_alternative<EIf>(e.node) ||
                             std::holds_alternative<ECase>(e.node) ||
                             std::holds_alternative<EBinOp>(e.node) ||
                             std::holds_alternative<EApp>(e.node);
  if (alwaysThunked && lazy->state == Evaluation::Unknown) return lazy;
  return emitForwarder(ctx, *lazy);
}

/// Slots for @p operands that are existing cells (makeTerm's callers hand over
/// registers, not expressions).
Lowered<std::vector<CellRef>> forwardAll(LowerContext &ctx,
                                         std::span<const Operand> operands) {
  std::vector<CellRef> slots;
  slots.reserve(operands.size());
  for (const auto &operand : operands) {
    auto slot = emitForwarder(ctx, operand);
    if (!slot) return std::unexpected(slot.error());
    slots.push_back(slot->cell);
  }
  return slots;
}

/// A cons spine `a : b : ... : tail`, built from the tail outwards so a long
/// literal list does not recurse. @p heads are the element expressions in
/// order; @p tail is the expression after the last `:`, or null for `[]`.
Lowered<Operand> buildCons(LowerContext &ctx,
                           std::span<const ExprNode *const> heads,
                           const ExprNode *tail) {
  auto tailSlot = tail != nullptr ? fieldSlot(ctx, *tail)
                                  : emitForwarder(ctx, atomFor(ctx, nilTag));
  if (!tailSlot) return tailSlot;
  Operand term = *tailSlot;
  for (auto it = heads.rbegin(); it != heads.rend(); ++it) {
    auto head = fieldSlot(ctx, **it);
    if (!head) return head;
    const std::array slots{head->cell, term.cell};
    term = emitTerm(ctx, consTag, slots);
  }
  return term;
}

// -- Patterns

/// Does the pattern look at its scrutinee before it can decline?
bool patternForces(const PatternNode &p) {
  return !std::holds_alternative<PWildcard>(p.node) &&
         !std::holds_alternative<PVar>(p.node);
}

/// A new site for a sub-pattern: same mismatch label, its own scrutinee.
PatternSite subSite(const PatternSite &site, Operand scrutinee) {
  return PatternSite{.scrutinee = scrutinee, .join = site.join, .branches = {}};
}

void mergeBranches(PatternSite &site, const PatternSite &sub) {
  site.branches.insert(site.branches.end(), sub.branches.begin(),
                       sub.branches.end());
}

void testOrMismatch(LowerContext &ctx, Operand test, PatternSite &site) {
  site.branches.push_back(ctx.branchIfFalse(test, site.join));
}

/// Match the term structure: tag, then header, then each non-wildcard
/// sub-pattern against its slot. @p subs are the sub-patterns by position.
Lowered<void> matchTerm(LowerContext &ctx, std::string_view tag,
                        std::span<const PatternNode *const> subs,
                        SourceLocation loc, PatternSite &site) {
  auto scrut = toWhnf(ctx, site.scrutinee, loc);
  if (!scrut) return std::unexpected(scrut.error());
  testOrMismatch(ctx,
                 emitBinary(ctx, OpcodeKind::Eq, scrut->cell,
                            ctx.emitConstant(CellValue{std::string(tag)}).cell),
                 site);
  const auto header = emitLinkRead(ctx, scrut->cell, ctx.core().dims().grab);
  testOrMismatch(
      ctx,
      emitBinary(ctx, OpcodeKind::Eq, header.cell,
                 ctx.emitConstant(CellValue{headerOf(subs.size())}).cell),
      site);
  // Only the slots up to the last pattern that looks at its slot are read.
  std::size_t reach = 0;
  for (std::size_t i = 0; i < subs.size(); ++i) {
    if (!std::holds_alternative<PWildcard>(subs[i]->node)) reach = i + 1;
  }
  Operand previous = header;
  for (std::size_t i = 0; i < reach; ++i) {
    const auto slot = emitLinkRead(ctx, previous.cell, ctx.core().dims().step);
    previous        = slot;
    if (std::holds_alternative<PWildcard>(subs[i]->node)) continue;
    auto sub = subSite(site, slot);
    if (auto matched = lowerPattern(ctx, *subs[i], sub); !matched)
      return matched;
    mergeBranches(site, sub);
  }
  return {};
}

/// Fail at run time unless @p operand is exactly a Bool (see lower_scalars.cpp:
/// the same pair of Eq tests, since BranchFalseCell would otherwise test the
/// truthiness of anything).
void emitBoolGuard(LowerContext &ctx, Operand operand, SourceLocation loc,
                   std::string_view message) {
  const auto isTrue  = emitBinary(ctx, OpcodeKind::Eq, operand.cell,
                                  ctx.emitConstant(CellValue{true}).cell);
  const auto isFalse = emitBinary(ctx, OpcodeKind::Eq, operand.cell,
                                  ctx.emitConstant(CellValue{false}).cell);
  const auto isBool =
      emitBinary(ctx, OpcodeKind::Or, isTrue.cell, isFalse.cell);
  const auto bad = ctx.mintLabel("#vlace_guard_not_bool");
  const auto ok  = ctx.mintLabel("#vlace_guard_is_bool");
  ctx.branchIfFalse(isBool, bad);
  ctx.jumpTo(ok);
  ctx.placeLabel(bad);
  ctx.emitRuntimeFailure(CompileError::TypeMismatch, loc, message);
  ctx.placeLabel(ok);
}

/// Reading a term's fields reads each slot back, and a slot that holds another
/// term would read that one's fields in turn: a long forced list would recurse
/// as deep as it is long. A term met while its parent's fields are being read
/// is returned as a handle (Kind::Thunk on its root) instead; rendering and the
/// mesh evaluator force a field before they look at it.
thread_local bool readingFields = false;

class FieldReading {
public:
  FieldReading() : outer_(std::exchange(readingFields, true)) {}
  FieldReading(const FieldReading &)            = delete;
  FieldReading &operator=(const FieldReading &) = delete;
  ~FieldReading() { readingFields = outer_; }

private:
  bool outer_;
};

} // namespace

// -- Constructors and tuples

Lowered<Operand> lowerCtor(LowerContext &ctx, const ECtor &e,
                           SourceLocation loc) {
  if (isOpaqueCtorTag(e.name)) {
    return fail(CompileError::TypeMismatch,
                "constructor '" + e.name +
                    "' is runtime-owned and cannot be constructed",
                loc);
  }
  const auto info = ctx.state().ctors().find(e.name);
  if (!info) {
    return fail(CompileError::UnboundIdentifier,
                "unknown constructor '" + e.name + "'", loc);
  }
  if (info->arity == 0) return atomFor(ctx, e.name);
  return termBuilder(ctx, e.name, info->arity, loc);
}

Lowered<Operand> lowerTuple(LowerContext &ctx, const ETuple &e,
                            SourceLocation) {
  const DepthGuard depth;
  std::vector<CellRef> slots;
  slots.reserve(e.elems.size());
  for (const auto &elem : e.elems) {
    auto slot = fieldSlot(ctx, *elem);
    if (!slot) return slot;
    slots.push_back(slot->cell);
  }
  return emitTerm(ctx, tupleTag, slots);
}

Lowered<Operand> lowerList(LowerContext &ctx, const EList &e, SourceLocation) {
  if (e.elems.empty()) return atomFor(ctx, nilTag);
  std::vector<const ExprNode *> heads;
  heads.reserve(e.elems.size());
  for (const auto &elem : e.elems) heads.push_back(elem.get());
  return buildCons(ctx, heads, nullptr);
}

Lowered<Operand> lowerCons(LowerContext &ctx, const EBinOp &e, SourceLocation) {
  std::vector<const ExprNode *> heads;
  const EBinOp *cur    = &e;
  const ExprNode *tail = nullptr;
  while (tail == nullptr) {
    heads.push_back(cur->lhs.get());
    const ExprNode &next = *cur->rhs;
    const auto *inner    = std::get_if<EBinOp>(&next.node);
    if (inner != nullptr && inner->op == ":") {
      cur = inner;
    } else {
      tail = &next;
    }
  }
  return buildCons(ctx, heads, tail);
}

Lowered<Operand> makeTerm(LowerContext &ctx, std::string_view tag,
                          std::span<const Operand> fields, SourceLocation) {
  if (fields.empty()) return atomFor(ctx, tag);
  auto slots = forwardAll(ctx, fields);
  if (!slots) return std::unexpected(slots.error());
  return emitTerm(ctx, tag, *slots);
}

Lowered<Operand> termBuilder(LowerContext &ctx, std::string_view tag,
                             std::size_t arity, SourceLocation loc) {
  if (arity == 0) return atomFor(ctx, tag);
  // An IO.* tag is known by its builder alone (no program can write one);
  // record its arity so the application the builder is made of saturates. A
  // runtime-owned tag such as Cell is built by makeTerm and has no builder.
  const auto known = ctx.state().ctors().find(tag);
  if (!known) {
    if (!tag.starts_with("IO.")) {
      return internalError("constructor '" + std::string(tag) +
                               "' is not declared, so it has no builder",
                           loc);
    }
    ctx.state().ctors().declare(
        CtorInfo{.name = std::string(tag), .arity = arity, .fields = {}});
  } else if (known->arity != arity) {
    return internalError("constructor '" + std::string(tag) + "' takes " +
                             std::to_string(known->arity) + " arguments, not " +
                             std::to_string(arity),
                         loc);
  }
  // `fn a0 .. an => Tag a0 .. an`: the lambda machinery supplies the curried
  // closure frames, and the saturated application inside builds the term.
  const auto argName = [](std::size_t i) {
    return "#ctor_arg" + std::to_string(i);
  };
  Expr body  = std::make_unique<ExprNode>();
  body->loc  = loc;
  body->node = ECtor{.name = std::string(tag)};
  ELambda lambda;
  for (std::size_t i = 0; i < arity; ++i) {
    auto arg  = std::make_unique<ExprNode>();
    arg->loc  = loc;
    arg->node = EVar{.name = argName(i)};
    auto app  = std::make_unique<ExprNode>();
    app->loc  = loc;
    app->node = EApp{.fn = std::move(body), .arg = std::move(arg)};
    body      = std::move(app);
    auto pat  = std::make_unique<PatternNode>();
    pat->loc  = loc;
    pat->node = PVar{.name = argName(i)};
    lambda.params.push_back(std::move(pat));
  }
  lambda.body = std::move(body);
  ExprNode root;
  root.loc  = loc;
  root.node = std::move(lambda);
  return lowerExpr(ctx, root);
}

// -- Patterns

Lowered<void> lowerPatVar(LowerContext &ctx, const PVar &p, SourceLocation loc,
                          PatternSite &site) {
  if (!data(ctx).bound[site.join].insert(p.name).second) {
    return fail(CompileError::NonLinearPattern,
                "'" + p.name + "' bound twice in one pattern", loc);
  }
  ctx.bind(p.name, site.scrutinee);
  return {};
}

Lowered<void> lowerPatLiteral(LowerContext &ctx, const PLiteral &p,
                              SourceLocation loc, PatternSite &site) {
  auto scrut = toWhnf(ctx, site.scrutinee, loc);
  if (!scrut) return std::unexpected(scrut.error());
  switch (p.lit.kind) {
  case LiteralKind::Int:
  case LiteralKind::Char: // a Char is its code point, as it always was
    testOrMismatch(ctx,
                   emitBinary(ctx, OpcodeKind::Eq, scrut->cell,
                              ctx.emitConstant(CellValue{p.lit.intValue}).cell),
                   site);
    return {};
  case LiteralKind::Float:
    testOrMismatch(
        ctx,
        emitBinary(ctx, OpcodeKind::Eq, scrut->cell,
                   ctx.emitConstant(CellValue{p.lit.floatValue}).cell),
        site);
    return {};
  case LiteralKind::String: {
    // A term's tag is text too: a String pattern must not match the atom `Nil`
    // or a constructor named like the string. Only a term has a `+d.grab`.
    const auto header = emitLinkRead(ctx, scrut->cell, ctx.core().dims().grab);
    testOrMismatch(
        ctx, emitBinary(ctx, OpcodeKind::Eq, header.cell, intConstant(ctx, 0)),
        site);
    testOrMismatch(
        ctx,
        emitBinary(ctx, OpcodeKind::Eq, scrut->cell,
                   ctx.emitConstant(CellValue{p.lit.textValue}).cell),
        site);
    return {};
  }
  }
  return {};
}

Lowered<void> lowerPatConstructor(LowerContext &ctx, const PConstructor &p,
                                  SourceLocation loc, PatternSite &site) {
  if (p.ctor == "True" || p.ctor == "False") {
    if (!p.args.empty()) {
      return fail(CompileError::TypeMismatch,
                  "constructor '" + p.ctor + "' takes no arguments", loc);
    }
    auto scrut = toWhnf(ctx, site.scrutinee, loc);
    if (!scrut) return std::unexpected(scrut.error());
    testOrMismatch(
        ctx,
        emitBinary(ctx, OpcodeKind::Eq, scrut->cell,
                   ctx.emitConstant(CellValue{p.ctor == "True"}).cell),
        site);
    return {};
  }
  auto info = ctx.state().ctors().find(p.ctor);
  if (!info) {
    for (const auto &[tag, arity] : opaqueCtors) {
      if (tag == p.ctor)
        info = CtorInfo{.name = p.ctor, .arity = arity, .fields = {}};
    }
  }
  if (!info) {
    return fail(CompileError::UnboundIdentifier,
                "unknown constructor '" + p.ctor + "' in pattern", loc);
  }
  if (info->arity != p.args.size()) {
    return fail(
        CompileError::TypeMismatch,
        "constructor '" + p.ctor + "' takes " + std::to_string(info->arity) +
            " arguments but the pattern has " + std::to_string(p.args.size()),
        loc);
  }
  std::vector<const PatternNode *> subs;
  subs.reserve(p.args.size());
  for (const auto &arg : p.args) subs.push_back(arg.get());
  return matchTerm(ctx, p.ctor, subs, loc, site);
}

Lowered<void> lowerPatTuple(LowerContext &ctx, const PTuple &p,
                            SourceLocation loc, PatternSite &site) {
  std::vector<const PatternNode *> subs;
  subs.reserve(p.elems.size());
  for (const auto &elem : p.elems) subs.push_back(elem.get());
  return matchTerm(ctx, tupleTag, subs, loc, site);
}

// -- case

Lowered<Operand> lowerCase(LowerContext &ctx, const ECase &e,
                           SourceLocation loc) {
  const DepthGuard depth;
  // The scrutinee is forced to weak head normal form only, and only if the
  // first alternative looks at it (a variable or wildcard first leaves it a
  // thunk); a later alternative forces it itself.
  const bool forcesFirst =
      !e.alts.empty() && patternForces(*e.alts.front().pattern);
  auto scrutinee = [&]() -> Lowered<Operand> {
    if (!forcesFirst) return lowerLazy(ctx, *e.scrutinee); // L2
    auto strict = lowerExpr(ctx, *e.scrutinee);
    if (!strict) return strict;
    return toWhnf(ctx, *strict, e.scrutinee->loc);
  }();
  if (!scrutinee) return scrutinee;

  const auto result = ctx.newRegister();
  const auto end    = ctx.mintLabel("#vlace_case_end");
  for (const auto &alt : e.alts) {
    const auto scope = ctx.pushScope(ScopeKind::Block);
    PatternSite site{.scrutinee = *scrutinee,
                     .join      = ctx.mintLabel("#vlace_alt_next"),
                     .branches  = {}};
    const auto matched = lowerPattern(ctx, *alt.pattern, site);
    data(ctx).bound.erase(site.join);
    if (!matched) return std::unexpected(matched.error());
    if (alt.guard) {
      auto guard = lowerExpr(ctx, **alt.guard);
      if (!guard) return guard;
      auto forced = toWhnf(ctx, *guard, (*alt.guard)->loc);
      if (!forced) return forced;
      emitBoolGuard(ctx, *forced, (*alt.guard)->loc, "case guard must be Bool");
      ctx.branchIfFalse(*forced, site.join);
    }
    auto body = lowerExpr(ctx, *alt.body);
    if (!body) return body;
    auto value = toWhnf(ctx, *body, alt.body->loc);
    if (!value) return value;
    emitCopyReference(ctx, *value, result);
    ctx.jumpTo(end);
    ctx.placeLabel(site.join);
  }
  ctx.emitRuntimeFailure(CompileError::NonExhaustiveMatch, loc,
                         "no case alternative matched the scrutinee");
  ctx.placeLabel(end);
  return Operand(result.cell, Evaluation::Whnf);
}

// -- Declarations and built-ins

Lowered<void> lowerDataDecl(LowerContext &ctx, const DataDecl &d,
                            SourceLocation loc) {
  std::unordered_set<std::string> seen;
  for (const auto &c : d.ctors) {
    if (isReservedCtorTag(c.name)) {
      return fail(
          CompileError::TypeMismatch,
          "data constructor '" + c.name + "' is reserved by the runtime", loc);
    }
    if (!seen.insert(c.name).second) {
      return fail(CompileError::TypeMismatch,
                  "data constructor '" + c.name + "' is declared twice", loc);
    }
  }
  for (const auto &c : d.ctors) {
    CtorInfo info{.name = c.name, .arity = c.argTypes.size(), .fields = {}};
    if (c.isRecord) {
      info.arity = c.fields.size();
      for (const auto &field : c.fields) info.fields.push_back(field.first);
    }
    ctx.state().ctors().declare(std::move(info));
  }
  return {};
}

void installBuiltinCtors(LowerState &state) {
  for (const auto &[tag, arity] : builtinCtors) {
    state.ctors().declare(
        CtorInfo{.name = std::string(tag), .arity = arity, .fields = {}});
  }
}

// -- Read-back

std::optional<Lowered<ValuePtr>> readbackData(Compiler &compiler,
                                              zigzag::CellRef cell) {
  auto &core        = compiler.core();
  const auto &arena = core.arena();
  const auto &dims  = core.dims();
  const auto head   = arena.linked(cell, dims.grab, DimVector::POS);
  if (!head) return std::nullopt;
  auto value  = std::make_shared<Value>();
  value->cell = cell;
  if (readingFields) {
    value->kind = Value::Kind::Thunk;
    return value;
  }
  value->kind           = Value::Kind::Ctor;
  value->tag            = arena.textOf(cell);
  const auto rankLength = arena.asInt64(*head);
  if (!rankLength || *rankLength < 1) {
    return std::unexpected(
        EvalError{.code   = CompileError::UnexpectedToken,
                  .detail = "vlace: a constructed value has no arity header",
                  .loc    = {}});
  }
  const FieldReading reading;
  CellRef previous = *head;
  for (std::int64_t i = 1; i < *rankLength; ++i) {
    const auto slot = arena.linked(previous, dims.step, DimVector::POS);
    if (!slot) {
      return std::unexpected(
          EvalError{.code   = CompileError::UnexpectedToken,
                    .detail = "vlace: a constructed value is missing a field",
                    .loc    = {}});
    }
    auto field = readbackCell(compiler, *slot);
    if (!field) return std::unexpected(std::move(field.error()));
    value->args.push_back(std::move(*field));
    previous = *slot;
  }
  value->ctorArity = value->args.size();
  return value;
}

std::string renderData(Compiler &compiler, const Value &v, int depthBudget) {
  // Lists: render Cons/Nil chains as [a, b, c].
  if (v.tag == nilTag) return "[]";
  if (v.tag == consTag && v.args.size() == 2) {
    std::string out = "[";
    bool first      = true;
    int budget      = depthBudget;
    ValuePtr node   = std::make_shared<Value>(v);
    while (budget-- > 0) {
      if (node->tag == consTag && node->args.size() == 2) {
        if (!first) out += ", ";
        first = false;
        out += compiler.render(node->args[0], depthBudget - 1);
        auto tail = compiler.forceValue(node->args[1]);
        if (!tail) {
          out += ", <error>";
          break;
        }
        node = *tail;
      } else if (node->tag == nilTag) {
        break;
      } else {
        out += " | " + compiler.render(node, depthBudget - 1); // improper tail
        break;
      }
    }
    out += "]";
    return out;
  }
  // Tuples: (a, b, ...).
  if (v.tag == tupleTag) {
    std::string out = "(";
    for (std::size_t k = 0; k < v.args.size(); ++k) {
      if (k != 0) out += ", ";
      out += compiler.render(v.args[k], depthBudget - 1);
    }
    out += ")";
    return out;
  }
  // General constructor: "Ctor a b".
  std::string out = v.tag;
  for (const auto &arg : v.args) {
    out += " " + compiler.render(arg, depthBudget - 1);
  }
  return out;
}

} // namespace xanadu::vlace::lower
