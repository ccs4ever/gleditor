/**
 * @file compiler.hpp
 * @brief vlace evaluator front: the public Compiler API over the Vortex VM.
 *
 * Every vlace expression is lowered to ordinary Vortex graph operations and
 * run on the VM (lower_*.cpp, one file per card); this class holds the
 * persistent state a lowering needs and the host-side `Value` read-back that
 * tests, the mesh evaluator and the REPL consume. A Value is a read-back
 * handle onto an arena cell: it carries the cell it was read from, so forcing
 * it later hits the same memo.
 */
#ifndef COMMON_XANADU_VLACE_COMPILER_HPP
#define COMMON_XANADU_VLACE_COMPILER_HPP

#include <expected>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vlace/ast.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace zigzag {
class ProjectionSession;
} // namespace zigzag

namespace xanadu::vlace::lower {
class LowerState;
} // namespace xanadu::vlace::lower

namespace xanadu::vlace {
/// A host-side view of a runtime value read back from an arena cell.
struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct Value {
  // Exactly one of these is active, chosen by `kind`.
  enum class Kind : std::uint8_t {
    Int,
    Float,
    Str,
    Bool,
    Closure, ///< a function; opaque, applied only on the VM
    Ctor,    ///< a constructed ADT value: tag + argument values (§4.2)
    Thunk,   ///< an unforced suspended computation (§6.2)
  } kind{Kind::Int};
  std::int64_t i{0};
  double f{0.0};
  std::string s;
  bool b{false};
  // Ctor payload.
  std::string tag;            ///< constructor name, e.g. "Cons", "Leaf"
  std::vector<ValuePtr> args; ///< constructor arguments (may be thunks)
  std::size_t ctorArity{0}; ///< total arity; args.size() < ctorArity == partial
  /// The arena cell this value was read from; forcing and applying act on it.
  std::optional<zigzag::CellRef> cell;
};

/// An evaluation failure, named by code (§8.2) with a human detail + location.
struct EvalError {
  CompileError code{CompileError::UnboundIdentifier};
  std::string detail;
  SourceLocation loc{};
};

class Compiler {
public:
  Compiler(zigzag::vortex::VortexCore &core, zigzag::vortex::VortexVM &vm);
  ~Compiler();
  Compiler(const Compiler &)            = delete;
  Compiler &operator=(const Compiler &) = delete;

  /// Lower @p expr to Vortex operations, run them, read the value back.
  [[nodiscard]] std::expected<ValuePtr, EvalError> evaluate(const Expr &expr);

  /// As evaluate, for a whole program (declarations, then the result).
  [[nodiscard]] std::expected<ValuePtr, EvalError>
  evaluateProgram(const Program &prog);

  /// Perform an IO action description at the boundary (§6.5). Building an
  /// action writes nothing; only this performs its leaf effects, and every
  /// force and application it needs runs on the VM.
  std::expected<ValuePtr, EvalError> runIO(const ValuePtr &action,
                                           std::istream &in, std::ostream &out);

  /// Render a value shallowly without forcing a thunk.
  [[nodiscard]] static std::string show(const ValuePtr &v);

  /// Render a value deeply, forcing thunks on the VM. Throws std::logic_error
  /// until rendering is lowered.
  [[nodiscard]] std::string render(const ValuePtr &v, int depthBudget = 10000);

  /// Force a value to weak head normal form on the VM.
  std::expected<ValuePtr, EvalError> forceValue(ValuePtr value);

  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return core_; }
  [[nodiscard]] zigzag::vortex::VortexVM &vm() noexcept { return vm_; }

  /// Constructor / record tables and per-card state that outlive one
  /// evaluation; the built-in constructors are installed on first use.
  [[nodiscard]] lower::LowerState &lowerState();

  /// Projection sessions opened by `openLens` (§7.4). Held so the projected
  /// cells (and their arena) outlive the evaluation that reads them.
  [[nodiscard]] std::vector<std::shared_ptr<zigzag::ProjectionSession>> &
  sessions() noexcept {
    return sessions_;
  }

  // The session that projected @p cell, or nullptr when no open session
  // owns it (an authored or foreign cell). Scans sessions_ calling
  // ownsCell -- the single ownership predicate: node cells resolve through
  // the session's key map, attribute cells one negward hop on a managed
  // dim. Shared by every session-resolution op and the cellKey/cellEq/
  // cellScalar leaves so none re-specifies the climb.
  [[nodiscard]] zigzag::ProjectionSession *
  sessionFor(zigzag::CellRef cell) const noexcept;

private:
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  std::unique_ptr<lower::LowerState> state_;
  std::vector<std::shared_ptr<zigzag::ProjectionSession>> sessions_;
};

} // namespace xanadu::vlace

#endif // COMMON_XANADU_VLACE_COMPILER_HPP
