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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vlace/ast.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

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

struct Value {
  // Exactly one of these is active, chosen by `kind`.
  enum class Kind : std::uint8_t {
    Int,
    Float,
    Str,
    Bool,
    Closure,
    Ctor,  ///< a constructed ADT value: tag + argument values (§4.2)
    Thunk, ///< an unforced suspended computation (§6.2)
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

  /// Render a value to the text a REPL prints (shallow; does not force).
  [[nodiscard]] static std::string show(const ValuePtr &v);

  /// Force a value fully and render it, including lazy lists as [a, b, c] and
  /// constructed values as "Ctor arg arg". Bounded so a cyclic/infinite value
  /// is truncated rather than looping (§6.7).
  [[nodiscard]] std::string render(const ValuePtr &v, int depthBudget = 10000);

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
  void registerData(const DataDecl &dd);

  // Build an unforced thunk value from an expression + environment.
  ValuePtr makeThunk(const ExprNode *body, const EnvPtr &env);

  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  EnvPtr globals_{std::make_shared<Env>()};
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
};

} // namespace xanadu::vlace

#endif // COMMON_XANADU_VLACE_COMPILER_HPP
