/**
 * @file logic_compile.hpp
 * @brief The shared surface of Prolog/Vlog resolution lowered to Vortex graphs.
 *
 * Design (c) of the P0 logic spike: each predicate (name, arity) compiles to a
 * VM frame whose entry is a `Choice` retry chain over its clauses; body goals
 * compile inline; the extensional builtins (data that lives in C++ state) are
 * read through one accessor opcode, `RelationNext`, by a retry loop that is
 * itself graph. A solution driver runs the compiled query on the VM.
 *
 * This header is the only place the cards P1-P4 meet. Every function declared
 * here is defined by exactly one card, in that card's own file, so no card
 * edits another's file or this header. Until its card lands, a definition is a
 * stub that throws LogicRefusal carrying the library's existing "unavailable"
 * text.
 *
 * Emission convention: goals are emitted FORWARD into a Chain. An emitter
 * appends ops to the chain's tail; a failing test branches to
 * FrameScope::failOp; success falls through to whatever is appended next.
 * Jump targets are Labels, which may be placed before or after the ops that
 * target them (see newLabel).
 *
 * Ownership of each declaration is the "Owner:" line above it.
 */
#ifndef COMMON_XANADU_VORTEX_LOGIC_COMPILE_HPP
#define COMMON_XANADU_VORTEX_LOGIC_COMPILE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace zigzag::vortex::logic {

/// The text VortexStdLib::solve has always thrown; stubs extend it with the
/// card that owes the form, never replace it.
inline constexpr std::string_view kLogicUnavailable =
    "Vortex logic execution is unavailable: VM resolution lowering is not "
    "implemented";

/// A refusal to lower or run a logic form. Derives from std::logic_error so
/// callers that already catch the library's refusal keep catching it.
class LogicRefusal : public std::logic_error {
public:
  using std::logic_error::logic_error;
};

/// Throws LogicRefusal "<kLogicUnavailable> (<owner card>: <what>)".
[[noreturn]] void refuseUnlowered(std::string_view ownerCard,
                                  std::string_view what);

/// A predicate is identified by functor text and arity. The old solver ignored
/// arity; the compiled one cannot, because arity fixes the frame's ports.
struct PredicateKey {
  std::string name;
  std::size_t arity{};
  [[nodiscard]] bool operator==(const PredicateKey &) const  = default;
  [[nodiscard]] auto operator<=>(const PredicateKey &) const = default;
  /// "name/arity", for refusals and logs.
  [[nodiscard]] std::string display() const;
};

struct PredicateKeyHash {
  [[nodiscard]] std::size_t operator()(const PredicateKey &k) const noexcept;
};

// -- Relations (extensional builtins) ----------------------------------------

/// What a provider is asked: the RelationNext opcode's inputs after the key.
struct RelationRequest {
  /// The goal's argument cells, not dereferenced. A provider derefs them:
  /// unbound = wildcard, bound = filter or point lookup, an ill-typed bound
  /// argument = exhausted.
  std::span<const CellRef> arguments;
  /// First candidate to consider. Opaque to the graph; 0 on the first call.
  std::size_t position{};
  /// Candidate bound fixed by the first call (the arena cellCount snapshot),
  /// so a scan that mints answer cells does not chase its own tail. Nullopt on
  /// the first call, which means "take the snapshot now"; an empty relation
  /// then answers with a present limit of 0, which is not the same thing.
  std::optional<std::size_t> limit;
};

/// One matching tuple. `cells` holds exactly one fresh answer cell per
/// argument (arity() of them); the graph unifies each with its argument.
struct RelationAnswer {
  std::size_t nextPosition{};
  std::size_t limit{};
  std::vector<CellRef> cells;
};

/// A read-only accessor over host state. It does no resolution: it returns the
/// first matching tuple at or after request.position and nothing else.
class RelationProvider {
public:
  virtual ~RelationProvider()                              = default;
  [[nodiscard]] virtual std::size_t arity() const noexcept = 0;
  /// Nullopt means exhausted.
  [[nodiscard]] virtual std::optional<RelationAnswer>
  next(const RelationRequest &request) = 0;
};

/// name/arity -> provider. Filled by registerStdlibRelations (P3) and
/// registerArenaRelations (P4); read by the dispatch and by
/// installRelationProvider.
class RelationRegistry {
public:
  /// Registers @p provider under @p name and provider->arity(). A second
  /// registration of the same key replaces the first (later card wins).
  RelationRegistry *add(std::string name,
                        std::unique_ptr<RelationProvider> provider) {
    PredicateKey key{std::move(name), provider->arity()};
    providers_[std::move(key)] = std::move(provider);
    return this;
  }
  [[nodiscard]] RelationProvider *find(const PredicateKey &key) const {
    const auto it = providers_.find(key);
    return it == providers_.end() ? nullptr : it->second.get();
  }
  [[nodiscard]] std::size_t size() const noexcept { return providers_.size(); }

private:
  std::map<PredicateKey, std::unique_ptr<RelationProvider>> providers_;
};

// -- Compile context ----------------------------------------------------------

/// Everything the lowering reads of one VortexStdLib, built by
/// VortexStdLib::logicContext (logic_solve.cpp). Ops are minted only through
/// `vm`, the stdlib's own VM: a temporary VM would disable the core's write
/// guard (spike R11).
struct CompileContext {
  VortexCore &core;
  VortexVM &vm;
  VortexStdLib &stdlib;
  /// Null when no store is bound; the store providers then answer "no
  /// solutions" or walk home, as the old solver did.
  const xanadu::Store *store{nullptr};
  /// Stdlib predicate cells first, in registration order, then the caller's
  /// custom predicates: the candidatePreds order of the old solver. Custom
  /// predicates enter only here and are matched to a goal by NAME, as
  /// `core.arena().textOf(pred) == functor`.
  std::vector<CellRef> candidatePredicates;
  /// VM cycle budget for one solve. Real, not the 100000 vm.run default.
  std::size_t maxCycles{};
  RelationRegistry relations;
  /// Compiled predicate frames. Entry-first protocol: a predicate's frame
  /// descriptor is registered here BEFORE its body is compiled, so recursion
  /// and mutual recursion can refer to it. Owner: P1.
  std::unordered_map<PredicateKey, CellRef, PredicateKeyHash> frames;
};

/// Forward-emission cursor. Empty until the first op is appended.
struct Chain {
  std::optional<CellRef> head;
  std::optional<CellRef> tail;
};

/// A jump target. See newLabel for when it may be placed.
struct Label {
  CellRef op{};
};

/// The predicate frame being compiled, as seen by clause and goal emitters.
struct FrameScope {
  PredicateKey predicate;
  /// The shared Fail op: a failing test branches here.
  CellRef failOp{};
  /// Frame-local slots. Argument port i is `locals[i]` (read it with
  /// argumentPort); variables, temporaries and the barrier follow.
  std::vector<CellRef> locals;
  /// Cut barrier slot, written by `ChoiceDepth`. P1 sets it at every predicate
  /// frame entry AND in the query-level scope that lowerQuery builds, so it is
  /// engaged whenever a card sees a scope. P2 never reads it: use emitCut,
  /// openNegationScope and closeNegationScope.
  std::optional<CellRef> barrier;
  /// Dereferenced clause/query variable cell -> its local slot. emitClause
  /// clears it on entry (every clause is a fresh renaming per activation) and
  /// nothing else clears it.
  std::unordered_map<CellRef, CellRef> variableSlots;
  /// The frame's one shared unify-result local: every head unify writes its
  /// Bool here and the very next branch consumes it, so one slot serves the
  /// whole frame instead of one per unify. Survives emitClause's clear of
  /// variableSlots (it is a frame property, not a clause renaming).
  std::optional<CellRef> scratchOk;
  /// Term temporaries free for reuse: a slot whose value was consumed is
  /// handed back instead of growing the frame. Reset by emitClause together
  /// with variableSlots, because a temporary freed in one clause may still be
  /// live on the other clause's branch.
  std::vector<CellRef> freeTemps;
};

// -- Compilation services (Owner: P1, logic_compile_core.cpp) -----------------
// P2 and P3 emit through these, so they are declared here and not in P2's/P3's
// files.

/// Appends one op to @p chain, spin-linked after the tail, and returns it.
CellRef appendOp(CompileContext &ctx, Chain &chain, OpcodeKind kind,
                 std::span<const CellRef> inputs  = {},
                 std::span<const CellRef> outputs = {});

/// A fresh frame-local slot (temporary) in @p scope.
CellRef newLocal(CompileContext &ctx, FrameScope &scope);

/// A scalar a graph can carry as a constant.
using ScalarConstant = std::variant<std::int64_t, double, std::string, bool>;

/// A static cell holding @p value, usable as an op input anywhere in @p chain.
/// Numbers are scalar cells (makeScalarCell), text is a text cell. Needed for
/// the zero-divisor guard, numeric conversion, boxing an arithmetic result and
/// the relation key and start position.
CellRef emitConstant(CompileContext &ctx, Chain &chain,
                     const ScalarConstant &value);

/// Port @p index of @p scope's frame: `scope.locals[index]`. Nullopt past the
/// predicate's arity.
[[nodiscard]] std::optional<CellRef> argumentPort(const FrameScope &scope,
                                                  std::size_t index);

/// Head-argument rule. A head argument that is a variable seen for the first
/// time IS its port: call this, which records the port as the variable's slot
/// and emits nothing. Returns false (and changes nothing) if the variable
/// already has a slot; the caller then emits `Unify port, variableSlot`. Any
/// other head argument is built with emitTerm and unified with the port. In a
/// body, variableSlot mints a fresh `MakeVar` instead.
bool bindArgumentPort(FrameScope &scope, CellRef variable, std::size_t index);

/// The slot of variable @p variable (deref'd), minting it with `MakeVar` into
/// @p chain on first occurrence.
CellRef variableSlot(CompileContext &ctx, FrameScope &scope, Chain &chain,
                     CellRef variable);

/// Emits ops building @p term (variables from variableSlot, ground parts as
/// static cells, compound parts with MakeTerm, post-order) and returns the
/// slot holding it.
CellRef emitTerm(CompileContext &ctx, FrameScope &scope, Chain &chain,
                 CellRef term);

/// Appends `Unify a, b -> ok; Not; Branch -> failOp`.
void emitUnifyOrFail(CompileContext &ctx, FrameScope &scope, Chain &chain,
                     CellRef a, CellRef b);

/// Appends `Not; Branch -> failOp` on the Bool in @p ok.
void emitFailUnless(CompileContext &ctx, FrameScope &scope, Chain &chain,
                    CellRef ok);

/// A fresh, unplaced jump target. It may be targeted by emitChoice, emitJump,
/// emitBranchIf and emitBranchUnless BEFORE or AFTER it is placed, so forward
/// targets (a clause chain's next alternative) and backward targets (a loop
/// head: `placeLabel; RelationNext ...; Choice -> label`) are both expressible.
/// Placing it appends a landing op to the chain, which the following ops
/// continue from. Placing a label twice, or leaving a targeted label unplaced
/// when the frame is finished, is a LogicRefusal.
Label newLabel(CompileContext &ctx);
void placeLabel(CompileContext &ctx, Chain &chain, Label label);
/// Appends `Choice target`.
CellRef emitChoice(CompileContext &ctx, Chain &chain, Label target);
/// Appends an unconditional `Jump target`.
CellRef emitJump(CompileContext &ctx, Chain &chain, Label target);
/// Appends a `Branch` to @p target taken when the Bool in @p condition is true
/// (emitBranchIf) or false (emitBranchUnless).
CellRef emitBranchIf(CompileContext &ctx, Chain &chain, CellRef condition,
                     Label target);
CellRef emitBranchUnless(CompileContext &ctx, Chain &chain, CellRef condition,
                         Label target);

// -- Cut (Owner: P1) ----------------------------------------------------------
// Semantics, from the old solver (e9090999^ vortex_stdlib.cpp:405, 795, 1371,
// 4331): a clause body's `!` cuts the alternatives of that predicate call (its
// remaining clauses and every choice its earlier goals left) and keeps the
// bindings; the caller's choices survive. A query-level `!` has frame id 1,
// which no clause matches, so it cuts every pending alternative of the query
// to its left and the goals to its right still run. `\+ G` runs G in a nested
// solve with its own cut state, so a `!` inside G cuts only G's alternatives;
// the old code seeds that nested goal's frame id with the NEXT unassigned
// frame id, which collides with the first predicate called inside G, and I
// could not determine whether that was intended. The specified behaviour here
// is the clean one: a cut inside `\+` is local to the negation.

/// Appends `Cut barrier` for the current scope's barrier. A scope without a
/// barrier is a LogicRefusal.
CellRef emitCut(CompileContext &ctx, FrameScope &scope, Chain &chain);

/// What openNegationScope displaced.
struct NegationScope {
  CellRef barrier{};
  std::optional<CellRef> outer;
};

/// For `\+ G`: allocates a fresh slot, appends `ChoiceDepth -> slot`, and makes
/// it @p scope's barrier so that emitGoal(G) lowers a `!` inside G to a cut
/// local to the negation. The caller then appends `Choice CONT`, G, a cut to
/// `.barrier` (emitCutTo) and `Fail`. Restore the outer barrier with
/// closeNegationScope.
NegationScope openNegationScope(CompileContext &ctx, FrameScope &scope,
                                Chain &chain);
void closeNegationScope(FrameScope &scope, const NegationScope &negation);
/// Appends `Cut barrier` for an explicit barrier slot (negation's final cut).
CellRef emitCutTo(CompileContext &ctx, Chain &chain, CellRef barrier);

// -- Query lowering and driving (Owner: P1, logic_compile_core.cpp) -----------

/// A compiled conjunction: run from `entry`; reaching `haltOp` is a solution;
/// failing into `failOp` backtracks.
struct LoweredQuery {
  CellRef entry{};
  CellRef haltOp{};
  CellRef failOp{};
  /// Query variables in the old solver's collectVariables order (depth first
  /// over the goal terms, deref-aware, cycle-safe, anonymous included), with
  /// the cell that holds each at run time.
  std::vector<std::pair<CellRef, CellRef>> variables;
};

/// Compiles @p goals and the transitive closure of predicates reachable from
/// them. An empty @p goals lowers to a query that reaches haltOp at once.
[[nodiscard]] LoweredQuery lowerQuery(CompileContext &ctx,
                                      std::span<const CellRef> goals);

/// What a call to SolutionDriver::next() ended with. Exhaustion and a spent
/// cycle budget are different results on purpose: the old drivers read both as
/// "no more solutions" (spike R10).
enum class DriveOutcome : std::uint8_t {
  Solution,    ///< Bindings are live; read them with SolutionDriver::solution.
  Exhausted,   ///< The search space is finished.
  BudgetSpent, ///< maxCycles ran out first; more solutions may exist.
};

/// Runs a LoweredQuery on ctx.vm. Resume is "fail into the last choice": next()
/// after a Solution backtracks for the following one. Bindings are live only
/// between next() returning Solution and the following next()/stop()/dtor, the
/// contract onSolution has always had. The destructor releases the outer arena
/// mark (so cells minted during the search are gone, as before) and restores
/// the frame limits it raised.
class SolutionDriver {
public:
  SolutionDriver(CompileContext &ctx, const LoweredQuery &query);
  ~SolutionDriver();
  SolutionDriver(const SolutionDriver &)            = delete;
  SolutionDriver &operator=(const SolutionDriver &) = delete;

  [[nodiscard]] DriveOutcome next();
  /// The current solution's bindings/varMap/formatted. Valid after Solution.
  [[nodiscard]] LogicSolution solution() const;
  /// Cuts the remaining search (onSolution returned false or maxSolutions hit).
  void stop();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// The one entry VortexStdLib::solve will call once P1 lands: lower, drive,
/// call @p onSolution per solution while its bindings are live, stop at
/// @p maxSolutions or when it returns false, and throw (std::runtime_error) on
/// a spent budget. Empty @p goals returns true without calling back, as the
/// pre-cutoff solve() did.
bool compileAndRunQuery(
    CompileContext &ctx, std::span<const CellRef> goals,
    const std::function<bool(const LogicSolution &)> &onSolution,
    std::size_t maxSolutions);

// -- Builtins (Owner: P2, logic_compile_builtins.cpp) -------------------------

/// True for the control and arithmetic builtins P2 lowers: true/0, fail/0, !/0,
/// is/2, < > =< >= =:= =\= (/2), \+/1, \=/2.
[[nodiscard]] bool isControlBuiltin(const PredicateKey &key);

/// Emits @p goal (a builtin for which isControlBuiltin is true).
void emitControlBuiltin(CompileContext &ctx, FrameScope &scope, Chain &chain,
                        const PredicateKey &key, CellRef goal);

// -- Relations (Owner: P3 machinery, logic_relations.cpp; providers: P3, P4) --

/// Registers vortex_module/1, vortex_function/1-3 (+ stdlib_ aliases),
/// vortex_instruction/3, vortex_contract/3, vortex_param/4, setting/2,
/// setting_shape/2, setting_default/2, transclude/3, xanalink/3. (P3)
void registerStdlibRelations(RelationRegistry &registry, VortexStdLib &stdlib);

/// Registers cell_value/2, cell_link/4, cell_span/2, bridge_edge/2-3, and the
/// lens and SQL fact enumerations that ride on them. (P4,
/// logic_relations_arena.cpp)
void registerArenaRelations(RelationRegistry &registry, VortexStdLib &stdlib);

/// Emits the retry loop around RelationNext for @p goal, whose key is in
/// ctx.relations: positions and limit in pre-Choice locals, `Choice` retry,
/// one Unify per argument. (P3, logic_relations.cpp)
void emitRelationGoal(CompileContext &ctx, FrameScope &scope, Chain &chain,
                      const PredicateKey &key, CellRef goal);

/// Connects ctx.relations to the VM's relation hook. The hook itself is O1's
/// (`VortexVM::setRelationProvider`); this is the single place that adapts to
/// its final signature. (P3, logic_relations.cpp)
void installRelationProvider(CompileContext &ctx);

// -- User predicates (Owner: P1, logic_compile_core.cpp) ----------------------

/// True when some candidate predicate cell has text equal to key.name
/// (`core.arena().textOf(pred) == key.name`) and at least one clause. Matches
/// by NAME only, as the old solver did: a clause of another arity is still
/// compiled into the frame and fails at head unification.
[[nodiscard]] bool hasUserClauses(const CompileContext &ctx,
                                  const PredicateKey &key);

/// Emits `InvokeFrame descriptor, args...` for @p goal, compiling the
/// predicate's frame first if it is not in ctx.frames.
void emitUserCall(CompileContext &ctx, FrameScope &scope, Chain &chain,
                  const PredicateKey &key, CellRef goal);

/// The frame descriptor of @p key, compiled on first use (entry-first, so
/// recursion closes). Nullopt when hasUserClauses is false.
[[nodiscard]] std::optional<CellRef>
compilePredicateFrame(CompileContext &ctx, const PredicateKey &key);

/// What the dispatch does with a functor nobody owns (no candidate predicate
/// matched by name, no builtin, no relation). The old solver failed silently
/// here: no candidate, so the goal simply had no solutions and raised nothing.
/// P1 emits a branch to scope.failOp and raises no error. Until then it
/// refuses naming the predicate.
void emitUnknownPredicate(CompileContext &ctx, FrameScope &scope, Chain &chain,
                          const PredicateKey &key);

/// A goal that is not a term (an unbound variable or other non-callable cell).
/// The old solver did `if (!functorOf(goal)) return true;`: it skipped the
/// goal and went on with the next one, with no error and no failure. P1
/// emits nothing for it, so control falls through to the following goal.
void emitNonTermGoal(CompileContext &ctx, FrameScope &scope, Chain &chain,
                     CellRef goal);

/// One clause of a predicate chain: head unification then body goals then
/// ReturnFrame, appended to @p chain. Clears scope.variableSlots on entry.
/// @p last is true for the final clause, which pushes no `Choice`.
void emitClause(CompileContext &ctx, FrameScope &scope, Chain &chain,
                CellRef clause, bool last);

// -- Dispatch (Owner: P0, final; logic_dispatch.cpp) --------------------------

/// A fresh context for @p stdlib: candidates, store, budget, and the relation
/// registry filled by both register functions.
[[nodiscard]] CompileContext
makeCompileContext(VortexCore &core, VortexVM &vm, VortexStdLib &stdlib,
                   std::span<const CellRef> customPredicates,
                   std::size_t maxCycles);

/// Emits @p goal by the lookup order user predicates (P1), control and
/// arithmetic builtins (P2), extensional relations (P3, P4). An unowned
/// functor/arity goes to emitUnknownPredicate (P1), which names it in its
/// refusal until P1 decides otherwise.
void emitGoal(CompileContext &ctx, FrameScope &scope, Chain &chain,
              CellRef goal);

} // namespace zigzag::vortex::logic

#endif // COMMON_XANADU_VORTEX_LOGIC_COMPILE_HPP
