/**
 * @file compiler.hpp
 * @brief vlace evaluator over the Vortex core (Stage 3: arithmetic, let,
 *        lambda/application, if).
 *
 * Stage 3 evaluates a vlace expression in an ArenaManifold, producing a value.
 * Arithmetic is performed through the VM's arithmetic opcodes over scalar cells
 * (R6: the bits in the node), so numeric work mints arena scalar cells and
 * spools no content -- the regime §2.2 calls the referentially transparent one.
 * Functions are closures capturing their defining environment (§4.3); `let` and
 * `letrec` extend the environment (§5.2); `if` is strict in its condition.
 *
 * Later stages replace this tree-walk with full opcode-stream lowering where
 * laziness and pattern matching demand it (§6, §5.4); Stage 3 is the first
 * end-to-end "it evaluates" increment the staged plan asks for.
 */
#ifndef COMMON_XANADU_VLACE_COMPILER_HPP
#define COMMON_XANADU_VLACE_COMPILER_HPP

#include <expected>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vlace/ast.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace zigzag {
class ProjectionSession;
} // namespace zigzag

namespace xanadu::vlace {
/// A runtime value. Stage 3: a scalar (int/float/string/bool) or a closure.
/// Later stages add constructed values (ADTs), thunks, and lists.
struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct Env; ///< lexical environment, a chain of frames
using EnvPtr = std::shared_ptr<Env>;

struct Closure {
  std::vector<Pattern> const *params{nullptr}; ///< borrowed from the AST
  ExprNode const *body{nullptr};               ///< borrowed from the AST
  std::size_t applied{0}; ///< how many params already supplied
  std::vector<ValuePtr> argsSoFar;
  EnvPtr env; ///< captured defining environment
};

struct Thunk; ///< a suspended computation (laziness, §6.2)

class Compiler;
/// A built-in C++ function body: receives the compiler (for arena access) and
/// the collected argument values, returns a value or an error (§13).
using NativeFn = std::function<std::expected<ValuePtr, struct EvalError>(
    Compiler &, std::vector<ValuePtr> &)>;

struct Value {
  // Exactly one of these is active, chosen by `kind`.
  enum class Kind : std::uint8_t {
    Int,
    Float,
    Str,
    Bool,
    Closure,
    Ctor,   ///< a constructed ADT value: tag + argument values (§4.2)
    Thunk,  ///< an unforced suspended computation (§6.2)
    Native, ///< a built-in C++ function (std:xanadu arena verbs, §13)
  } kind{Kind::Int};
  std::int64_t i{0};
  double f{0.0};
  std::string s;
  bool b{false};
  std::shared_ptr<Closure> clo;
  // Ctor payload.
  std::string tag;            ///< constructor name, e.g. "Cons", "Leaf"
  std::vector<ValuePtr> args; ///< constructor arguments (may be thunks)
  std::size_t ctorArity{0}; ///< total arity; args.size() < ctorArity == partial
  // Thunk payload.
  std::shared_ptr<Thunk> thunk;
  // Native payload: a C++ built-in collecting `nativeArity` arguments, then
  // invoking `native` with the Compiler and the collected (unforced) args. Used
  // for the std:xanadu arena-authoring verbs (§13); partial application
  // accumulates into `args` exactly as a Ctor does.
  std::size_t nativeArity{0};
  std::string nativeName; ///< for diagnostics
  std::shared_ptr<NativeFn> native;
};

/// A suspended computation forced at most once, memoized in place (§6.2/§6.3).
struct Thunk {
  ExprNode const *body{nullptr}; ///< borrowed from the AST
  EnvPtr env;
  bool forced{false};
  ValuePtr cached;
};

struct Env {
  std::unordered_map<std::string, ValuePtr> vars;
  EnvPtr parent;
};

/// An evaluation failure, named by code (§8.2) with a human detail + location.
struct EvalError {
  CompileError code{CompileError::UnboundIdentifier};
  std::string detail;
  SourceLocation loc{};
};

class Compiler {
public:
  Compiler(zigzag::vortex::VortexCore &core, zigzag::vortex::VortexVM &vm)
      : core_(core), vm_(vm) {}

  /// Evaluate an expression in a fresh global environment (plus any
  /// previously-defined top-level bindings). Returns the value or a named
  /// error.
  [[nodiscard]] std::expected<ValuePtr, EvalError> evaluate(const Expr &expr);

  /// Evaluate a whole program: bind its declarations, then evaluate the
  /// trailing expression (or return a unit-ish Int 0 when there is none).
  [[nodiscard]] std::expected<ValuePtr, EvalError>
  evaluateProgram(const Program &prog);

  /// Install the built-in value bindings a fresh program expects without an
  /// explicit import: the typed IO primitives (pure/bind/then/putStr/putStrLn/
  /// getLine, §6.5) and the Option/Result constructors (§6.6). Called by
  /// evaluateProgram; idempotent. IO primitives are description-builders
  /// (tagged Ctor values) performed only by runIO at the boundary -- building
  /// one writes nothing, so the pure fragment stays referentially transparent.
  void installBuiltins();

  /// Perform an IO action at the program boundary (§6.5/§8.2). `action` is the
  /// value produced by evaluating an `IO a` program; runIO interprets its
  /// description tree (IO.Pure/PutStr/PutStrLn/GetLine/Bind/Then and any
  /// std:xanadu effect node) performing real console/authoring effects, and
  /// returns the final wrapped value `a`. `in`/`out` are the console streams
  /// (injected so tests can drive them). Only runIO performs effects; render()
  /// and evaluate() never do.
  std::expected<ValuePtr, EvalError> runIO(const ValuePtr &action,
                                           std::istream &in, std::ostream &out);

  /// Render a value to the text a REPL prints (shallow; does not force).
  [[nodiscard]] static std::string show(const ValuePtr &v);

  /// Force a value fully and render it, including lazy lists as [a, b, c] and
  /// constructed values as "Ctor arg arg". Bounded so a cyclic/infinite value
  /// is truncated rather than looping (§6.7).
  [[nodiscard]] std::string render(const ValuePtr &v, int depthBudget = 10000);

  /// Force a portable result under the same evaluator budget, without
  /// rendering.
  std::expected<ValuePtr, EvalError> forceValue(ValuePtr value) {
    return force(std::move(value));
  }

  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return core_; }

private:
  std::expected<ValuePtr, EvalError> eval(const ExprNode &e, const EnvPtr &env);
  std::expected<ValuePtr, EvalError>
  evalBinOp(const EBinOp &op, const EnvPtr &env, SourceLocation loc);
  std::expected<ValuePtr, EvalError> apply(const ValuePtr &fn, ValuePtr arg,
                                           SourceLocation loc);

  // Evaluate a record construction / functional update (§5.7). The record
  // subexpr forces to a Ctor; named fields are placed by the ctor's field-name
  // list (ctorFields_), replacing positional args by name. A bare constructor
  // subexpr yields a fresh fully-applied Ctor; an existing record is copied so
  // the original is left unchanged (sharing the untouched fields).
  std::expected<ValuePtr, EvalError> evalRecordUpdate(const ERecordUpdate &ru,
                                                      const EnvPtr &env,
                                                      SourceLocation loc);

  // Arithmetic / comparison performed via the VM's opcodes over scalar cells.
  std::expected<ValuePtr, EvalError> numericOp(std::string_view op,
                                               const ValuePtr &lhs,
                                               const ValuePtr &rhs,
                                               SourceLocation loc);

  // Force a value to weak head normal form: if it is an unforced thunk, run
  // its body once and memoize the result (§6.7). Returns the forced value.
  std::expected<ValuePtr, EvalError> force(ValuePtr v);

  // Try to match `pat` against `scrutinee`, binding pattern variables into
  // `out`. Returns true on a match. Forces the scrutinee to WHNF as far as the
  // pattern demands (§5.4). A failed match leaves `out` to be discarded by the
  // caller.
  std::expected<bool, EvalError>
  matchPattern(const PatternNode &pat, const ValuePtr &scrutinee,
               std::unordered_map<std::string, ValuePtr> &out);

  // Register a data declaration's constructors (name -> arity) so ECtor /
  // application can build constructed values and matching can check arity.
  // Refuses a ctor name the runtime itself interprets (the reserved tags:
  // Cell/Dim/IO.*/the prelude ADTs/the Scalar ctors -- §7 P1).
  std::expected<void, EvalError> registerData(const DataDecl &dd);

  // Install the std:xanadu arena-authoring verbs (§13) as native built-ins:
  // makeText/makeDim/dimensionNamed/setLink/cellText/linked (pure arena verbs)
  // and `promote` (the one authoring effect, an IO description). Called by
  // installBuiltins.
  void installXanadu();

  // Build an unforced thunk value from an expression + environment.
  ValuePtr makeThunk(const ExprNode *body, const EnvPtr &env);

  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  EnvPtr globals_{std::make_shared<Env>()};
  std::size_t evaluationDepth_{0};
  // Constructor name -> arity. Nil/Cons and True/False are pre-registered.
  std::unordered_map<std::string, std::size_t> ctorArity_;
  // Record constructor name -> ordered field names, in Inputs-Wing order
  // (§5.7). Populated by registerData() for record ctors; drives named-field
  // placement in ERecordUpdate and record construction.
  std::unordered_map<std::string, std::vector<std::string>> ctorFields_;
  // Field name -> the record constructor it belongs to. A field name is a
  // top-level accessor function C -> fieldType (§5.7); eval of EVar/EApp
  // special-cases a known accessor applied to a Ctor to return that arg.
  std::unordered_map<std::string, std::string> fieldAccessor_;
  // Projection sessions opened by `openLens` (§7.4). Held so the projected
  // cells (and their arena) outlive the evaluation that reads them.
  std::vector<std::shared_ptr<zigzag::ProjectionSession>> sessions_;

  // The session that projected @p cell, or nullptr when no open session
  // owns it (an authored or foreign cell). Scans sessions_ calling
  // ownsCell -- the single ownership predicate: node cells resolve through
  // the session's key map, attribute cells one negward hop on a managed
  // dim. Shared by every session-resolution op and the cellKey/cellEq/
  // cellScalar natives so none re-specifies the climb.
  [[nodiscard]] zigzag::ProjectionSession *
  sessionFor(zigzag::CellRef cell) const noexcept;
};

} // namespace xanadu::vlace

#endif // COMMON_XANADU_VLACE_COMPILER_HPP
