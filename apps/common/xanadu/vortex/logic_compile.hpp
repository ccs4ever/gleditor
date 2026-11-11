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
 * Forward jump targets are Labels placed later.
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
  /// so a scan that mints answer cells does not chase its own tail. 0 on the
  /// first call, which means "take the snapshot now".
  std::size_t limit{};
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
  /// custom predicates: the candidatePreds order of the old solver.
  std::vector<CellRef> candidatePredicates;
  /// VM cycle budget for one solve. Real, not the 100000 vm.run default.
  std::size_t maxCycles{};
  RelationRegistry relations;
  /// Compiled predicate frames, so recursion and mutual recursion close over
  /// the descriptor cell. Owner: P1.
  std::unordered_map<PredicateKey, CellRef, PredicateKeyHash> frames;
};

/// Forward-emission cursor. Empty until the first op is appended.
struct Chain {
  std::optional<CellRef> head;
  std::optional<CellRef> tail;
};

/// A forward jump target, placed later with placeLabel().
struct Label {
  CellRef op{};
};

/// The predicate frame being compiled, as seen by clause and goal emitters.
struct FrameScope {
  PredicateKey predicate;
  /// The shared Fail op: a failing test branches here.
  CellRef failOp{};
  /// Frame-local slots. Argument ports come first, then variables, temporaries
  /// and the barrier.
  std::vector<CellRef> locals;
  /// The slot written by `ChoiceDepth` at entry; `Cut barrier` is `!`.
  std::optional<CellRef> barrier;
  /// Dereferenced clause/query variable cell -> its local slot.
  std::unordered_map<CellRef, CellRef> variableSlots;
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

/// A forward target; place it with placeLabel() once its position is known.
Label newLabel(CompileContext &ctx);
void placeLabel(CompileContext &ctx, Chain &chain, Label label);
/// Appends `Choice target`.
CellRef emitChoice(CompileContext &ctx, Chain &chain, Label target);

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

/// True when some candidate predicate cell named key.name has at least one
/// clause whose head has key.arity arguments.
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

/// What the dispatch does with a functor/arity nobody owns. The old solver let
/// such a goal fail silently, and the spike keeps that, so P1 emits a `Fail`;
/// until then it refuses naming the predicate.
void emitUnknownPredicate(CompileContext &ctx, FrameScope &scope, Chain &chain,
                          const PredicateKey &key);

/// One clause of a predicate chain: head unification then body goals then
/// ReturnFrame, appended to @p chain.
void emitClause(CompileContext &ctx, FrameScope &scope, Chain &chain,
                CellRef clause);

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
