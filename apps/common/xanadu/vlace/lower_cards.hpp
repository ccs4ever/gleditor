/**
 * @file lower_cards.hpp
 * @brief Every function a lowering card L1-L8 fills in, grouped by card, plus
 *        the dispatch entry points (lower_dispatch.cpp) that call them.
 *
 * Written once by card L0. Each card replaces the bodies of its own functions
 * in its own file and never edits this header; the stubs refuse with today's
 * "VM lowering is not implemented for VLace <what>" text until filled.
 * `/data/git/gleditor-restore/.logs/w2/L-stub-table.md` lists, per card, each
 * function, its file and the dispatch site that calls it.
 */
#ifndef COMMON_XANADU_VLACE_LOWER_CARDS_HPP
#define COMMON_XANADU_VLACE_LOWER_CARDS_HPP

#include <expected>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vlace/driver.hpp"
#include "common/xanadu/vlace/lower_context.hpp"

namespace xanadu::vlace::lower {

/// Where a failed pattern jumps. Pattern lowering emits straight-line tests;
/// each test is a BranchFalseCell whose target is not yet known (the next
/// alternative does not exist yet). The pattern pushes every such branch op
/// onto `mismatch` and the caller (the `case` lowering, or a lambda's
/// parameter prologue) patches them all to one place.
struct PatternSite {
  Operand scrutinee;
  std::vector<zigzag::CellRef> mismatch;
};

// =============================================================================
// Dispatch (lower_dispatch.cpp, card L0): one entry per AST node kind. Each
// case calls the card function named in the comment on the case.
// =============================================================================

[[nodiscard]] Lowered<Operand> lowerExpr(LowerContext &ctx, const ExprNode &e);

/// Lower a pattern against `site.scrutinee`, binding its variables in the
/// innermost scope of @p ctx and recording mismatch branches in `site`.
[[nodiscard]] Lowered<void>
lowerPattern(LowerContext &ctx, const PatternNode &p, PatternSite &site);

[[nodiscard]] Lowered<void> lowerDecl(LowerContext &ctx, const Decl &d,
                                      SourceLocation loc = {});

/// Declarations in order, then the trailing expression if any (nullopt when
/// the program has none).
[[nodiscard]] Lowered<std::optional<Operand>> lowerProgram(LowerContext &ctx,
                                                           const Program &prog);

/// The names no scope binds: top-level values (L2), record field accessors
/// (L4), then IO builders (L6), `std:xanadu` verbs (L7), effect leaves (L8),
/// in that order; UnboundIdentifier when none claims the name. Card L1's
/// lowerVar calls this after its scope lookup misses.
[[nodiscard]] Lowered<Operand>
resolveGlobalName(LowerContext &ctx, std::string_view name, SourceLocation loc);

// =============================================================================
// L1: scalars, the run loop, read-back skeleton (lower_scalars.cpp, run_vm.cpp,
// readback.cpp)
// =============================================================================

[[nodiscard]] Lowered<Operand>
lowerLiteral(LowerContext &ctx, const ELiteral &e, SourceLocation loc);
/// A local (scope lookup, force unless known WHNF), else resolveGlobalName.
[[nodiscard]] Lowered<Operand> lowerVar(LowerContext &ctx, const EVar &e,
                                        SourceLocation loc);
/// `+ - * / % == /= < <= > >= && || ++` only; `:`, `$`, `>>=` and `>>` are
/// routed by the dispatch to L3, L2 and L6.
[[nodiscard]] Lowered<Operand> lowerBinOp(LowerContext &ctx, const EBinOp &e,
                                          SourceLocation loc);
[[nodiscard]] Lowered<Operand> lowerIf(LowerContext &ctx, const EIf &e,
                                       SourceLocation loc);
/// `True` / `False` as a scalar bool register.
[[nodiscard]] Lowered<Operand> lowerBoolCtor(LowerContext &ctx, bool value,
                                             SourceLocation loc);

/// Compiler::evaluate / evaluateProgram bodies: build a LowerContext, lower,
/// assemble, run the VM, read the result back.
[[nodiscard]] std::expected<ValuePtr, EvalError>
evaluateExpressionOnVm(Compiler &compiler, const Expr &expr);
[[nodiscard]] std::expected<ValuePtr, EvalError>
evaluateProgramOnVm(Compiler &compiler, const Program &prog);
/// Driver::run's evaluation half (after parse and type check): fills
/// `partial.value` and returns it, or a "compile" / "eval" phase error.
[[nodiscard]] std::expected<DriverResult, DriverError>
driverEvaluate(const Program &prog, DriverResult partial);

/// Read the cell back into a host Value. Classifies by the cell: scalars (L1),
/// then kernel cells -- thunk or closure (L2), then data terms (L3); a cell no
/// hook claims is refused. These are the per-kind hooks; nullopt = not mine.
[[nodiscard]] std::expected<ValuePtr, EvalError>
readbackCell(Compiler &compiler, zigzag::CellRef cell);
[[nodiscard]] std::optional<Lowered<ValuePtr>>
readbackScalar(Compiler &compiler, zigzag::CellRef cell);
/// Compiler::render body. Scalars and closures are L1's; a constructed value
/// is delegated to renderData (L3).
[[nodiscard]] std::string renderValue(Compiler &compiler, const ValuePtr &v,
                                      int depthBudget);

// =============================================================================
// L2: thunk / force / apply kernel; let, lambda, application, top level
// (lower_kernel.cpp)
// =============================================================================

[[nodiscard]] Lowered<Operand> lowerLet(LowerContext &ctx, const ELet &e,
                                        SourceLocation loc);
[[nodiscard]] Lowered<Operand> lowerLambda(LowerContext &ctx, const ELambda &e,
                                           SourceLocation loc);
[[nodiscard]] Lowered<Operand> lowerApp(LowerContext &ctx, const EApp &e,
                                        SourceLocation loc);
/// `f $ x`, the `$` binary operator; same lowering as EApp.
[[nodiscard]] Lowered<Operand> lowerDollar(LowerContext &ctx, const EBinOp &e,
                                           SourceLocation loc);
[[nodiscard]] Lowered<void>
lowerValueDecl(LowerContext &ctx, const ValueDecl &d, SourceLocation loc);
/// Top-level value or known function named @p name; nullopt when no such
/// declaration exists (an empty table is a correct answer until L2 fills it).
[[nodiscard]] std::optional<Lowered<Operand>>
lookupTopLevel(LowerContext &ctx, std::string_view name, SourceLocation loc);

/// Shared kernel helpers that L3-L8 call. All return a register.
/// A register for @p e that is evaluated later: the value itself when its form
/// is statically WHNF, otherwise a fresh thunk capturing what @p e needs.
[[nodiscard]] Lowered<Operand> lowerLazy(LowerContext &ctx, const ExprNode &e);
/// Emit the shared `force` frame call on @p value; the result is in WHNF.
[[nodiscard]] Lowered<Operand> forceOperand(LowerContext &ctx, Operand value,
                                            SourceLocation loc);
/// Emit the shared `apply` call of the unary closure @p fn to @p arg.
[[nodiscard]] Lowered<Operand> applyOperand(LowerContext &ctx, Operand fn,
                                            Operand arg, SourceLocation loc);

/// Compiler::forceValue body: run the force frame on `value->cell` on the VM,
/// then read back.
[[nodiscard]] std::expected<ValuePtr, EvalError>
forceValueOnVm(Compiler &compiler, ValuePtr value);
/// Read-back hook for thunk cells (memo edge present: read the memo target)
/// and closure roots (Kind::Closure). nullopt = not a kernel cell.
[[nodiscard]] std::optional<Lowered<ValuePtr>>
readbackKernel(Compiler &compiler, zigzag::CellRef cell);

// =============================================================================
// L3: constructors, tuples, lists, patterns, case, builtin ctors
// (lower_data.cpp)
// =============================================================================

/// A constructor used as a value (nullary term, or a curried builder when it
/// takes arguments). A saturated application is built by makeTerm directly.
[[nodiscard]] Lowered<Operand> lowerCtor(LowerContext &ctx, const ECtor &e,
                                         SourceLocation loc);
[[nodiscard]] Lowered<Operand> lowerTuple(LowerContext &ctx, const ETuple &e,
                                          SourceLocation loc);
[[nodiscard]] Lowered<Operand> lowerList(LowerContext &ctx, const EList &e,
                                         SourceLocation loc);
/// `h : t`; the dispatch routes the `:` operator here.
[[nodiscard]] Lowered<Operand> lowerCons(LowerContext &ctx, const EBinOp &e,
                                         SourceLocation loc);
[[nodiscard]] Lowered<Operand> lowerCase(LowerContext &ctx, const ECase &e,
                                         SourceLocation loc);
[[nodiscard]] Lowered<void> lowerPatVar(LowerContext &ctx, const PVar &p,
                                        SourceLocation loc, PatternSite &site);
[[nodiscard]] Lowered<void> lowerPatLiteral(LowerContext &ctx,
                                            const PLiteral &p,
                                            SourceLocation loc,
                                            PatternSite &site);
[[nodiscard]] Lowered<void> lowerPatConstructor(LowerContext &ctx,
                                                const PConstructor &p,
                                                SourceLocation loc,
                                                PatternSite &site);
[[nodiscard]] Lowered<void> lowerPatTuple(LowerContext &ctx, const PTuple &p,
                                          SourceLocation loc,
                                          PatternSite &site);
/// Register a `data` declaration in the constructor / field tables; refuses a
/// reserved or duplicate tag.
[[nodiscard]] Lowered<void> lowerDataDecl(LowerContext &ctx, const DataDecl &d,
                                          SourceLocation loc);
/// Install None/Some/Ok/Err and the runtime-owned tags (Cell, Dim, Unit,
/// SInt, SFloat, SBool, Nil, Cons, Tuple, True, False) in the ctor table. Run
/// once per Compiler, before the first evaluation. A no-op stub is correct
/// until L3: nothing resolves a constructor before then.
void installBuiltinCtors(LowerState &state);

/// Shared term builders that L6-L8 (and L2 for partial constructors) call.
/// A saturated constructor term: one fresh slot per field holding the field.
[[nodiscard]] Lowered<Operand> makeTerm(LowerContext &ctx, std::string_view tag,
                                        std::span<const Operand> fields,
                                        SourceLocation loc);
/// A curried builder closure for (tag, arity) over makeTerm.
[[nodiscard]] Lowered<Operand> termBuilder(LowerContext &ctx,
                                           std::string_view tag,
                                           std::size_t arity,
                                           SourceLocation loc);

/// Read-back of a term cell into Kind::Ctor (tag, args, ctorArity); nullopt
/// when the cell is not a term.
[[nodiscard]] std::optional<Lowered<ValuePtr>>
readbackData(Compiler &compiler, zigzag::CellRef cell);
/// Deep rendering of a constructed value: lists `[a, b]`, tuples `(a, b)`,
/// `Tag a b`; children are forced through compiler.forceValue.
[[nodiscard]] std::string renderData(Compiler &compiler, const Value &v,
                                     int depthBudget);

// =============================================================================
// L4: records (lower_records.cpp)
// =============================================================================

[[nodiscard]] Lowered<Operand> lowerRecordUpdate(LowerContext &ctx,
                                                 const ERecordUpdate &e,
                                                 SourceLocation loc);
/// A record field used as a function (`x p`), when @p name is a field no scope
/// shadows; nullopt when it is not a field name.
[[nodiscard]] std::optional<Lowered<Operand>>
lowerFieldAccessor(LowerContext &ctx, std::string_view name,
                   SourceLocation loc);
[[nodiscard]] Lowered<void> lowerPatRecord(LowerContext &ctx, const PRecord &p,
                                           SourceLocation loc,
                                           PatternSite &site);

// =============================================================================
// L6: IO descriptions and the host trampoline (lower_io.cpp)
// =============================================================================

/// `>>=` and `>>` (the operator string selects IO.Bind / IO.Then).
[[nodiscard]] Lowered<Operand> lowerIoBind(LowerContext &ctx, const EBinOp &e,
                                           SourceLocation loc);
/// pure bind then putStr putStrLn getLine; nullopt for any other name.
[[nodiscard]] std::optional<Lowered<Operand>>
lowerIoBuiltin(LowerContext &ctx, std::string_view name, SourceLocation loc);

/// The state a trampoline step and an effect leaf share.
struct IoRunContext {
  Compiler &compiler;
  LowerContext &lower;
  std::istream &in;
  std::ostream &out;
};

/// Compiler::runIO body.
[[nodiscard]] std::expected<ValuePtr, EvalError>
runIoOnVm(Compiler &compiler, const ValuePtr &action, std::istream &in,
          std::ostream &out);
/// Driver::runIO's evaluation half; same contract as driverEvaluate.
[[nodiscard]] std::expected<DriverResult, DriverError>
driverEvaluateIO(const Program &prog, DriverResult partial, std::istream &in,
                 std::ostream &out);

// =============================================================================
// L7: std:xanadu pure verbs (lower_xanadu.cpp)
// =============================================================================

/// makeText makeDim dimensionNamed setLink cellText linked childrenAll words;
/// nullopt for any other name.
[[nodiscard]] std::optional<Lowered<Operand>>
lowerXanaduBuiltin(LowerContext &ctx, std::string_view name,
                   SourceLocation loc);

// =============================================================================
// L8: effect leaves and lens (lower_effects.cpp)
// =============================================================================

/// promote openLens the nine lens* verbs and cellKey cellEq cellScalar;
/// nullopt for any other name.
[[nodiscard]] std::optional<Lowered<Operand>>
lowerEffectBuiltin(LowerContext &ctx, std::string_view name,
                   SourceLocation loc);
/// Perform one effect leaf the L6 trampoline met: @p tag is the IO.* tag,
/// @p args the leaf's already-evaluated argument registers. Returns the
/// leaf's result register; nullopt when @p tag is not an L8 leaf.
[[nodiscard]] std::optional<Lowered<Operand>>
performEffectLeaf(IoRunContext &run, std::string_view tag,
                  std::span<const Operand> args, SourceLocation loc);

} // namespace xanadu::vlace::lower

#endif // COMMON_XANADU_VLACE_LOWER_CARDS_HPP
