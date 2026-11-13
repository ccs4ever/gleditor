/**
 * @file logic_compile_core.cpp
 * @brief Card P1: predicate frames, clause chains, head unification, body
 * goals, recursion, the solution driver and the query entry.
 *
 * Shape (spike P0, design (c)): a predicate (name, arity) is one VM frame whose
 * entry is `ChoiceDepth -> barrier` followed by its clauses as a `Choice` retry
 * chain in rank order, the last clause without a `Choice`. Each clause is a
 * fresh renaming per activation (`MakeVar` per variable), head arguments are
 * built with `MakeTerm` and `Unify`'d against the argument ports, body goals
 * are `InvokeFrame`s in spin order, and the clause ends in `ReturnFrame`.
 * Backtracking is the VM's `Fail`, which rolls the arena back to the clause's
 * `Choice` mark, so the arena trail is the logic trail.
 *
 * The query is itself a zero-arity frame. A frame, unlike a bare cursor, owns
 * its locals as per-activation backing cells that the VM erases on return, so
 * releasing the arena mark that covers the compiled graph leaves no stale
 * reference in the VM's maps.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gleditor/logging.hpp>

#include "common/xanadu/vortex/logic_compile.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace zigzag::vortex::logic {

namespace {

// The query lowers to a frame under this name. No clause can carry it: the
// parser cannot produce a functor beginning with '<' and a space-free tail.
constexpr std::string_view kQueryName = "<query>";

// Nesting depth of simultaneously active frames one solve may reach. The old
// solver stopped silently at 1000, which truncated solutions; recursion here is
// bounded by the cycle budget, and this cap only keeps a runaway frame stack
// from exhausting memory before the budget does.
constexpr std::size_t kLogicMaxFrameDepth = 1'000'000;

// Frame size caps. All of a predicate's clauses share one frame, so a fact
// table of thousands of clauses or a large list literal puts thousands of
// locals in one descriptor; the VM defaults (4096 locals, 8192 records) are
// sized for hand-written graphs, and the old solver had no such cap. Locals
// are pooled and the unify flag is shared, so these only bound genuinely many
// distinct clause variables.
constexpr std::size_t kLogicMaxLocalsPerFrame    = 65'536;
constexpr std::size_t kLogicMaxDescriptorRecords = 2 * kLogicMaxLocalsPerFrame;

// The VM reports a spent cycle budget only as this text (ExecutionResult has
// no structured field for it), so the driver matches it by name here.
constexpr std::string_view kVmCycleBudgetMessage = "Exceeded max cycles";

// Goals the old solver answered with a C++ arm checked BEFORE any clause (see
// e9090999^ vortex_stdlib.cpp, solveQueryHelper). Each is owned by a later
// card (P2: control and arithmetic, P3/P4: relations). Until its card lowers
// it, a goal naming one must REFUSE by name, not fail like an unknown user
// predicate, or the query silently has no answers. Delete an entry when its
// card lands; the dispatch then reaches that card's emitter first. `=` is not
// listed: it is a stdlib clause predicate that P1 lowers itself.
struct OwnedGoal {
  std::string_view name;
  std::size_t arity;
};
// One row per line so a card deleting its rows touches nothing else.
// clang-format off
constexpr std::array kOwnedGoals{
    OwnedGoal{"vortex_module", 1},
    OwnedGoal{"stdlib_module", 1},
    OwnedGoal{"vortex_function", 1},
    OwnedGoal{"vortex_function", 2},
    OwnedGoal{"vortex_function", 3},
    OwnedGoal{"stdlib_function", 1},
    OwnedGoal{"stdlib_function", 2},
    OwnedGoal{"stdlib_function", 3},
    OwnedGoal{"vortex_instruction", 3},
    OwnedGoal{"vortex_contract", 3},
    OwnedGoal{"vortex_param", 4},
    OwnedGoal{"setting", 2},
    OwnedGoal{"setting_shape", 2},
    OwnedGoal{"setting_default", 2},
    OwnedGoal{"transclude", 3},
    OwnedGoal{"xanalink", 3},
    OwnedGoal{"cell_value", 2},
    OwnedGoal{"cell_link", 4},
    OwnedGoal{"cell_span", 2},
    OwnedGoal{"bridge_edge", 2},
    OwnedGoal{"bridge_edge", 3},
};
// clang-format on

[[nodiscard]] bool isOwnedGoal(const PredicateKey &key) {
  return std::ranges::any_of(kOwnedGoals, [&](const OwnedGoal &owned) {
    return owned.name == key.name && owned.arity == key.arity;
  });
}

// Per-label state, kept in a cell the label's Nop op takes as its input so it
// lives in the arena (and so is undone with it) rather than in a side table
// keyed by an address that a later context can reuse.
constexpr std::int64_t kLabelTargeted = 1;
constexpr std::int64_t kLabelPlaced   = 2;
constexpr std::string_view kLabelText = "#LOGIC_LABEL";

// Labels created since the innermost frame compile began, so a frame that
// finishes with a targeted label unplaced can be refused. Frames nest (a call
// site compiles its callee mid-body), hence a stack discipline by size mark.
std::vector<CellRef> &labelBook() {
  thread_local std::vector<CellRef> book;
  return book;
}

[[nodiscard]] zigzag::ArenaManifold &arenaOf(const CompileContext &ctx) {
  return ctx.core.arena();
}

[[nodiscard]] std::int64_t asOpId(CellRef op) {
  return static_cast<std::int64_t>(op);
}

// A static cell carrying @p op as an Int64: how Choice, Jump and Branch name
// their target (the VM reads the value, not the cell).
[[nodiscard]] CellRef opIdCell(CompileContext &ctx, CellRef op) {
  return arenaOf(ctx).makeScalarCell(asOpId(op));
}

void attach(CompileContext &ctx, Chain &chain, CellRef op) {
  if (!chain.head) chain.head = op;
  if (chain.tail) {
    zigzag::expectWritten(arenaOf(ctx).link(*chain.tail, ctx.core.dims().spin,
                                            DimVector::POS, op));
  }
  chain.tail = op;
}

[[nodiscard]] std::int64_t labelFlags(const CompileContext &ctx,
                                      const Label &label) {
  const auto inputs = ctx.core.inputsOf(label.op);
  if (inputs.empty())
    throw LogicRefusal("logic label is not a label created by newLabel");
  return arenaOf(ctx).asInt64(inputs.front()).value_or(0);
}

void setLabelFlags(CompileContext &ctx, const Label &label,
                   std::int64_t flags) {
  const auto inputs = ctx.core.inputsOf(label.op);
  if (inputs.empty())
    throw LogicRefusal("logic label is not a label created by newLabel");
  zigzag::expectWritten(
      arenaOf(ctx).setValueBits(inputs.front(), xanadu::ValueKind::Int64,
                                static_cast<std::uint64_t>(flags)));
}

void markTargeted(CompileContext &ctx, const Label &label) {
  setLabelFlags(ctx, label, labelFlags(ctx, label) | kLabelTargeted);
}

// Refuses a frame that ends with a targeted label nobody placed, then forgets
// the labels this frame created.
void settleLabels(const CompileContext &ctx, const std::size_t bookMark,
                  const PredicateKey &frame) {
  auto &book = labelBook();
  for (std::size_t i = bookMark; i < book.size(); ++i) {
    const Label label{book[i]};
    const auto flags = labelFlags(ctx, label);
    if ((flags & kLabelTargeted) != 0 && (flags & kLabelPlaced) == 0) {
      book.resize(bookMark);
      throw LogicRefusal("logic label targeted but never placed in " +
                         frame.display());
    }
  }
  book.resize(bookMark);
}

// The shared Fail op of a frame. A Fail inside an activation frame with no
// spin successor is refused by the VM before it runs ("body ended without
// ReturnFrame"), so it gets a successor that is never reached.
[[nodiscard]] CellRef makeFailOp(CompileContext &ctx) {
  const CellRef fail = ctx.vm.mintOpcode(OpcodeKind::Fail, "#LOGIC_FAIL");
  const CellRef tail = ctx.vm.mintOpcode(OpcodeKind::Nop, "#LOGIC_FAIL_TAIL");
  zigzag::expectWritten(
      arenaOf(ctx).link(fail, ctx.core.dims().spin, DimVector::POS, tail));
  return fail;
}

CellRef emitJumpToOp(CompileContext &ctx, Chain &chain, CellRef op) {
  const std::array<CellRef, 1> inputs{opIdCell(ctx, op)};
  return appendOp(ctx, chain, OpcodeKind::Jump, inputs);
}

// Raises the frame limits for one compile or solve and puts them back. Raises
// only: a caller that configured more keeps it.
class FrameLimitScope {
public:
  explicit FrameLimitScope(VortexVM &vm) : vm_(vm), saved_(vm.frameLimits()) {
    FrameLimits raised = saved_;
    raised.maxDepth    = std::max(raised.maxDepth, kLogicMaxFrameDepth);
    // Never decremented by the VM, so a long-lived VM would otherwise run out
    // across many solves.
    raised.maxBackingCells = std::numeric_limits<std::size_t>::max();
    raised.maxLocalsPerFrame =
        std::max(raised.maxLocalsPerFrame, kLogicMaxLocalsPerFrame);
    raised.maxDescriptorRecords =
        std::max(raised.maxDescriptorRecords, kLogicMaxDescriptorRecords);
    vm_.setFrameLimits(raised);
  }
  ~FrameLimitScope() { vm_.setFrameLimits(saved_); }
  FrameLimitScope(const FrameLimitScope &)            = delete;
  FrameLimitScope &operator=(const FrameLimitScope &) = delete;

private:
  VortexVM &vm_;
  FrameLimits saved_;
};

[[nodiscard]] CellRef scratchOk(CompileContext &ctx, FrameScope &scope) {
  if (!scope.scratchOk) scope.scratchOk = newLocal(ctx, scope);
  return *scope.scratchOk;
}

[[nodiscard]] CellRef takeTemp(CompileContext &ctx, FrameScope &scope) {
  if (scope.freeTemps.empty()) return newLocal(ctx, scope);
  const CellRef taken = scope.freeTemps.back();
  scope.freeTemps.pop_back();
  return taken;
}

void freeTemp(FrameScope &scope, CellRef temp) {
  scope.freeTemps.push_back(temp);
}

[[nodiscard]] bool isQueryScope(const FrameScope &scope) {
  return scope.predicate.name == kQueryName;
}

// True when @p term reaches itself. Unification has no occurs check, so
// `X = f(X)` binds a rational tree; rendering one recurses without end.
// Iterative on purpose: a 100,000-element acyclic list is a term depth of
// 100,000, which a recursive walk would take down the stack with.
[[nodiscard]] bool isCyclic(const VortexStdLib &stdlib, CellRef term,
                            std::unordered_set<CellRef> &path,
                            std::unordered_set<CellRef> &cleared) {
  struct Pending {
    CellRef cell;
    std::vector<CellRef> args;
    std::size_t next;
  };
  std::vector<Pending> stack;
  // Returns true when @p ref closes a cycle; otherwise opens a stack entry for
  // a term not yet proven acyclic.
  const auto enter = [&](CellRef ref) {
    const auto actual = stdlib.deref(ref);
    if (!actual || cleared.contains(*actual)) return false;
    if (!path.insert(*actual).second) return true;
    stack.push_back(Pending{*actual, stdlib.argumentsOf(*actual), 0});
    return false;
  };
  if (enter(term)) return true;
  while (!stack.empty()) {
    Pending &top = stack.back();
    if (top.next == top.args.size()) {
      path.erase(top.cell);
      cleared.insert(top.cell);
      stack.pop_back();
      continue;
    }
    const CellRef arg = top.args[top.next++];
    if (enter(arg)) return true;
  }
  return false;
}

constexpr std::string_view kCyclicText = "<cyclic>";

// Depth-first variables of @p term as the old solver's collectVariables found
// them: the cell as met (not its master), first occurrence per master, cycle
// safe.
// Explicit stack, not recursion: a list query is as deep as its length.
void collectVariables(const VortexStdLib &stdlib, CellRef term,
                      std::vector<CellRef> &vars,
                      std::unordered_set<CellRef> &seenVars,
                      std::unordered_set<CellRef> &visitedTerms) {
  struct Pending {
    std::vector<CellRef> args;
    std::size_t next;
  };
  std::vector<Pending> stack;
  // Visits @p ref in the order the recursive walk did: a variable is recorded
  // at its first occurrence, a compound opens its arguments.
  const auto enter = [&](CellRef ref) {
    const auto actual = stdlib.deref(ref);
    if (!actual) return;
    if (stdlib.isVar(*actual)) {
      if (seenVars.insert(*actual).second) vars.push_back(ref);
      return;
    }
    if (!visitedTerms.insert(*actual).second) return;
    stack.push_back(Pending{stdlib.argumentsOf(*actual), 0});
  };
  enter(term);
  while (!stack.empty()) {
    Pending &top = stack.back();
    if (top.next == top.args.size()) {
      stack.pop_back();
      continue;
    }
    const CellRef arg = top.args[top.next++];
    enter(arg);
  }
}

[[nodiscard]] std::vector<CellRef> clausesOf(const CompileContext &ctx,
                                             const PredicateKey &key) {
  auto &arena = arenaOf(ctx);
  std::vector<CellRef> clauses;
  for (const CellRef pred : ctx.candidatePredicates) {
    if (pred == noCell || !arena.contains(pred)) continue;
    if (arena.textOf(pred) != key.name) continue;
    auto clause = arena.linked(pred, ctx.core.dims().clause, DimVector::POS);
    std::size_t limit = arena.cellCount() + 1;
    while (clause && limit-- > 0) {
      arena.consumeWork();
      const auto head =
          arena.linked(*clause, ctx.core.dims().grab, DimVector::POS);
      // A head of another functor or arity never unifies with the goal, so it
      // is dropped here instead of compiled to a certain failure.
      if (head && ctx.stdlib.functorOf(*head) == key.name &&
          ctx.stdlib.argumentsOf(*head).size() == key.arity) {
        clauses.push_back(*clause);
      }
      clause = arena.linked(*clause, ctx.core.dims().clause, DimVector::POS);
    }
  }
  return clauses;
}

} // namespace

// Test seam: the (name, arity) of every goal kOwnedGoals refuses, in table
// order. Declared by the tests that sweep the table; the header is frozen, so
// it is not declared in logic_compile.hpp.
std::vector<std::pair<std::string_view, std::size_t>> ownedGoalSignatures() {
  std::vector<std::pair<std::string_view, std::size_t>> out;
  out.reserve(kOwnedGoals.size());
  for (const OwnedGoal &owned : kOwnedGoals)
    out.emplace_back(owned.name, owned.arity);
  return out;
}

// -- Emission primitives ------------------------------------------------------

CellRef appendOp(CompileContext &ctx, Chain &chain, OpcodeKind kind,
                 std::span<const CellRef> inputs,
                 std::span<const CellRef> outputs) {
  auto &arena = arenaOf(ctx);
  for (const CellRef cell : inputs)
    if (!arena.contains(cell))
      throw LogicRefusal("logic op input is not a cell");
  for (const CellRef cell : outputs)
    if (!arena.contains(cell))
      throw LogicRefusal("logic op output is not a cell");
  const CellRef op = ctx.vm.mintOpcode(kind);
  for (const CellRef cell : inputs) ctx.core.bindInput(op, cell);
  for (const CellRef cell : outputs) ctx.core.bindOutput(op, cell);
  attach(ctx, chain, op);
  return op;
}

CellRef newLocal(CompileContext &ctx, FrameScope &scope) {
  const CellRef local = arenaOf(ctx).makeCell();
  scope.locals.push_back(local);
  return local;
}

CellRef emitConstant(CompileContext &ctx, Chain &,
                     const ScalarConstant &value) {
  auto &arena = arenaOf(ctx);
  return std::visit(
      [&arena](const auto &held) -> CellRef {
        using T = std::remove_cvref_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return arena.makeCell(held);
        } else {
          return arena.makeScalarCell(held);
        }
      },
      value);
}

std::optional<CellRef> argumentPort(const FrameScope &scope,
                                    std::size_t index) {
  if (index >= scope.predicate.arity || index >= scope.locals.size())
    return std::nullopt;
  return scope.locals[index];
}

bool bindArgumentPort(FrameScope &scope, CellRef variable, std::size_t index) {
  const auto port = argumentPort(scope, index);
  if (!port)
    throw LogicRefusal("argument port " + std::to_string(index) +
                       " is past the arity of " + scope.predicate.display());
  return scope.variableSlots.emplace(variable, *port).second;
}

CellRef variableSlot(CompileContext &ctx, FrameScope &scope, Chain &chain,
                     CellRef variable) {
  const CellRef master = ctx.stdlib.deref(variable).value_or(variable);
  if (const auto it = scope.variableSlots.find(master);
      it != scope.variableSlots.end())
    return it->second;
  if (isQueryScope(scope)) {
    // Query variables are the real cells; bindings are made on them in place.
    scope.variableSlots.emplace(master, master);
    return master;
  }
  const CellRef slot = newLocal(ctx, scope);
  const std::array<CellRef, 1> outputs{slot};
  appendOp(ctx, chain, OpcodeKind::MakeVar, {}, outputs);
  scope.variableSlots.emplace(master, slot);
  return slot;
}

namespace {

// emitTerm, also reporting whether the returned slot is a temporary this call
// made (so its consumer may hand it back to the free list).
CellRef emitTermTracked(CompileContext &ctx, FrameScope &scope, Chain &chain,
                        CellRef term, bool &fresh) {
  fresh             = false;
  const auto actual = ctx.stdlib.deref(term);
  if (!actual) throw LogicRefusal("logic term is not a cell");
  if (ctx.stdlib.isVar(*actual))
    return variableSlot(ctx, scope, chain, *actual);
  // Query terms already exist and are shared with the caller exactly as the
  // old solver shared them; only a clause is renamed per activation.
  if (isQueryScope(scope)) return *actual;

  auto &arena      = arenaOf(ctx);
  const auto parts = ctx.stdlib.argumentsOf(*actual);
  if (parts.empty()) {
    // A fresh static copy: binding a variable to a leaf splices it into the
    // leaf's clone rank, and the clause's own cell must stay unbound-clean.
    switch (arena.valueKindOf(*actual)) {
    case xanadu::ValueKind::Int64:
      if (const auto v = arena.asInt64(*actual))
        return emitConstant(ctx, chain, *v);
      break;
    case xanadu::ValueKind::Double:
      if (const auto v = arena.asDouble(*actual))
        return emitConstant(ctx, chain, *v);
      break;
    case xanadu::ValueKind::Bool:
      if (const auto v = arena.asBool(*actual))
        return emitConstant(ctx, chain, *v);
      break;
    default:
      break;
    }
    return emitConstant(ctx, chain, arena.textOf(*actual));
  }

  std::vector<CellRef> inputs;
  std::vector<CellRef> consumed;
  inputs.reserve(parts.size() + 1);
  inputs.push_back(emitConstant(ctx, chain, arena.textOf(*actual)));
  for (const CellRef part : parts) {
    bool partFresh = false;
    inputs.push_back(emitTermTracked(ctx, scope, chain, part, partFresh));
    if (partFresh) consumed.push_back(inputs.back());
  }
  const CellRef built = takeTemp(ctx, scope);
  const std::array<CellRef, 1> outputs{built};
  appendOp(ctx, chain, OpcodeKind::MakeTerm, inputs, outputs);
  // The op has read its arguments; later terms may reuse their slots.
  for (const CellRef spent : consumed) freeTemp(scope, spent);
  fresh = true;
  return built;
}

} // namespace

CellRef emitTerm(CompileContext &ctx, FrameScope &scope, Chain &chain,
                 CellRef term) {
  bool fresh = false;
  return emitTermTracked(ctx, scope, chain, term, fresh);
}

void emitUnifyOrFail(CompileContext &ctx, FrameScope &scope, Chain &chain,
                     CellRef a, CellRef b) {
  const CellRef ok = scratchOk(ctx, scope);
  const std::array<CellRef, 2> inputs{a, b};
  const std::array<CellRef, 1> outputs{ok};
  appendOp(ctx, chain, OpcodeKind::Unify, inputs, outputs);
  emitFailUnless(ctx, scope, chain, ok);
}

void emitFailUnless(CompileContext &ctx, FrameScope &scope, Chain &chain,
                    CellRef ok) {
  // BranchFalseCell names its target as a cell, so no scalar is minted and no
  // separate Not is needed; the effect is `Not; Branch -> failOp`.
  const std::array<CellRef, 2> inputs{ok, scope.failOp};
  appendOp(ctx, chain, OpcodeKind::BranchFalseCell, inputs);
}

// -- Labels -------------------------------------------------------------------

Label newLabel(CompileContext &ctx) {
  const CellRef op    = ctx.vm.mintOpcode(OpcodeKind::Nop, kLabelText);
  const CellRef state = arenaOf(ctx).makeCell();
  zigzag::expectWritten(
      arenaOf(ctx).setValueBits(state, xanadu::ValueKind::Int64, 0));
  ctx.core.bindInput(op, state);
  labelBook().push_back(op);
  return Label{op};
}

void placeLabel(CompileContext &ctx, Chain &chain, Label label) {
  const auto flags = labelFlags(ctx, label);
  if ((flags & kLabelPlaced) != 0)
    throw LogicRefusal("logic label placed twice");
  setLabelFlags(ctx, label, flags | kLabelPlaced);
  attach(ctx, chain, label.op);
}

CellRef emitChoice(CompileContext &ctx, Chain &chain, Label target) {
  markTargeted(ctx, target);
  const std::array<CellRef, 1> inputs{opIdCell(ctx, target.op)};
  return appendOp(ctx, chain, OpcodeKind::Choice, inputs);
}

CellRef emitJump(CompileContext &ctx, Chain &chain, Label target) {
  markTargeted(ctx, target);
  return emitJumpToOp(ctx, chain, target.op);
}

CellRef emitBranchIf(CompileContext &ctx, Chain &chain, CellRef condition,
                     Label target) {
  markTargeted(ctx, target);
  const std::array<CellRef, 2> inputs{condition, opIdCell(ctx, target.op)};
  return appendOp(ctx, chain, OpcodeKind::Branch, inputs);
}

CellRef emitBranchUnless(CompileContext &ctx, Chain &chain, CellRef condition,
                         Label target) {
  markTargeted(ctx, target);
  const std::array<CellRef, 2> inputs{condition, target.op};
  return appendOp(ctx, chain, OpcodeKind::BranchFalseCell, inputs);
}

// -- Cut ----------------------------------------------------------------------

CellRef emitCut(CompileContext &ctx, FrameScope &scope, Chain &chain) {
  if (!scope.barrier)
    throw LogicRefusal("cut in " + scope.predicate.display() +
                       " has no cut barrier");
  return emitCutTo(ctx, chain, *scope.barrier);
}

NegationScope openNegationScope(CompileContext &ctx, FrameScope &scope,
                                Chain &chain) {
  const CellRef slot = newLocal(ctx, scope);
  const std::array<CellRef, 1> outputs{slot};
  appendOp(ctx, chain, OpcodeKind::ChoiceDepth, {}, outputs);
  // `slot` is the depth BEFORE the caller's `Choice CONT`, so cutting to it
  // (emitCutTo, after the negated goal succeeds) removes CONT too, which is
  // wanted. A `!` inside the negated goal must spare CONT: `\+ (!, fail)`
  // succeeds, because the cut is local to the goal. It therefore cuts to
  // slot + 1, the depth once CONT is pushed.
  const CellRef afterChoice = newLocal(ctx, scope);
  const std::array<CellRef, 2> addInputs{
      slot, emitConstant(ctx, chain, std::int64_t{1})};
  const std::array<CellRef, 1> addOutputs{afterChoice};
  appendOp(ctx, chain, OpcodeKind::Add, addInputs, addOutputs);
  NegationScope negation{.barrier = slot, .outer = scope.barrier};
  scope.barrier = afterChoice;
  return negation;
}

void closeNegationScope(FrameScope &scope, const NegationScope &negation) {
  scope.barrier = negation.outer;
}

CellRef emitCutTo(CompileContext &ctx, Chain &chain, CellRef barrier) {
  const std::array<CellRef, 1> inputs{barrier};
  return appendOp(ctx, chain, OpcodeKind::Cut, inputs);
}

// -- User predicates ----------------------------------------------------------

bool hasUserClauses(const CompileContext &ctx, const PredicateKey &key) {
  // The old solver tried its builtin arms before any clause, so a user clause
  // named like a builtin never replaced it; the same holds here.
  if (isOwnedGoal(key) || isControlBuiltin(key)) return false;
  auto &arena = arenaOf(ctx);
  return std::ranges::any_of(ctx.candidatePredicates, [&](const CellRef pred) {
    return pred != noCell && arena.contains(pred) &&
           arena.textOf(pred) == key.name &&
           arena.linked(pred, ctx.core.dims().clause, DimVector::POS)
               .has_value();
  });
}

void emitNonTermGoal(CompileContext &, FrameScope &, Chain &, CellRef) {
  // The old solver skipped a goal with no functor and went on to the next.
}

void emitUnknownPredicate(CompileContext &ctx, FrameScope &scope, Chain &chain,
                          const PredicateKey &key) {
  if (isOwnedGoal(key))
    refuseUnlowered("P1", key.display() + " awaits P2/P3/P4");
  GLEDITOR_LOG_DEBUG("vortex.logic",
                     "unknown predicate {} fails without an error",
                     key.display());
  emitJumpToOp(ctx, chain, scope.failOp);
}

void emitClause(CompileContext &ctx, FrameScope &scope, Chain &chain,
                CellRef clause, bool last) {
  // scratchOk survives (a frame property); the free list restarts with the
  // renaming, as it always has.
  scope.variableSlots.clear();
  scope.freeTemps.clear();
  auto &arena = arenaOf(ctx);

  std::optional<Label> next;
  if (!last) {
    next = newLabel(ctx);
    emitChoice(ctx, chain, *next);
  }

  const auto head = arena.linked(clause, ctx.core.dims().grab, DimVector::POS);
  if (!head) throw LogicRefusal("clause has no head");
  const auto headArgs = ctx.stdlib.argumentsOf(*head);

  // Plain-variable arguments first: a first-seen one IS its port, which costs
  // nothing, and building a compound argument that mentions it then reads the
  // port directly.
  std::vector<std::size_t> compound;
  for (std::size_t i = 0; i < headArgs.size(); ++i) {
    const auto actual = ctx.stdlib.deref(headArgs[i]);
    if (!actual) throw LogicRefusal("clause head argument is not a cell");
    if (!ctx.stdlib.isVar(*actual)) {
      compound.push_back(i);
      continue;
    }
    if (!bindArgumentPort(scope, *actual, i)) {
      const CellRef slot = variableSlot(ctx, scope, chain, *actual);
      emitUnifyOrFail(ctx, scope, chain, *argumentPort(scope, i), slot);
    }
  }
  for (const std::size_t i : compound) {
    bool fresh = false;
    const CellRef built =
        emitTermTracked(ctx, scope, chain, headArgs[i], fresh);
    emitUnifyOrFail(ctx, scope, chain, *argumentPort(scope, i), built);
    if (fresh) freeTemp(scope, built);
  }

  std::vector<CellRef> goals;
  auto goal = arena.linked(clause, ctx.core.dims().spin, DimVector::POS);
  std::size_t limit = arena.cellCount() + 1;
  while (goal && limit-- > 0) {
    arena.consumeWork();
    goals.push_back(*goal);
    goal = arena.linked(*goal, ctx.core.dims().spin, DimVector::POS);
  }

  // Mint every body variable before the first goal. A first occurrence inside
  // a branch (a negation, a retry loop) would otherwise leave its slot unset
  // on the path that skips the branch.
  std::vector<CellRef> bodyVars;
  std::unordered_set<CellRef> seenVars;
  std::unordered_set<CellRef> visited;
  for (const CellRef g : goals)
    collectVariables(ctx.stdlib, g, bodyVars, seenVars, visited);
  for (const CellRef v : bodyVars) variableSlot(ctx, scope, chain, v);

  for (const CellRef g : goals) emitGoal(ctx, scope, chain, g);

  const auto descriptor = ctx.frames.find(scope.predicate);
  if (descriptor == ctx.frames.end())
    throw LogicRefusal("clause emitted outside a registered frame " +
                       scope.predicate.display());
  const std::array<CellRef, 1> inputs{descriptor->second};
  appendOp(ctx, chain, OpcodeKind::ReturnFrame, inputs);

  if (next) placeLabel(ctx, chain, *next);
}

std::optional<CellRef> compilePredicateFrame(CompileContext &ctx,
                                             const PredicateKey &key) {
  if (const auto it = ctx.frames.find(key); it != ctx.frames.end())
    return it->second;
  if (!hasUserClauses(ctx, key)) return std::nullopt;

  const auto reserved = ctx.vm.reserveFrameDescriptor();
  if (!reserved)
    throw LogicRefusal("cannot reserve a frame for " + key.display() + ": " +
                       reserved.error().message);
  // Registered before the body compiles so a call to itself, or to a
  // predicate that calls back, resolves to this descriptor.
  ctx.frames.emplace(key, *reserved);
  const std::size_t bookMark = labelBook().size();

  try {
    FrameScope scope;
    scope.predicate = key;
    scope.failOp    = makeFailOp(ctx);
    for (std::size_t i = 0; i < key.arity; ++i)
      scope.locals.push_back(arenaOf(ctx).makeCell());
    const CellRef barrier = arenaOf(ctx).makeCell();
    scope.locals.push_back(barrier);
    scope.barrier = barrier;

    Chain chain;
    const std::array<CellRef, 1> barrierOut{barrier};
    appendOp(ctx, chain, OpcodeKind::ChoiceDepth, {}, barrierOut);

    const auto clauses = clausesOf(ctx, key);
    if (clauses.empty()) {
      emitJumpToOp(ctx, chain, scope.failOp);
    }
    for (std::size_t i = 0; i < clauses.size(); ++i)
      emitClause(ctx, scope, chain, clauses[i], i + 1 == clauses.size());
    settleLabels(ctx, bookMark, key);

    FrameDescriptorSpec spec{.entry = chain.head, .locals = scope.locals};
    for (std::size_t i = 0; i < key.arity; ++i)
      spec.argumentPorts.emplace_back(scope.locals[i]);
    const auto finalised = ctx.vm.finalizeFrameDescriptor(*reserved, spec);
    if (!finalised)
      throw LogicRefusal("cannot finish the frame for " + key.display() + ": " +
                         finalised.error().message);
    GLEDITOR_LOG_DEBUG("vortex.logic", "compiled {} with {} clauses, {} locals",
                       key.display(), clauses.size(), scope.locals.size());
  } catch (...) {
    labelBook().resize(std::min(labelBook().size(), bookMark));
    ctx.frames.erase(key);
    throw;
  }
  return *reserved;
}

void emitUserCall(CompileContext &ctx, FrameScope &scope, Chain &chain,
                  const PredicateKey &key, CellRef goal) {
  const auto descriptor = compilePredicateFrame(ctx, key);
  if (!descriptor)
    throw LogicRefusal("no clauses for " + key.display() + " to call");
  std::vector<CellRef> inputs{*descriptor};
  std::vector<CellRef> consumed;
  for (const CellRef arg : ctx.stdlib.argumentsOf(goal)) {
    bool fresh = false;
    inputs.push_back(emitTermTracked(ctx, scope, chain, arg, fresh));
    if (fresh) consumed.push_back(inputs.back());
  }
  // The op after this one is the call's continuation; the caller always
  // appends something (the next goal, ReturnFrame or the query's return).
  appendOp(ctx, chain, OpcodeKind::InvokeFrame, inputs);
  for (const CellRef spent : consumed) freeTemp(scope, spent);
}

// -- Query lowering -----------------------------------------------------------

LoweredQuery lowerQuery(CompileContext &ctx, std::span<const CellRef> goals) {
  const FrameLimitScope limits(ctx.vm);
  const PredicateKey key{std::string(kQueryName), 0};

  LoweredQuery lowered;
  std::vector<CellRef> vars;
  std::unordered_set<CellRef> seen;
  std::unordered_set<CellRef> visited;
  for (const CellRef goal : goals)
    collectVariables(ctx.stdlib, goal, vars, seen, visited);
  for (const CellRef v : vars) {
    const CellRef master = ctx.stdlib.deref(v).value_or(v);
    lowered.variables.emplace_back(v, master);
  }

  const auto reserved = ctx.vm.reserveFrameDescriptor();
  if (!reserved)
    throw LogicRefusal("cannot reserve the query frame: " +
                       reserved.error().message);
  const std::size_t bookMark = labelBook().size();

  FrameScope scope;
  scope.predicate       = key;
  scope.failOp          = makeFailOp(ctx);
  const CellRef barrier = arenaOf(ctx).makeCell();
  scope.locals.push_back(barrier);
  scope.barrier = barrier;
  for (const auto &[original, master] : lowered.variables)
    scope.variableSlots.emplace(master, master);

  Chain body;
  const std::array<CellRef, 1> barrierOut{barrier};
  appendOp(ctx, body, OpcodeKind::ChoiceDepth, {}, barrierOut);
  try {
    for (const CellRef goal : goals) emitGoal(ctx, scope, body, goal);
    const std::array<CellRef, 1> self{*reserved};
    appendOp(ctx, body, OpcodeKind::ReturnFrame, self);
    settleLabels(ctx, bookMark, key);
  } catch (...) {
    labelBook().resize(std::min(labelBook().size(), bookMark));
    throw;
  }

  const FrameDescriptorSpec spec{.entry = body.head, .locals = scope.locals};
  const auto finalised = ctx.vm.finalizeFrameDescriptor(*reserved, spec);
  if (!finalised)
    throw LogicRefusal("cannot finish the query frame: " +
                       finalised.error().message);

  Chain entry;
  const std::array<CellRef, 1> call{*reserved};
  lowered.entry  = appendOp(ctx, entry, OpcodeKind::InvokeFrame, call);
  lowered.haltOp = appendOp(ctx, entry, OpcodeKind::Halt);
  lowered.failOp = scope.failOp;
  return lowered;
}

// -- Solution driver ----------------------------------------------------------

struct SolutionDriver::Impl {
  Impl(CompileContext &context, const LoweredQuery &lowered)
      : ctx(context), query(lowered), limits(context.vm),
        mark(context.core.arena().mark()), baseDepth(context.vm.choiceDepth()),
        startCycles(context.vm.totalCycles()),
        sentinel(context.vm.mintOpcode(OpcodeKind::Halt, "#LOGIC_EXHAUSTED")),
        cursor(context.vm.spawnCursor(lowered.entry, "logic.query")) {}

  CompileContext &ctx;
  LoweredQuery query;
  FrameLimitScope limits;
  zigzag::Mark mark;
  std::size_t baseDepth;
  std::uint64_t startCycles;
  CellRef sentinel;
  CellRef cursor;
  bool started{false};
  bool finished{false};
};

SolutionDriver::SolutionDriver(CompileContext &ctx, const LoweredQuery &query)
    : impl_(std::make_unique<Impl>(ctx, query)) {}

SolutionDriver::~SolutionDriver() {
  if (!impl_) return;
  auto &vm = impl_->ctx.vm;
  try {
    // Whatever alternatives remain are abandoned; this also pops the sentinel
    // when the search stopped early.
    if (vm.choiceDepth() > impl_->baseDepth) vm.cut(impl_->baseDepth);
    vm.setCursorOpcode(impl_->cursor, std::nullopt);
  } catch (...) {
    GLEDITOR_LOG_DEBUG("vortex.logic", "driver cleanup could not close cursor");
  }
  impl_->ctx.core.arena().release(impl_->mark);
  vm.pruneAssemblyState();
}

DriveOutcome SolutionDriver::next() {
  auto &impl = *impl_;
  auto &vm   = impl.ctx.vm;
  if (impl.finished) return DriveOutcome::Exhausted;

  if (!impl.started) {
    impl.started = true;
    // The sentinel sits beneath the query's own choices: when the search runs
    // dry, backtracking lands on it (a Halt) instead of failing outright.
    vm.pushChoicePoint(impl.cursor, impl.sentinel);
  } else {
    // Resume is Icon's: land on a Fail and let backtracking find the next
    // alternative.
    vm.setCursorOpcode(impl.cursor, impl.query.failOp);
  }

  const std::uint64_t used = vm.totalCycles() - impl.startCycles;
  if (used >= impl.ctx.maxCycles) return DriveOutcome::BudgetSpent;
  const std::size_t remaining =
      impl.ctx.maxCycles - static_cast<std::size_t>(used);

  const auto result = vm.run(impl.cursor, remaining);
  if (!result.success) {
    if (result.errorMessage == kVmCycleBudgetMessage)
      return DriveOutcome::BudgetSpent;
    throw std::runtime_error("Prolog resolution failed: " +
                             result.errorMessage);
  }
  const auto pc = vm.getCursorOpcode(impl.cursor);
  if (pc == impl.query.haltOp) return DriveOutcome::Solution;
  if (pc == impl.sentinel) {
    impl.finished = true;
    return DriveOutcome::Exhausted;
  }
  throw std::runtime_error("Prolog resolution stopped at an unexpected op");
}

LogicSolution SolutionDriver::solution() const {
  auto &ctx   = impl_->ctx;
  auto &arena = ctx.core.arena();
  LogicSolution sol;
  for (const auto &[original, master] : impl_->query.variables) {
    arena.consumeWork();
    const auto value = ctx.stdlib.deref(original);
    if (!value) continue;
    sol.bindings.emplace_back(original, *value);
    sol.varMap[original] = *value;
    std::string name;
    const auto nameCell =
        arena.linked(original, ctx.core.dims().name, DimVector::POS);
    if (nameCell && arena.contains(*nameCell)) {
      name = arena.textOf(*nameCell);
    } else {
      name = "_G" + std::to_string(original);
    }
    std::unordered_set<CellRef> path;
    std::unordered_set<CellRef> cleared;
    if (isCyclic(ctx.stdlib, original, path, cleared)) {
      sol.formatted[name] = std::string(kCyclicText);
    } else if (const auto rendered = ctx.stdlib.renderTerm(original)) {
      sol.formatted[name] = *rendered;
    }
  }
  return sol;
}

void SolutionDriver::stop() {
  auto &impl = *impl_;
  if (impl.ctx.vm.choiceDepth() > impl.baseDepth)
    impl.ctx.vm.cut(impl.baseDepth);
  impl.finished = true;
}

namespace {

// Releases the compiled graph and what the search left, after the driver has
// closed its cursor, and tells the VM which op cells no longer exist.
class CompileArea {
public:
  explicit CompileArea(CompileContext &ctx)
      : ctx_(ctx), mark_(ctx.core.arena().mark()) {}
  ~CompileArea() {
    ctx_.core.arena().release(mark_);
    ctx_.vm.pruneAssemblyState();
  }
  CompileArea(const CompileArea &)            = delete;
  CompileArea &operator=(const CompileArea &) = delete;

private:
  CompileContext &ctx_;
  zigzag::Mark mark_;
};

} // namespace

bool compileAndRunQuery(
    CompileContext &ctx, std::span<const CellRef> goals,
    const std::function<bool(const LogicSolution &)> &onSolution,
    std::size_t maxSolutions) {
  if (goals.empty()) return true;
  if (maxSolutions == 0) return false;

  const FrameLimitScope limits(ctx.vm);
  const CompileArea area(ctx);
  // A provider is only needed when some relation is registered; with none, no
  // RelationNext can be emitted and the VM hook stays untouched.
  if (ctx.relations.size() != 0) installRelationProvider(ctx);

  const LoweredQuery query = lowerQuery(ctx, goals);
  SolutionDriver driver(ctx, query);
  std::size_t reported = 0;
  for (;;) {
    switch (driver.next()) {
    case DriveOutcome::Exhausted:
      return true;
    case DriveOutcome::BudgetSpent:
      throw std::runtime_error(
          "Prolog resolution exceeded its cycle budget of " +
          std::to_string(ctx.maxCycles) + " after " + std::to_string(reported) +
          " solutions");
    case DriveOutcome::Solution:
      break;
    }
    ++reported;
    const bool keepGoing = !onSolution || onSolution(driver.solution());
    if (!keepGoing) {
      driver.stop();
      return false;
    }
    if (reported >= maxSolutions) {
      driver.stop();
      return true;
    }
  }
}

} // namespace zigzag::vortex::logic
