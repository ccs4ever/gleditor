/**
 * @file types.hpp
 * @brief Hindley-Milner type inference for vlace (§5.5). Normative: a
 *        conforming vlace rejects an ill-typed program before evaluation.
 *
 * Types are represented with mutable union-find type variables (TyVar), type
 * constructors (TyCon name + args: Int, Bool, List a, a -> b as Fun), and
 * generalized to type schemes (forall vars . type) at let/letrec bindings
 * (§5.5 generalizes freely -- no refs or exceptions to force a value
 * restriction). Int and Float are distinct with no implicit conversion
 * (§4.6), so 1 + 2.0 is a TypeMismatch.
 */
#ifndef COMMON_XANADU_VLACE_TYPES_HPP
#define COMMON_XANADU_VLACE_TYPES_HPP

#include <expected>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vlace/ast.hpp"

namespace xanadu::vlace {

struct Ty;
using TyPtr = std::shared_ptr<Ty>;

/// A type: a variable (possibly bound to another type), or a constructor
/// applied to zero+ argument types. Functions are TyCon "->" {from, to}.
struct Ty {
  enum class Kind : std::uint8_t { Var, Con } kind{Kind::Var};
  // Var:
  int varId{0};
  TyPtr instance; ///< non-null once this variable is unified to a type
  // Con:
  std::string con;         ///< "Int", "Bool", "->", "List", a data type name
  std::vector<TyPtr> args; ///< constructor arguments
};

/// A type scheme: forall quantified-vars . body. Monomorphic when vars empty.
struct Scheme {
  std::vector<int> vars; ///< quantified type-variable ids
  TyPtr body;
};

/// A type error, named by CompileError (§8.2) with a human detail + location.
struct TypeError {
  CompileError code{CompileError::TypeMismatch};
  std::string detail;
  SourceLocation loc{};
};

class TypeChecker {
public:
  TypeChecker();

  /// Infer the type of a whole program (registering data decls and binding
  /// top-level value decls), returning the final expression's type rendered as
  /// a string, or a named TypeError. The checker is a separate pass; it does
  /// not evaluate.
  [[nodiscard]] std::expected<std::string, TypeError>
  checkProgram(const Program &prog);

  /// Infer the type of a single expression (REPL), rendered to a string.
  [[nodiscard]] std::expected<std::string, TypeError>
  checkExpression(const Expr &expr);

  /// Render a type to its textual form, e.g. "Int -> Int", "List a".
  [[nodiscard]] static std::string renderType(const TyPtr &t);

private:
  struct TypeEnv {
    std::unordered_map<std::string, Scheme> vars;
    std::shared_ptr<TypeEnv> parent;
  };
  using EnvPtr = std::shared_ptr<TypeEnv>;

  TyPtr freshVar();
  TyPtr prune(TyPtr t); ///< follow a variable's instance chain
  bool occurs(int varId, const TyPtr &t);
  std::expected<void, TypeError> unify(TyPtr a, TyPtr b, SourceLocation loc);

  TyPtr instantiate(const Scheme &s);
  Scheme generalize(const TyPtr &t, const EnvPtr &env);
  void freeVarsOfEnv(const EnvPtr &env, std::vector<int> &out);
  void freeVars(const TyPtr &t, std::vector<int> &out);

  std::expected<TyPtr, TypeError> infer(const ExprNode &e, const EnvPtr &env);
  std::expected<TyPtr, TypeError> inferRecordUpdate(const ERecordUpdate &ru,
                                                    const EnvPtr &env,
                                                    SourceLocation loc);
  std::expected<TyPtr, TypeError>
  inferPattern(const PatternNode &p, const EnvPtr &env, TyPtr expected,
               std::unordered_map<std::string, Scheme> &binds);
  // Register a data decl's ctors; refuses a ctor name the runtime
  // reserves (isReservedCtorTag, §7 P1) since redeclaring e.g. `Some`
  // would overwrite the prelude scheme a program already typechecks
  // against.
  std::expected<void, TypeError> registerData(const DataDecl &dd);

  // Convenience constructors for ground types.
  TyPtr tycon(std::string name, std::vector<TyPtr> args = {});
  TyPtr tyInt();
  TyPtr tyFloat();
  TyPtr tyBool();
  TyPtr tyStr();
  TyPtr tyChar();
  TyPtr tyFun(TyPtr from, TyPtr to);
  TyPtr tyList(TyPtr elem);
  TyPtr tyIO(TyPtr inner);             ///< IO a
  TyPtr tyOption(TyPtr inner);         ///< Option a
  TyPtr tyResult(TyPtr err, TyPtr ok); ///< Result e a
  TyPtr tyUnit();                      ///< Unit (the () type)

  int nextVar_{0};
  EnvPtr globals_{std::make_shared<TypeEnv>()};
  // Constructor name -> its scheme (a function into its data type).
  std::unordered_map<std::string, Scheme> ctors_;
  // Data type name -> its constructor names (for exhaustiveness, §5.5).
  std::unordered_map<std::string, std::vector<std::string>> dataCtors_;
  // Constructor name -> owning data type name.
  std::unordered_map<std::string, std::string> ctorData_;
  // Record field accessor name -> its scheme (forall params. C -> fieldType),
  // so `x p` type-checks as an application of the accessor (§5.7).
  std::unordered_map<std::string, Scheme> fieldAccessors_;
  // Record constructor name -> ordered field names (for ERecordUpdate field
  // name resolution against the ctor's argument types).
  std::unordered_map<std::string, std::vector<std::string>> recordFields_;
  // Built-in value identifiers (not constructors): the typed IO primitives
  // pure/bind/then/putStr/putStrLn/getLine (§6.5). Looked up for an EVar after
  // the lexical env and before failing, so a program references them without
  // import but a user binding of the same name shadows them.
  std::unordered_map<std::string, Scheme> builtins_;
};

} // namespace xanadu::vlace

#endif // COMMON_XANADU_VLACE_TYPES_HPP
