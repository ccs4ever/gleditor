#include "common/xanadu/vagent/compiler.hpp"

#include "common/xanadu/vagent/bootstrap.hpp"
#include "common/xanadu/vagent/cell_path.hpp"
#include "common/xanadu/vagent/fragment.hpp"
#include "common/xanadu/vagent/marshal.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace xanadu::vagent {
namespace {

using zigzag::CellRef;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

enum class Scalar : std::uint8_t {
  Int,
  Float,
  Str,
  Bool,
  Unit,
  Cell,
  CellList,
  Bottom
};

std::optional<Scalar> scalarType(const Type &type) {
  if (type.name == "List" && type.arguments.size() == 1 &&
      type.arguments.front() == Type{"Cell", {}})
    return Scalar::CellList;
  if (!type.arguments.empty()) return std::nullopt;
  if (type.name == "Int") return Scalar::Int;
  if (type.name == "Float") return Scalar::Float;
  if (type.name == "Str") return Scalar::Str;
  if (type.name == "Bool") return Scalar::Bool;
  if (type.name == "()") return Scalar::Unit;
  if (type.name == "Cell") return Scalar::Cell;
  return std::nullopt;
}

std::optional<Type> resultType(Scalar type) {
  switch (type) {
  case Scalar::Int:
    return Type{"Int", {}};
  case Scalar::Float:
    return Type{"Float", {}};
  case Scalar::Str:
    return Type{"Str", {}};
  case Scalar::Bool:
    return Type{"Bool", {}};
  case Scalar::Unit:
    return Type{"()", {}};
  case Scalar::Cell:
    return Type{"Cell", {}};
  case Scalar::CellList:
    return Type{"List", {Type{"Cell", {}}}};
  case Scalar::Bottom:
    return std::nullopt;
  }
  return std::nullopt;
}

bool comparison(std::string_view op) {
  return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
         op == ">=";
}

// §5.3's capture-alias namespaces: `cap$N` marks an expand-minted
// identifier rename ('$' never lexes), `cap_N` the cells"…" $-anchor
// spelling (vql's `$var` charset is [alnum_]) — either prefix marks a
// binding-table entry standing for a captured binding's own cell.
bool capName(std::string_view name) {
  return name.size() > 4 &&
         (name.starts_with("cap$") || name.starts_with("cap_"));
}

OpcodeKind operation(std::string_view op) {
  if (op == "+" || op == "++") return OpcodeKind::Add;
  if (op == "-") return OpcodeKind::Sub;
  if (op == "*") return OpcodeKind::Mul;
  if (op == "/") return OpcodeKind::Div;
  if (op == "%") return OpcodeKind::Mod;
  if (op == "==") return OpcodeKind::Eq;
  if (op == "!=") return OpcodeKind::Neq;
  if (op == "<") return OpcodeKind::Lt;
  if (op == "<=") return OpcodeKind::Lte;
  if (op == ">") return OpcodeKind::Gt;
  return OpcodeKind::Gte;
}

class Emitter {
public:
  Emitter(VortexCore &core, VortexVM &vm, const CompilerBindings &bindings,
          ScalarCompilerLimits limits, const FragmentContext *fragments,
          Compiler *compiler, bool recordOps)
      : core_(core), vm_(vm), bindings_(bindings), limits_(limits),
        fragments_(fragments), compiler_(compiler), recordOps_(recordOps) {}

  std::expected<CompiledStatement, Diagnostic> compile(const Expr &expression,
                                                       ResultUse use) {
    try {
      if (&vm_.core() != &core_)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "compiler core and VM do not share an arena");
      if (vm_.choiceDepth() != 0 || core_.arena().outstandingMarks() != 0)
        refuse(expression, DiagnosticCode::InvalidPosition,
               "assemble a statement before opening turn marks or choices");
      const auto type = preflight(expression, true, 0, false, false, use);
      return emitStatement(expression, type);
    } catch (const Diagnostic &diagnostic) {
      return std::unexpected(diagnostic);
    }
  }

  // §6.1's fragment emission seam: the body is preflighted with its
  // declared parameters seeded as frame-bound locals (indices 0..n-1 —
  // the prologue's per-name #RESOLVE ops fill them off the call frame's
  // d.vars/d.values channel), then emitted under the statement wrapper
  // with a #Return tail and the spill/publish records run relies on.
  std::expected<FragmentBody, Diagnostic> compileFragment(
      const Expr &body, const std::vector<Parameter> &parameters, ResultUse use,
      const OwnerFields *ownerFields = nullptr,
      const std::map<std::string, std::string, std::less<>> *capFieldNames =
          nullptr) {
    try {
      if (&vm_.core() != &core_)
        refuse(body, DiagnosticCode::InvalidDeclaration,
               "compiler core and VM do not share an arena");
      scopes_.emplace_back();
      paramNames_.reserve(parameters.size());
      for (const auto &parameter : parameters) {
        const auto scalar = scalarType(parameter.type);
        if (!scalar)
          refuse(body, DiagnosticCode::InvalidDeclaration,
                 "fragment parameter type is outside the scalar lowering "
                 "subset: " +
                     typeName(parameter.type));
        scopes_.back().emplace(parameter.name, Local{localCount_, *scalar});
        paramNames_.push_back(parameter.name);
        ++localCount_;
      }
      paramCount_    = parameters.size();
      capFieldNames_ = capFieldNames;
      if (ownerFields && !ownerFields->empty()) {
        ownerFields_ = ownerFields;
        // The dispatcher binds the runtime owner ref on the head's
        // trailing `$owner` parameter — the one name callers may not
        // declare themselves (snake_case admits no '$').
        for (std::size_t i = parameters.size(); i-- > 0;)
          if (parameters[i].name == "$owner") {
            ownerIndex_ = i;
            break;
          }
        if (ownerIndex_ == npos)
          refuse(body, DiagnosticCode::InvalidDeclaration,
                 "owner-relative fields require a trailing $owner "
                 "parameter");
      }
      const auto type = preflight(body, true, 0, false, false, use);
      return emitFragmentBody(body, type);
    } catch (const Diagnostic &diagnostic) {
      return std::unexpected(diagnostic);
    }
  }

  // §5.5 staged evaluation: lower the operand into its own graph, run
  // it in the compile arena under staticFuel with no mark outstanding,
  // and answer the type-matched result. `bindings` is the caller's
  // table plus the closed-`let` overlay; `methods` the callsite table
  // the eval's RunCall hops resolve through. The eval residue — dead
  // graph, cursor cell, temporaries — stays in the compile arena,
  // which is discarded; recordOps=false keeps it out of the program
  // image's op sequence. Public for Compiler::drainStatics, which hosts
  // it over records the deferred sink collected.
  // `expected` is the checked scalar — absent, any literal-expressible
  // result is accepted (an unannotated `var`'s seed has no declared
  // type to match).
  std::expected<CellValue, Diagnostic>
  evalStatic(const Expr &expression, std::optional<Scalar> expected,
             const CompilerBindings &bindings,
             const std::map<std::string, MethodTarget, std::less<>> *methods) {
    auto fail = [&](DiagnosticCode code, std::string detail) {
      return std::unexpected(
          Diagnostic{code, std::move(detail), expression.loc});
    };
    FragmentContext evalContext;
    if (fragments_) evalContext = *fragments_;
    evalContext.methods = methods;
    // A `static` inside the operand evaluates eagerly at its own emit —
    // the enclosing run needs the folded cell before it starts.
    evalContext.pendingStatics = nullptr;
    evalContext.bindings       = &bindings;
    Emitter eval(core_, vm_, bindings, limits_, &evalContext, compiler_, false);
    auto lowered = eval.compile(*expression.children.front(), ResultUse::Value);
    if (!lowered)
      return fail(lowered.error().code,
                  "'static' operand does not lower: " + lowered.error().detail);
    const auto cursor = vm_.spawnCursor(lowered->entry, "vagent.static");
    vm_.setCursorOpcode(cursor, lowered->entry);
    const auto outcome = vm_.run(cursor, limits_.staticFuel);
    // spawnCursor registered the eval cursor on home's +d.cursors rank;
    // unlink it there so the eval leaves only dead graph behind.
    zigzag::expectWritten(core_.arena().unlink(cursor, core_.dims().cursors,
                                               zigzag::DimVector::NEG));
    zigzag::expectWritten(core_.arena().unlink(cursor, core_.dims().cursors,
                                               zigzag::DimVector::POS));
    if (!outcome.success)
      return fail(DiagnosticCode::InvalidDeclaration,
                  "'static' operand did not evaluate: " + outcome.errorMessage);
    // The compiler.cpp:108-110 invariant holds across eval: a `static`
    // that leaves the VM off its no-choice, no-mark baseline would
    // refuse the next compileStatement anyway — refuse it here.
    if (vm_.choiceDepth() != 0 || core_.arena().outstandingMarks() != 0)
      return fail(DiagnosticCode::InvalidDeclaration,
                  "'static' evaluation left the VM off its no-choice, "
                  "no-mark baseline");
    const auto flag = core_.render(lowered->produced);
    if (!flag || !std::holds_alternative<bool>(*flag) || !std::get<bool>(*flag))
      return fail(DiagnosticCode::InvalidDeclaration,
                  "'static' operand produced no value");
    const auto value = core_.render(lowered->result);
    const bool matches =
        value &&
        (!expected ? (std::holds_alternative<std::int64_t>(*value) ||
                      std::holds_alternative<double>(*value) ||
                      std::holds_alternative<std::string>(*value) ||
                      std::holds_alternative<bool>(*value))
                   : (*expected == Scalar::Int &&
                      std::holds_alternative<std::int64_t>(*value)) ||
                         (*expected == Scalar::Float &&
                          std::holds_alternative<double>(*value)) ||
                         (*expected == Scalar::Str &&
                          std::holds_alternative<std::string>(*value)) ||
                         (*expected == Scalar::Bool &&
                          std::holds_alternative<bool>(*value)) ||
                         (*expected == Scalar::Unit &&
                          std::holds_alternative<std::string>(*value) &&
                          std::get<std::string>(*value).empty()));
    if (!matches)
      return fail(DiagnosticCode::TypeMismatch,
                  "'static' result does not match its checked type");
    return *value;
  }

private:
  [[noreturn]] static void refuse(const Expr &expression, DiagnosticCode code,
                                  std::string detail) {
    throw Diagnostic{code, std::move(detail), expression.loc};
  }

  static bool isBuiltinCall(std::string_view name) {
    static const std::unordered_set<std::string_view> kBuiltins{
        "str_len",    "byte_at",     "str_slice",   "int_to_str", "str_to_int",
        "abs",        "min",         "max",         "clamp",      "trim",
        "to_lower",   "to_upper",    "head_of",     "make_cell",  "program",
        "agent_cell", "boot_writer", "boot_linker", "tab",        "move",
        "match",      "many",        "upto",        "any",        "find",
        "pos",        "subject",
    };
    return kBuiltins.contains(name);
  }

  static std::uint64_t magnitude(const Expr &expression) {
    auto spelling = expression.text;
    std::erase(spelling, '_');
    std::string_view digits = spelling;
    int radix               = 10;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
      digits.remove_prefix(2);
      radix = 16;
    }
    std::uint64_t value{};
    const auto parsed = std::from_chars(
        digits.data(), digits.data() + digits.size(), value, radix);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != digits.data() + digits.size() || digits.empty())
      refuse(expression, DiagnosticCode::TypeMismatch,
             "invalid Int literal for scalar lowering");
    return value;
  }

  Scalar binding(const Expr &expression) {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      const auto local = scope->find(expression.text);
      if (local != scope->end()) {
        localReferences_[&expression] = local->second.index;
        if (local->second.rich) resultTypes_[&expression] = *local->second.rich;
        return local->second.type;
      }
    }
    // §5.3's owner-field captures: a cap$N whose captured name is an
    // owner field resolves owner-relative — the alias itself is never
    // a field, so the join-supplied map names it.
    if (capFieldNames_ && ownerFields_ && capName(expression.text)) {
      if (const auto field = capFieldNames_->find(expression.text);
          field != capFieldNames_->end()) {
        const auto found = ownerFields_->find(field->second);
        if (found == ownerFields_->end())
          refuse(expression, DiagnosticCode::UnknownName,
                 "owner field '" + field->second + "' captured by " +
                     expression.text + " is not declared");
        const auto type = scalarType(found->second);
        if (!type)
          refuse(expression, DiagnosticCode::InvalidDeclaration,
                 "owner field '" + field->second +
                     "' has a type outside the scalar lowering subset: " +
                     typeName(found->second));
        ownerRefs_.insert(&expression);
        capFieldRefs_[&expression] = field->second;
        return *type;
      }
    }
    // §5.8's owner-relative fields: a member body's bare `var` name
    // resolves on the runtime owner (installed frame / mounted
    // instance), not any compile-time cell — recorded per-expression
    // so emit can pick the #FIND/#SET hop instead of a slot read.
    // Locals and parameters still shadow the owner's names.
    if (ownerFields_) {
      if (const auto field = ownerFields_->find(expression.text);
          field != ownerFields_->end()) {
        const auto type = scalarType(field->second);
        if (!type)
          refuse(expression, DiagnosticCode::InvalidDeclaration,
                 "owner field '" + expression.text +
                     "' has a type outside the scalar lowering subset: " +
                     typeName(field->second));
        ownerRefs_.insert(&expression);
        return *type;
      }
    }
    const auto found = bindings_.find(expression.text);
    if (found == bindings_.end())
      refuse(expression, DiagnosticCode::UnknownName,
             "unbound scalar name '" + expression.text + "'");
    const auto type = scalarType(found->second.type);
    if (!type)
      refuse(expression, DiagnosticCode::InvalidDeclaration,
             "scalar lowering does not yet support binding type " +
                 typeName(found->second.type));
    if (!core_.arena().contains(found->second.cell))
      refuse(expression, DiagnosticCode::UnknownName,
             "binding cell is not present in this arena");
    // §5.3: a cap$N capture alias names the captured binding's own cell.
    // Capture cells are compile-time-unvalued — frame cells fill per
    // activation — and the checker already proved the type, so the
    // unvalued render check is skipped; the alias resolves straight into
    // externalReferences_ (cell identity, never a value snapshot).
    // State-side slots are compile-time-unvalued like captures — a
    // `var`'s record exists before any body writes it, so the render
    // check's value contract would refuse legitimate write-before-
    // read state. The fragment reads/writes the record's +d.values at
    // run time; the checker already proved the declared type.
    if (!capName(expression.text) && !found->second.stateSide) {
      const auto rendered = core_.render(found->second.cell);
      const auto value    = rendered.value_or(CellValue{});
      const bool matches =
          (*type == Scalar::Int &&
           std::holds_alternative<std::int64_t>(value)) ||
          (*type == Scalar::Float && std::holds_alternative<double>(value)) ||
          (*type == Scalar::Bool && std::holds_alternative<bool>(value)) ||
          (*type == Scalar::Str &&
           std::holds_alternative<std::string>(value)) ||
          (*type == Scalar::Unit &&
           std::holds_alternative<std::string>(value) &&
           std::get<std::string>(value).empty()) ||
          // A list head carries no value contract; the rank under +d.grab
          // is what the type describes.
          *type == Scalar::CellList ||
          // A Cell binding is a ref-box: the int content must name a live
          // cell.
          (*type == Scalar::Cell &&
           std::holds_alternative<std::int64_t>(value) &&
           core_.arena().contains(
               static_cast<CellRef>(std::get<std::int64_t>(value))));
      if (!matches)
        refuse(expression, DiagnosticCode::TypeMismatch,
               "binding value does not have its declared scalar type");
    } else if (capName(expression.text)) {
      // §5.3's capture class: references to a cap$N alias carry the
      // captured binding's own cell — operand wings for it are capture
      // slots (unvalued cells appended to the binding's +d.clone rank
      // tail, decoded via masterOf at run time), never int64 snapshots.
      // The recipe's capture member edges +d.values at the *slot*, not
      // the binding — a state-side holder's -d.values is
      // single-occupancy, and edging it would evict the var record's.
      captureCells_.insert(found->second.cell);
    }
    externalReferences_[&expression] = found->second.cell;
    return *type;
  }

  static bool numeric(Scalar type) {
    return type == Scalar::Int || type == Scalar::Float;
  }

  static void requireSame(const Expr &expression, Scalar left, Scalar right) {
    if (left != Scalar::Bottom && right != Scalar::Bottom && left != right)
      refuse(expression, DiagnosticCode::TypeMismatch,
             "scalar operands require the same static type");
  }

  static void conditionValue(const Expr &expression, Scalar type) {
    if (type == Scalar::Bool &&
        !(expression.kind == ExprKind::Binary && comparison(expression.text)))
      refuse(expression, DiagnosticCode::TypeMismatch,
             "a Bool search operand must be compared explicitly");
  }

  Scalar preflight(const Expr &expression, bool bounded, std::size_t depth,
                   bool pure = false, bool declaration = false,
                   ResultUse use = ResultUse::Value) {
    if (depth >= limits_.maxDepth || ++nodeCount_ > limits_.maxNodes)
      refuse(expression, DiagnosticCode::InvalidDeclaration,
             "scalar compiler syntax budget exceeded");
    if (expression.scope || expression.recipients || expression.deadline ||
        expression.guard || expression.recovery || expression.binding ||
        (expression.annotation && expression.kind != ExprKind::Let) ||
        (!expression.names.empty() && expression.kind != ExprKind::Record))
      refuse(expression, DiagnosticCode::InvalidDeclaration,
             "scalar lowering does not yet support expression metadata");
    for (const auto &child : expression.children)
      if (!child)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "scalar expression contains a missing child");
    const auto arity = [&](std::size_t expected) {
      if (expression.children.size() != expected)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "invalid scalar expression arity");
    };
    Scalar type = Scalar::Bottom;
    switch (expression.kind) {
    case ExprKind::Integer: {
      arity(0);
      const auto value = magnitude(expression);
      if (value >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        refuse(expression, DiagnosticCode::TypeMismatch,
               "integer literal is outside the Int range");
      literals_[&expression] = static_cast<std::int64_t>(value);
      type                   = Scalar::Int;
      break;
    }
    case ExprKind::Float: {
      arity(0);
      auto spelling = expression.text;
      std::erase(spelling, '_');
      double value{};
      const auto parsed = std::from_chars(
          spelling.data(), spelling.data() + spelling.size(), value);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != spelling.data() + spelling.size() ||
          !std::isfinite(value))
        refuse(expression, DiagnosticCode::TypeMismatch,
               "invalid finite Float literal");
      literals_[&expression] = value;
      type                   = Scalar::Float;
      break;
    }
    case ExprKind::String:
      arity(0);
      literals_[&expression] = expression.text;
      type                   = Scalar::Str;
      break;
    case ExprKind::Boolean:
      arity(0);
      if (expression.text != "true" && expression.text != "false")
        refuse(expression, DiagnosticCode::TypeMismatch,
               "invalid Bool literal");
      literals_[&expression] = expression.text == "true";
      type                   = Scalar::Bool;
      break;
    case ExprKind::Unit:
      arity(0);
      type = Scalar::Unit;
      break;
    case ExprKind::Identifier:
      arity(0);
      type = binding(expression);
      break;
    case ExprKind::Fail:
      arity(0);
      break;
    case ExprKind::Unary: {
      arity(1);
      const auto &operand = *expression.children.front();
      if (expression.text == "-" && operand.kind == ExprKind::Integer) {
        // The signed-minimum literal cannot first exist as a positive Int cell.
        const auto value            = magnitude(operand);
        const auto minimumMagnitude = std::uint64_t{1} << 63;
        if (value == minimumMagnitude && operand.children.empty() &&
            !operand.scope && !operand.recipients && !operand.deadline &&
            !operand.guard && !operand.recovery && !operand.binding &&
            !operand.annotation && operand.names.empty()) {
          if (depth + 1 >= limits_.maxDepth || ++nodeCount_ > limits_.maxNodes)
            refuse(operand, DiagnosticCode::InvalidDeclaration,
                   "scalar compiler syntax budget exceeded");
          literals_[&expression] = std::numeric_limits<std::int64_t>::min();
          type                   = Scalar::Int;
          break;
        }
      }
      const auto value = preflight(operand, false, depth + 1, pure);
      if (expression.text == "-") {
        if (!numeric(value) && value != Scalar::Bottom)
          refuse(expression, DiagnosticCode::TypeMismatch,
                 "unary '-' requires a number");
        type = value;
      } else if (expression.text == "not") {
        conditionValue(operand, value);
        type = Scalar::Unit;
      } else {
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "scalar lowering supports only unary '-' and 'not'");
      }
      break;
    }
    case ExprKind::Binary: {
      arity(2);
      const auto &left      = *expression.children[0];
      const auto &right     = *expression.children[1];
      const auto &op        = expression.text;
      const bool assignment = op == ":=" || op == "<-";
      if (assignment && pure)
        refuse(expression, DiagnosticCode::InvalidPosition,
               "conditions cannot contain assignments");
      if (assignment && left.kind != ExprKind::Identifier)
        refuse(left, DiagnosticCode::InvalidDeclaration,
               "scalar assignment requires an identifier binding");
      if (op == ":=" && !bounded)
        refuse(expression, DiagnosticCode::InvalidPosition,
               "':=' requires a bounded statement position");
      const auto leftType  = preflight(left, false, depth + 1, pure);
      const auto rightType = preflight(right, false, depth + 1, pure);
      if (assignment) {
        requireSame(expression, leftType, rightType);
        // Only a cells literal can write a list head under the binding; a
        // generic CellList value would copy content and drop the rank.
        if (rightType == Scalar::CellList && right.kind != ExprKind::Cells)
          refuse(expression, DiagnosticCode::InvalidDeclaration,
                 "cell lists assign only from a cells path, which writes the "
                 "rank under the binding cell");
        type = rightType == Scalar::Bottom ? Scalar::Bottom : Scalar::Unit;
      } else if (op == "?") {
        if (leftType != Scalar::Bottom && leftType != Scalar::Str)
          refuse(left, DiagnosticCode::TypeMismatch,
                 "'?' requires a string subject on the left");
        type = rightType;
        if (const auto it = resultTypes_.find(&right); it != resultTypes_.end())
          resultTypes_[&expression] = it->second;
      } else if (op == "&") {
        conditionValue(left, leftType);
        conditionValue(right, rightType);
        type = leftType == Scalar::Bottom ? Scalar::Bottom : rightType;
      } else if (op == "|") {
        conditionValue(left, leftType);
        conditionValue(right, rightType);
        requireSame(expression, leftType, rightType);
        type = leftType == Scalar::Bottom ? rightType : leftType;
      } else {
        requireSame(expression, leftType, rightType);
        const auto effective =
            leftType == Scalar::Bottom ? rightType : leftType;
        if (comparison(op)) {
          if (effective == Scalar::CellList)
            refuse(expression, DiagnosticCode::TypeMismatch,
                   "cell lists do not support comparison; compare elements");
          if (op != "==" && op != "!=" && effective != Scalar::Bottom &&
              effective != Scalar::Int && effective != Scalar::Float &&
              effective != Scalar::Str)
            refuse(expression, DiagnosticCode::TypeMismatch,
                   "ordered comparison requires numbers or strings");
          type = leftType == Scalar::Bottom || rightType == Scalar::Bottom
                     ? Scalar::Bottom
                     : rightType;
        } else if (op == "++") {
          if (effective != Scalar::Str && effective != Scalar::Bottom)
            refuse(expression, DiagnosticCode::TypeMismatch,
                   "scalar '++' requires strings");
          type = leftType == Scalar::Bottom || rightType == Scalar::Bottom
                     ? Scalar::Bottom
                     : Scalar::Str;
        } else if (op == "+" || op == "-" || op == "*" || op == "/" ||
                   op == "%") {
          if ((!numeric(effective) && effective != Scalar::Bottom) ||
              (op == "%" && effective == Scalar::Float))
            refuse(expression, DiagnosticCode::TypeMismatch,
                   "scalar arithmetic requires matching numeric operands");
          type = leftType == Scalar::Bottom || rightType == Scalar::Bottom
                     ? Scalar::Bottom
                     : effective;
        } else {
          refuse(expression, DiagnosticCode::InvalidDeclaration,
                 "scalar lowering does not yet support operator '" + op + "'");
        }
      }
      break;
    }
    case ExprKind::Block: {
      scopes_.emplace_back();
      type        = Scalar::Unit;
      bool failed = false;
      for (const auto &statement : expression.children) {
        const auto childUse =
            statement.get() == expression.children.back().get()
                ? use
                : ResultUse::Discard;
        const auto next =
            preflight(*statement, bounded, depth + 1, pure, true, childUse);
        failed = failed || next == Scalar::Bottom;
        type   = next;
      }
      scopes_.pop_back();
      if (failed) type = Scalar::Bottom;
      // A block's semantic result type is its last statement's — a
      // `run` or code"…" tail carries the richer Type, not the scalar.
      if (!expression.children.empty()) {
        const auto &last = *expression.children.back();
        if (const auto found = resultTypes_.find(&last);
            found != resultTypes_.end())
          resultTypes_[&expression] = found->second;
      }
      break;
    }
    case ExprKind::Let: {
      arity(1);
      if (!declaration || scopes_.empty())
        refuse(expression, DiagnosticCode::InvalidPosition,
               "scalar locals must be direct block statements");
      const auto validName = [](std::string_view name) {
        return !name.empty() && name != "_" &&
               (name.front() == '_' ||
                (name.front() >= 'a' && name.front() <= 'z')) &&
               std::ranges::all_of(name, [](char c) {
                 return c == '_' || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9');
               });
      };
      if (!validName(expression.text))
        refuse(expression, DiagnosticCode::InvalidIdentifier,
               "scalar local names must be snake_case and cannot be '_'");
      auto value =
          preflight(*expression.children.front(), false, depth + 1, pure);
      if (expression.annotation) {
        const auto annotated = scalarType(*expression.annotation);
        if (!annotated)
          refuse(expression, DiagnosticCode::InvalidDeclaration,
                 "scalar local annotation is unsupported");
        requireSame(expression, value, *annotated);
        value = *annotated;
      }
      if (scopes_.back().contains(expression.text))
        refuse(expression, DiagnosticCode::DuplicateName,
               "duplicate local '" + expression.text + "'");
      const auto index = localCount_++;
      {
        Local local{index, value};
        local.init = expression.children.front().get();
        if (const auto rich =
                resultTypes_.find(expression.children.front().get());
            rich != resultTypes_.end())
          local.rich = rich->second;
        scopes_.back().emplace(expression.text, local);
      }
      localReferences_[&expression] = index;
      type = types_.at(expression.children.front().get()) == Scalar::Bottom
                 ? Scalar::Bottom
                 : Scalar::Unit;
      break;
    }
    case ExprKind::If: {
      if (expression.children.size() != 2 && expression.children.size() != 3)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "invalid scalar conditional arity");
      const auto test =
          preflight(*expression.children[0], false, depth + 1, true);
      conditionValue(*expression.children[0], test);
      const auto thenType =
          preflight(*expression.children[1], bounded, depth + 1, pure, false,
                    expression.children.size() == 2 ? ResultUse::Value : use);
      const auto elseType = expression.children.size() == 3
                                ? preflight(*expression.children[2], bounded,
                                            depth + 1, pure, false, use)
                                : Scalar::Unit;
      if (expression.children.size() == 2 && thenType != Scalar::Unit &&
          thenType != Scalar::Bottom)
        refuse(expression, DiagnosticCode::TypeMismatch,
               "a conditional without else requires a Unit then arm");
      if (thenType != Scalar::Bottom && elseType != Scalar::Bottom &&
          thenType != elseType)
        refuse(expression, DiagnosticCode::TypeMismatch,
               "value-producing conditional arms require a common type");
      type = thenType == Scalar::Bottom ? elseType : thenType;
      break;
    }
    case ExprKind::Match: {
      // Scalar `match`: literal, wildcard and `?name` patterns over an
      // Int/Float/Str/Bool scrutinee. Lowered to an Eq/Branch decision
      // chain — no Choice or Fail is minted for pattern tests, so an
      // arm's side effects cannot be undone by a later rollback.
      if (expression.children.size() < 2)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "a match needs a scrutinee and at least one arm");
      const auto subject =
          preflight(*expression.children[0], false, depth + 1, pure);
      if (subject == Scalar::Bottom || subject == Scalar::Unit ||
          subject == Scalar::Cell || subject == Scalar::CellList)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "match lowering supports Int, Float, Str and Bool scrutinees");
      Scalar result  = Scalar::Bottom;
      bool sawResult = false;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        const auto &arm = *expression.children[i];
        if (arm.kind != ExprKind::MatchArm || arm.children.size() != 2 ||
            !arm.children[0] || !arm.children[1])
          refuse(arm, DiagnosticCode::InvalidDeclaration,
                 "malformed match arm");
        scopes_.emplace_back();
        const auto &pattern = *arm.children[0];
        switch (pattern.kind) {
        case ExprKind::WildcardPattern:
          break;
        case ExprKind::LogicVariable: {
          const auto index = localCount_++;
          scopes_.back().emplace(pattern.text, Local{index, subject});
          localReferences_[&pattern] = index;
          break;
        }
        case ExprKind::Integer:
        case ExprKind::Float:
        case ExprKind::String:
        case ExprKind::Boolean: {
          const auto patternType = preflight(pattern, false, depth + 1, true);
          requireSame(pattern, subject, patternType);
          break;
        }
        default:
          refuse(pattern, DiagnosticCode::InvalidDeclaration,
                 "match lowering supports literal, wildcard and variable "
                 "patterns");
        }
        if (arm.guard) {
          const auto &guard = *arm.guard;
          if (guard.kind != ExprKind::Binary || !comparison(guard.text) ||
              guard.children.size() != 2)
            refuse(guard, DiagnosticCode::TypeMismatch,
                   "a match guard must be an explicit comparison");
          // The guard is a pure test: the Binary arm's own preflight would
          // refuse its metadata-free node only through its children, so the
          // operands are checked directly and the node typed as Bool.
          const auto left =
              preflight(*guard.children[0], false, depth + 1, true);
          const auto right =
              preflight(*guard.children[1], false, depth + 1, true);
          requireSame(guard, left, right);
          types_[&guard] = Scalar::Bool;
        }
        const auto bodyType =
            preflight(*arm.children[1], bounded, depth + 1, pure, false, use);
        if (bodyType != Scalar::Bottom) {
          if (sawResult && bodyType != result)
            refuse(arm, DiagnosticCode::TypeMismatch,
                   "match arms require a common result type");
          result    = bodyType;
          sawResult = true;
        }
        scopes_.pop_back();
      }
      type = result;
      break;
    }
    case ExprKind::Cells: {
      arity(0);
      auto path = parseCellsPath(expression.text, expression.loc);
      if (!path) {
        const auto diagnostic = path.error();
        refuse(expression, diagnostic.code, diagnostic.detail);
      }
      if (pure && path->mutating)
        refuse(expression, DiagnosticCode::InvalidPosition,
               "cells paths that create are not allowed in conditions");
      if (path->anchor.kind == CellsPath::Anchor::Kind::Variable) {
        const auto found = bindings_.find(path->anchor.variable);
        if (found == bindings_.end())
          refuse(expression, DiagnosticCode::UnknownName,
                 "cells anchor '$" + path->anchor.variable + "' is not bound");
        if (scalarType(found->second.type) != Scalar::Cell)
          refuse(expression, DiagnosticCode::TypeMismatch,
                 "cells anchor '$" + path->anchor.variable +
                     "' must be declared Cell");
        if (path->mutating)
          refuse(expression, DiagnosticCode::InvalidDeclaration,
                 "cells paths anchored on variables cannot create");
        if (!core_.arena().contains(found->second.cell))
          refuse(expression, DiagnosticCode::UnknownName,
                 "binding cell is not present in this arena");
        // §5.3: cap_N anchors resolve only on stable captures — a
        // frame-bound capture has no emission-time cell to name, so the
        // state-side bit is the discriminator.
        if (capName(path->anchor.variable) && !found->second.stateSide)
          refuse(expression, DiagnosticCode::InvalidDeclaration,
                 "cells anchor '$" + path->anchor.variable +
                     "' names a frame-bound capture; anchors resolve on "
                     "stable captures only");
        externalReferences_[&expression] = found->second.cell;
      }
      if (path->anchor.kind == CellsPath::Anchor::Kind::LiteralCell &&
          !core_.arena().contains(CellRef(path->anchor.cellId)))
        refuse(expression, DiagnosticCode::UnknownName,
               "cells anchor cell is not present in this arena");
      paths_[&expression] = std::move(*path);
      type                = Scalar::CellList;
      break;
    }
    case ExprKind::Record: {
      // `S{f: v, …}` — the value is the marshaled term cell's ref: a
      // cell whose +d.step rank carries +d.name-labelled field cells,
      // the shape #FIND reads and belief facts unify against. The
      // marshal is static — literals and nested terms/records only,
      // emit's operand marshal holds the same boundary — so the cell
      // mints here and emit copies its ref out.
      auto marshaled = marshalTermExpr(core_.arena(), core_, expression);
      if (!marshaled)
        refuse(expression, marshaled.error().code, marshaled.error().detail);
      recordCells_[&expression] = *marshaled;
      type                      = Scalar::Cell;
      break;
    }
    case ExprKind::Field: {
      // `term.field` — the base is a cell ref-box whose +d.step rank the
      // #FIND op walks for the member's +d.name. The emitted value is
      // whatever the member renders, so the static type is Bottom: the
      // checker already proved the field exists on the declared type.
      arity(1);
      const auto base =
          preflight(*expression.children.front(), false, depth + 1, pure);
      if (base != Scalar::Cell && base != Scalar::Bottom)
        refuse(expression, DiagnosticCode::TypeMismatch,
               "field access requires a cell-bound base");
      type = Scalar::Bottom;
      break;
    }
    case ExprKind::Static: {
      // §5.5: the operand was checked pure+closed with a
      // literal-expressible scalar type; the kernel re-verifies the type
      // arm and evaluates at emit time.
      arity(1);
      const auto value =
          preflight(*expression.children.front(), false, depth + 1, pure);
      if (value != Scalar::Int && value != Scalar::Float &&
          value != Scalar::Str && value != Scalar::Bool &&
          value != Scalar::Unit)
        refuse(expression, DiagnosticCode::TypeMismatch,
               "'static' operand must have a literal-expressible scalar "
               "type (Int, Float, Str, Bool or ())");
      // §5.5 inside a fragment: a `static` is judged in the context the
      // parameter lands in, and a parameter is unvalued there — the
      // run/graft emission has no activation to evaluate against, so
      // `static(param)` is a named emission refusal.
      if (paramCount_ != 0 && referencesParam(*expression.children.front()))
        refuse(expression, DiagnosticCode::InvalidPosition,
               "'static' operand inside a fragment references a "
               "parameter — parameters are unvalued at the run/graft "
               "emission the operand is judged in");
      // §5.5's closed-name channel: the operand's free names may only
      // be closed `let`s — snapshot the in-scope set so emit's overlay
      // sees them after scopes_ pops (inner scopes shadow outer, the
      // resolver's own order).
      {
        auto &snapshot = staticCaptures_[&expression];
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
          for (const auto &[name, local] : *scope)
            snapshot.emplace(name, local);
      }
      type = value;
      break;
    }
    case ExprKind::Code: {
      // §6.1: a surviving code"…" literal lowers to a real fragment
      // object — head + descriptor + body graph — minted by
      // compileFragment at emit time and read as a head-valued ref.
      arity(0);
      if (!fragments_)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "a code\"…\" literal reached lowering without the "
               "expansion/check tables — supply the FragmentContext");
      const auto record = fragmentRecord(expression);
      if (!record)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "code\"…\" literal has no fragment-table record; the "
               "expand stage must run before compile");
      const auto *meta = fragmentMeta(expression);
      if (meta && meta->spliceOnly)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "a splice-only fragment (parameterized by a Code "
               "parameter) emits no head and no descriptor — it may "
               "appear only as a splice or template actual");
      const auto *info = fragmentInfo(expression);
      if (!info)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "code\"…\" literal has no checked fragment info; the "
               "check stage must run before compile");
      // §5.3's frame-local channel: the literal's cap$N captures join
      // to this scope's `let`/parameter cells at emit — preflight pops
      // scopes_ before emit runs, so the in-scope set is snapshotted
      // per literal here (inner scopes shadow outer, matching the
      // resolver's own lookup order).
      {
        auto &snapshot = frameCaptures_[&expression];
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
          for (const auto &[name, local] : *scope)
            snapshot.emplace(name, local);
      }
      // A Code value is a ref-box: the cell content int64 names the
      // fragment head, so it rides the Cell scalar mechanically while
      // the descriptor carries the real Code<T>.
      resultTypes_[&expression] = Type{"Code", {info->result}};
      type                      = Scalar::Cell;
      break;
    }
    case ExprKind::Call: {
      // §6.2's `run` special form and member `def` calls — the latter
      // resolve through the methods table the member pre-pass minted:
      // the callsite names the module symbol and RunCall hops
      // `sym +d.values → head` at run, so the emit needs only the
      // declared signature here. Every other callee stays unsupported.
      const auto &callee = *expression.children.front();
      if (callee.kind == ExprKind::Identifier && callee.text != "run") {
        if (isBuiltinCall(callee.text)) {
          const std::size_t arity = expression.children.size() - 1;
          if (callee.text == "str_len") {
            if (arity != 1)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'str_len' takes 1 argument");
            preflight(*expression.children[1], false, depth + 1, pure);
            type                      = Scalar::Int;
            resultTypes_[&expression] = Type{"Int"};
            break;
          }
          if (callee.text == "byte_at") {
            if (arity != 2)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'byte_at' takes 2 arguments");
            preflight(*expression.children[1], false, depth + 1, pure);
            preflight(*expression.children[2], false, depth + 1, pure);
            type                      = Scalar::Int;
            resultTypes_[&expression] = Type{"Int"};
            break;
          }
          if (callee.text == "str_slice") {
            if (arity != 3)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'str_slice' takes 3 arguments");
            preflight(*expression.children[1], false, depth + 1, pure);
            preflight(*expression.children[2], false, depth + 1, pure);
            preflight(*expression.children[3], false, depth + 1, pure);
            type                      = Scalar::Str;
            resultTypes_[&expression] = Type{"Str"};
            break;
          }
          if (callee.text == "int_to_str") {
            if (arity != 1)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'int_to_str' takes 1 argument");
            preflight(*expression.children[1], false, depth + 1, pure);
            type                      = Scalar::Str;
            resultTypes_[&expression] = Type{"Str"};
            break;
          }
          if (callee.text == "str_to_int") {
            if (arity != 1)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'str_to_int' takes 1 argument");
            preflight(*expression.children[1], false, depth + 1, pure);
            type                      = Scalar::Int;
            resultTypes_[&expression] = Type{"Int"};
            break;
          }
          if (callee.text == "abs") {
            if (arity != 1)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'abs' takes 1 argument");
            const auto argType =
                preflight(*expression.children[1], false, depth + 1, pure);
            type = (argType == Scalar::Float ? Scalar::Float : Scalar::Int);
            resultTypes_[&expression] =
                (type == Scalar::Float ? Type{"Float"} : Type{"Int"});
            break;
          }
          if (callee.text == "min" || callee.text == "max") {
            if (arity != 2)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'" + std::string(callee.text) + "' takes 2 arguments");
            const auto leftType =
                preflight(*expression.children[1], false, depth + 1, pure);
            const auto rightType =
                preflight(*expression.children[2], false, depth + 1, pure);
            type = (leftType == Scalar::Float || rightType == Scalar::Float
                        ? Scalar::Float
                        : Scalar::Int);
            resultTypes_[&expression] =
                (type == Scalar::Float ? Type{"Float"} : Type{"Int"});
            break;
          }
          if (callee.text == "clamp") {
            if (arity != 3)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'clamp' takes 3 arguments");
            const auto valType =
                preflight(*expression.children[1], false, depth + 1, pure);
            (void)preflight(*expression.children[2], false, depth + 1, pure);
            (void)preflight(*expression.children[3], false, depth + 1, pure);
            type = (valType == Scalar::Float ? Scalar::Float : Scalar::Int);
            resultTypes_[&expression] =
                (type == Scalar::Float ? Type{"Float"} : Type{"Int"});
            break;
          }
          if (callee.text == "trim" || callee.text == "to_lower" ||
              callee.text == "to_upper") {
            if (arity != 1)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'" + std::string(callee.text) + "' takes 1 argument");
            preflight(*expression.children[1], false, depth + 1, pure);
            type                      = Scalar::Str;
            resultTypes_[&expression] = Type{"Str"};
            break;
          }
          if (callee.text == "head_of") {
            if (arity != 1)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'head_of' takes 1 argument");
            preflight(*expression.children[1], false, depth + 1, pure);
            type                      = Scalar::Cell;
            resultTypes_[&expression] = Type{"Cell"};
            break;
          }
          if (callee.text == "make_cell") {
            if (pure)
              refuse(expression, DiagnosticCode::InvalidPosition,
                     "'make_cell()' is opaque-impure — it cannot appear in a "
                     "condition");
            if (arity != 1)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'make_cell' takes 1 argument");
            preflight(*expression.children[1], false, depth + 1, pure);
            type                      = Scalar::Cell;
            resultTypes_[&expression] = Type{"Cell"};
            break;
          }
          if (callee.text == "program" || callee.text == "agent_cell") {
            if (arity != 0)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'" + std::string(callee.text) + "' takes 0 arguments");
            type                      = Scalar::Cell;
            resultTypes_[&expression] = Type{"Cell"};
            break;
          }
          if (callee.text == "boot_writer" || callee.text == "boot_linker") {
            if (arity != 0)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'" + std::string(callee.text) + "' takes 0 arguments");
            type                      = Scalar::Cell;
            resultTypes_[&expression] = Type{"Code", {Type{"Cell"}}};
            break;
          }
          if (callee.text == "tab" || callee.text == "move") {
            if (arity != 1)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'" + std::string(callee.text) + "' takes 1 argument");
            preflight(*expression.children[1], false, depth + 1, pure);
            type                      = Scalar::Str;
            resultTypes_[&expression] = Type{"Str"};
            break;
          }
          if (callee.text == "match" || callee.text == "many" ||
              callee.text == "upto" || callee.text == "any" ||
              callee.text == "find") {
            if (arity != 1)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'" + std::string(callee.text) + "' takes 1 argument");
            preflight(*expression.children[1], false, depth + 1, pure);
            type                      = Scalar::Int;
            resultTypes_[&expression] = Type{"Int"};
            break;
          }
          if (callee.text == "pos") {
            if (arity > 1)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'pos' takes at most 1 argument");
            if (arity == 1)
              preflight(*expression.children[1], false, depth + 1, pure);
            type                      = Scalar::Int;
            resultTypes_[&expression] = Type{"Int"};
            break;
          }
          if (callee.text == "subject") {
            if (arity != 0)
              refuse(expression, DiagnosticCode::TypeMismatch,
                     "'subject' takes 0 arguments");
            type                      = Scalar::Str;
            resultTypes_[&expression] = Type{"Str"};
            break;
          }
        }
        const MethodTarget *target = nullptr;
        if (fragments_ && fragments_->methods)
          if (const auto it = fragments_->methods->find(callee.text);
              it != fragments_->methods->end())
            target = &it->second;
        if (!target)
          refuse(expression, DiagnosticCode::InvalidDeclaration,
                 "scalar lowering resolves calls only against member "
                 "`def`s — '" +
                     std::string(callee.text) + "'");
        if (pure)
          refuse(expression, DiagnosticCode::InvalidPosition,
                 "'" + std::string(callee.text) +
                     "()' is opaque-impure — it cannot appear in a "
                     "condition");
        for (std::size_t i = 1; i < expression.children.size(); ++i)
          preflight(*expression.children[i], false, depth + 1, pure);
        resultTypes_[&expression] = target->result;
        if (const auto scalar = scalarType(target->result)) {
          type = *scalar;
        } else if (target->result.name == "Code") {
          // A method returning Code — the produced value is a
          // head-valued ref, carried as a Cell scalar.
          type = Scalar::Cell;
        } else {
          refuse(expression, DiagnosticCode::TypeMismatch,
                 "'" + std::string(callee.text) +
                     "' result type is outside the scalar lowering "
                     "subset: " +
                     typeName(target->result));
        }
        break;
      }
      if (callee.kind != ExprKind::Identifier || callee.text != "run")
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "scalar lowering does not yet support calls — only the "
               "'run' special form");
      if (pure)
        refuse(expression, DiagnosticCode::InvalidPosition,
               "'run' is opaque-impure — it cannot appear in a "
               "condition");
      if (!fragments_)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "'run' reached lowering without the expansion/check "
               "tables — supply the FragmentContext");
      if (expression.children.size() < 2 || expression.children.size() > 3)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "'run' takes a fragment and an optional argument list");
      const auto &operandExpr = *expression.children[1];
      if (operandExpr.kind != ExprKind::Code) {
        // §6.2's dynamic run — a Code value rather than a literal.
        // The RunCall/RunResume pair carries the call: arity and the
        // produced/result gate read the head's descriptor at run time,
        // so preflight only proves the operand and the arg list are
        // emittable. A statically-known Code<T> operand gives back T —
        // the richer resultTypes_ channel carries it; Bottom is the
        // honest report only when the operand's fragment type is
        // genuinely unknown.
        (void)preflight(operandExpr, false, depth + 1, pure);
        if (expression.children.size() == 3) {
          const auto &argsExpr = *expression.children[2];
          if (argsExpr.kind != ExprKind::List)
            refuse(argsExpr, DiagnosticCode::TypeMismatch,
                   "'run' argument list must be a list literal");
          for (const auto &item : argsExpr.children)
            preflight(*item, false, depth + 1, pure);
        }
        type = Scalar::Bottom;
        if (const auto found = resultTypes_.find(&operandExpr);
            found != resultTypes_.end() && found->second.name == "Code" &&
            found->second.arguments.size() == 1) {
          const auto &result        = found->second.arguments.front();
          resultTypes_[&expression] = result;
          if (const auto scalar = scalarType(result))
            type = *scalar;
          else if (result.name == "Code")
            type = Scalar::Cell;
        }
        break;
      }
      const auto operandType = preflight(operandExpr, false, depth + 1, pure);
      (void)operandType; // the Code arm already checked its record
      const auto *record = fragmentRecord(operandExpr);
      const auto *info   = fragmentInfo(operandExpr);
      const auto *meta   = fragmentMeta(operandExpr);
      if (meta && meta->spliceOnly)
        refuse(expression, DiagnosticCode::InvalidDeclaration,
               "'run' cannot execute a splice-only fragment — it has "
               "no value-level parameter to receive");
      const std::size_t declared = record->body.parameters.size();
      if (expression.children.size() == 3) {
        const auto &argsExpr = *expression.children[2];
        if (argsExpr.kind != ExprKind::List)
          refuse(argsExpr, DiagnosticCode::TypeMismatch,
                 "'run' argument list must be a list literal");
        if (argsExpr.children.size() != declared)
          refuse(expression, DiagnosticCode::TypeMismatch,
                 "'run' argument count differs from the fragment's "
                 "declared arity");
        // §6.2: the argument list is heterogeneous — each element is
        // typed per-element by `any`, so no common-type join applies.
        for (const auto &item : argsExpr.children)
          preflight(*item, false, depth + 1, pure);
      } else if (declared != 0) {
        refuse(expression, DiagnosticCode::TypeMismatch,
               "'run' argument count differs from the fragment's "
               "declared arity");
      }
      const auto resultScalar   = scalarType(info->result);
      resultTypes_[&expression] = info->result;
      if (resultScalar) {
        type = *resultScalar;
      } else if (info->result.name == "Code") {
        // A fragment whose result is itself Code — the produced value
        // is a head-valued ref, carried as a Cell scalar.
        type = Scalar::Cell;
      } else {
        refuse(expression, DiagnosticCode::TypeMismatch,
               "'run' result type is outside the scalar lowering "
               "subset: " +
                   typeName(info->result));
      }
      break;
    }
    case ExprKind::Splice:
      refuse(expression, DiagnosticCode::InvalidDeclaration,
             "a residual splice reached lowering; the expand stage "
             "substitutes every splice before compile");
    default:
      refuse(expression, DiagnosticCode::InvalidDeclaration,
             "construct is not supported by the scalar lowering iteration");
    }
    if (use == ResultUse::Discard) {
      discarded_.insert(&expression);
      if (type != Scalar::Bottom) type = Scalar::Unit;
    }
    types_[&expression] = type;
    return type;
  }

  // -- Emission bookkeeping (§6.1) ------------------------------------
  // Every cell this Emitter mints lands in own_ so a bound operand's
  // referent class resolves to internal (rebinds through the old→new
  // reify map) versus external/state-side (carried verbatim) — the
  // map-membership discriminator the recipe replays on.
  CellRef own(CellRef cell) {
    own_.insert(cell);
    return cell;
  }
  // The +d.recipe link-tuple source — recorded only while a fragment
  // body is being emitted (body_ is set); a statement image leaves the
  // cells unrecorded, matching the unmarked program refs they are. The
  // `from` endpoint may be an external/state-side cell (a materialized
  // cells-path rank under a binding head) — the tuple still records,
  // with referent classification distinguishing endpoints at replay.
  void recLink(CellRef from, zigzag::DimRef dim, bool neg, CellRef to) {
    if (body_)
      body_->links.push_back(
          {from, dim, neg, to, std::string{linkDimName(dim)}});
  }
  // Recipe link tuples carry the dim's *name* — identity re-resolves
  // by name at genesis. The emission only ever links on core dims and
  // every canonical name is the mintNamedDimension filing verbatim.
  std::string_view linkDimName(zigzag::DimRef dim) const {
    const auto &d = core_.dims();
    if (dim == d.grab) return "d.grab";
    if (dim == d.step) return "d.step";
    if (dim == d.spin) return "d.spin";
    if (dim == d.values) return "d.values";
    if (dim == d.clone) return "d.clone";
    if (dim == d.vars) return "d.vars";
    if (dim == d.name) return "d.name";
    if (dim == d.stack) return "d.stack";
    return {};
  }
  void recLiteral(CellRef cell, const CellValue &value) {
    if (body_) body_->literals.push_back({cell, value});
  }
  void recRef(CellRef cell, EmissionRef::Referent referent, std::int64_t target,
              std::string dimName = {}) {
    if (body_)
      body_->refs.push_back(
          {cell, std::nullopt, {}, referent, target, std::move(dimName)});
  }
  void recRefNamed(CellRef cell, std::string name, std::string role,
                   std::int64_t target) {
    if (!body_) return;
    // A cell that carries the same re-resolvable ref twice does not
    // get two name-records.
    for (const auto &entry : body_->refs)
      if (entry.cell == cell && entry.name && *entry.name == name &&
          entry.role == role)
        return;
    // Name-record referents classify like every other ref: the head a
    // callsite names is external to this body's coverage set (its own
    // fragment object), so the rebind is verbatim and the +d.name label
    // is the genesis re-resolution channel.
    body_->refs.push_back({cell,
                           std::move(name),
                           std::move(role),
                           classify(static_cast<CellRef>(target)),
                           target,
                           {}});
  }

  // VortexCore::bindInput/bindOutput's shapes with the slot + wing links
  // recorded — bindInput mints an int64 slot naming the operand, links
  // it +d.grab-first then +d.step-chained on the wing.
  CellRef bindIn(CellRef op, CellRef operand) {
    const auto slot =
        own(core_.arena().makeScalarCell(static_cast<std::int64_t>(operand)));
    wing(op, slot, false);
    recRef(slot, classify(operand), static_cast<std::int64_t>(operand));
    return slot;
  }
  CellRef bindOut(CellRef op, CellRef operand) {
    const auto slot =
        own(core_.arena().makeScalarCell(static_cast<std::int64_t>(operand)));
    wing(op, slot, true);
    recRef(slot, classify(operand), static_cast<std::int64_t>(operand));
    return slot;
  }
  // §5.3's capture slot: an int64(binding) slot appended to the
  // binding's own +d.clone rank tail — wingOf decodes the int64 at run
  // time exactly like an ordinary slot, and the clone-rank membership
  // is what reify replays (the ref-capture record's +d.values→binding
  // arm re-appends the slot twin onto the *same* binding — a captured
  // cell is shared, never copied). The clone-rank append itself is
  // bookkeeping, not a recipe link tuple.
  CellRef bindInCapture(CellRef op, CellRef binding) {
    const auto slot =
        own(core_.arena().makeScalarCell(static_cast<std::int64_t>(binding)));
    const auto tail =
        zigzag::rankTail(core_.arena(), binding, core_.dims().clone);
    zigzag::expectWritten(core_.arena().link(tail, core_.dims().clone,
                                             zigzag::DimVector::POS, slot));
    wing(op, slot, false);
    if (body_) body_->captures.push_back({slot, binding});
    return slot;
  }
  void wing(CellRef op, CellRef slot, bool output) {
    const auto &dims = core_.dims();
    const auto dir   = output ? zigzag::DimVector::NEG : zigzag::DimVector::POS;
    const auto first = core_.arena().linked(op, dims.grab, dir);
    if (!first) {
      recLink(op, dims.grab, output, slot);
      zigzag::expectWritten(core_.arena().link(op, dims.grab, dir, slot));
      return;
    }
    const auto tail = zigzag::rankTail(core_.arena(), *first, dims.step);
    recLink(tail, dims.step, false, slot);
    zigzag::expectWritten(
        core_.arena().link(tail, dims.step, zigzag::DimVector::POS, slot));
  }
  EmissionRef::Referent classify(CellRef operand) const {
    if (const auto dim = dimNames_.find(operand); dim != dimNames_.end())
      return EmissionRef::Referent::Dim;
    if (!own_.contains(operand)) return EmissionRef::Referent::External;
    return EmissionRef::Referent::Internal;
  }

  CellRef mint(OpcodeKind kind) {
    const auto op = own(vm_.mintOpcode(kind, "vagent.scalar"));
    recLiteral(op, static_cast<std::int64_t>(kind));
    if (recordOps_ && compiler_) compiler_->recordProgramOp(op);
    return op;
  }
  CellRef temporary() {
    const auto cell = own(core_.arena().makeCell());
    // An unvalued literal record — every own_-minted cell must reach
    // the recipe or a ref-capture so reify twins it; a slot would
    // otherwise rebind to the *original* temp.
    if (body_) body_->literals.push_back({cell, std::nullopt});
    return cell;
  }
  CellRef scalar(std::int64_t value) {
    const auto cell = own(core_.arena().makeScalarCell(value));
    recLiteral(cell, value);
    return cell;
  }
  CellRef scalar(bool value) {
    const auto cell = own(core_.arena().makeScalarCell(value));
    recLiteral(cell, value);
    return cell;
  }
  CellRef scalar(double value) {
    const auto cell = own(core_.arena().makeScalarCell(value));
    recLiteral(cell, value);
    return cell;
  }
  CellRef textCell(std::string_view text) {
    const auto cell = own(core_.arena().makeCell(text));
    recLiteral(cell, std::string(text));
    return cell;
  }
  CellRef reference(CellRef target) {
    const auto cell =
        own(core_.arena().makeScalarCell(static_cast<std::int64_t>(target)));
    recRef(cell, classify(target), static_cast<std::int64_t>(target));
    return cell;
  }
  // A head-valued convention cell — the operand slot that stays
  // int64(head), never the Call operand itself. `role` is the
  // name-record's +d.grab label: "run" (head-valued) or "call"
  // (entryOp-valued writeback).
  CellRef namedRef(CellRef target, std::string name, std::string role) {
    const auto cell =
        own(core_.arena().makeScalarCell(static_cast<std::int64_t>(target)));
    recRefNamed(cell, std::move(name), std::move(role),
                static_cast<std::int64_t>(target));
    return cell;
  }
  // An int64(dim) constant — a dim referent: its ref-capture record
  // carries +d.name→the dim name so genesis re-resolves by name.
  CellRef dimRef(zigzag::DimRef dim, std::string_view name) {
    const auto cell =
        own(core_.arena().makeScalarCell(static_cast<std::int64_t>(dim)));
    dimNames_[cell] = std::string(name);
    recRef(cell, EmissionRef::Referent::Dim, static_cast<std::int64_t>(dim),
           std::string(name));
    return cell;
  }
  void spin(CellRef from, CellRef to) {
    recLink(from, core_.dims().spin, false, to);
    zigzag::expectWritten(core_.arena().link(from, core_.dims().spin,
                                             zigzag::DimVector::POS, to));
  }
  void link(CellRef from, zigzag::DimRef dim, bool neg, CellRef to) {
    recLink(from, dim, neg, to);
    zigzag::expectWritten(core_.arena().link(
        from, dim, neg ? zigzag::DimVector::NEG : zigzag::DimVector::POS, to));
  }
  CellRef jump(CellRef target) {
    const auto op = mint(OpcodeKind::Jump);
    bindIn(op, reference(target));
    return op;
  }
  CellRef copy(CellRef target, CellRef source, CellRef continuation) {
    return copyValue(target, source, continuation, false);
  }
  CellRef copyValue(CellRef target, CellRef source, CellRef continuation,
                    bool captureSource) {
    const auto op = mint(OpcodeKind::Value);
    bindIn(op, target);
    bindIn(op, cells_->zero);
    bindIn(op, cells_->minusOne);
    if (captureSource)
      bindInCapture(op, source);
    else
      bindIn(op, source);
    // §7.2's canonical wing table makes Value four-in/one-out; the
    // write result itself is discardable.
    bindOut(op, temporary());
    spin(op, jump(continuation));
    return op;
  }
  CellRef arithmetic(OpcodeKind kind, CellRef left, CellRef right,
                     CellRef output, CellRef continuation) {
    const auto op = mint(kind);
    bindIn(op, left);
    bindIn(op, right);
    bindOut(op, output);
    spin(op, jump(continuation));
    return op;
  }
  CellRef unaryOp(OpcodeKind kind, CellRef input, CellRef output,
                  CellRef continuation) {
    const auto op = mint(kind);
    bindIn(op, input);
    bindOut(op, output);
    spin(op, jump(continuation));
    return op;
  }
  CellRef ternaryOp(OpcodeKind kind, CellRef in0, CellRef in1, CellRef in2,
                    CellRef output, CellRef continuation) {
    const auto op = mint(kind);
    bindIn(op, in0);
    bindIn(op, in1);
    bindIn(op, in2);
    bindOut(op, output);
    spin(op, jump(continuation));
    return op;
  }
  CellRef gate(CellRef condition, CellRef onTrue, CellRef onFalse) {
    const auto branch = mint(OpcodeKind::Branch);
    bindIn(branch, condition);
    bindIn(branch, reference(onTrue));
    spin(branch, jump(onFalse));
    return branch;
  }
  CellRef cut(CellRef depth, CellRef continuation) {
    const auto op = mint(OpcodeKind::Cut);
    bindIn(op, depth);
    spin(op, jump(continuation));
    return op;
  }
  // #BIND name value — the d.vars/d.values name-keyed channel the run
  // convention binds arguments through. The canonical wing table makes
  // Bind two-in/one-out; the bound cell itself is discardable.
  CellRef bindOp(std::string_view name, CellRef value, CellRef continuation) {
    const auto op = mint(OpcodeKind::Bind);
    bindIn(op, textCell(name));
    bindIn(op, value);
    bindOut(op, temporary());
    spin(op, jump(continuation));
    return op;
  }
  // #RESOLVE name → bound output — the name-keyed read half.
  CellRef resolveOp(std::string_view name, CellRef output,
                    CellRef continuation) {
    const auto op = mint(OpcodeKind::Resolve);
    bindIn(op, textCell(name));
    bindOut(op, output);
    spin(op, jump(continuation));
    return op;
  }
  // #FIND subject name → member's rendered value — the name-keyed
  // child hop `term.field` lowers to.
  CellRef findOp(CellRef subject, std::string_view field, CellRef output,
                 CellRef continuation) {
    const auto op = mint(OpcodeKind::Find);
    bindIn(op, subject);
    bindIn(op, textCell(field));
    bindOut(op, output);
    spin(op, jump(continuation));
    return op;
  }
  // #LINKED subject dim + follow — the runtime hop that materializes a
  // rank of ref-boxes under `output` through the subject's own link.
  CellRef linkedOp(CellRef subject, zigzag::DimRef dim,
                   std::string_view dimName, CellRef output,
                   CellRef continuation) {
    const auto op = mint(OpcodeKind::Linked);
    bindIn(op, subject);
    bindIn(op, dimRef(dim, dimName));
    bindIn(op, cells_->zero);
    bindIn(op, scalar(true));
    bindOut(op, output);
    spin(op, jump(continuation));
    return op;
  }
  CellRef literal(const CellValue &value) {
    return std::visit(
        [&](const auto &item) -> CellRef {
          if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                       std::string>)
            return own(core_.arena().makeCell(item));
          else
            return own(core_.arena().makeScalarCell(item));
        },
        value);
  }
  // `literal` without the recipe literal record — for cells whose
  // content is an int64 reference (slots, ref cells) or recipe-internal
  // data that must not be replayed as a literal value write.
  CellRef literalCell(const CellValue &value) {
    const auto cell = literal(value);
    recLiteral(cell, value);
    return cell;
  }

  CellRef resolved(const Expr &expression) const {
    const auto local = localReferences_.find(&expression);
    return local != localReferences_.end()
               ? localCells_.at(local->second)
               : externalReferences_.at(&expression);
  }

  // A subtree scan for fragment-parameter references — `static`
  // operands inside a parameterized fragment must not read a frame-
  // bound local (indices below paramCount_).
  bool referencesParam(const Expr &expression) const {
    if (expression.kind == ExprKind::Identifier) {
      if (const auto found = localReferences_.find(&expression);
          found != localReferences_.end() && found->second < paramCount_)
        return true;
    }
    for (const auto &child : expression.children)
      if (child && referencesParam(*child)) return true;
    for (const ExprPtr *meta :
         {&expression.scope, &expression.recipients, &expression.deadline,
          &expression.guard, &expression.recovery})
      if (*meta && referencesParam(**meta)) return true;
    return false;
  }

  // The §5.6 table lookups — same loc+ordinal key the checker builds.
  FragmentKey keyOf(const Expr &expression) const {
    return fragmentKeyOf(expression);
  }
  const FragmentRecord *fragmentRecord(const Expr &expression) const {
    if (!fragments_ || !fragments_->fragments) return nullptr;
    const auto found = fragments_->fragments->find(keyOf(expression));
    return found != fragments_->fragments->end() ? &found->second : nullptr;
  }
  const FragmentMeta *fragmentMeta(const Expr &expression) const {
    if (!fragments_ || !fragments_->meta) return nullptr;
    const auto found = fragments_->meta->find(keyOf(expression));
    return found != fragments_->meta->end() ? &found->second : nullptr;
  }
  const FragmentInfo *fragmentInfo(const Expr &expression) const {
    if (!fragments_ || !fragments_->info) return nullptr;
    const auto found = fragments_->info->find(keyOf(expression));
    return found != fragments_->info->end() ? &found->second : nullptr;
  }

  CellRef bounded(const Expr &expression, CellRef output,
                  CellRef continuation) {
    const auto depth  = scalar(std::int64_t{0});
    const auto choice = mint(OpcodeKind::Choice);
    bindIn(choice, reference(cells_->fail));
    spin(choice, emit(expression, output, cut(depth, continuation)));
    const auto readDepth = mint(OpcodeKind::ChoiceDepth);
    bindOut(readDepth, depth);
    spin(readDepth, choice);
    return readDepth;
  }

  CellRef emit(const Expr &expression, CellRef output, CellRef continuation) {
    if (discarded_.contains(&expression))
      return emitValue(expression, temporary(),
                       copy(output, cells_->unit, continuation));
    return emitValue(expression, output, continuation);
  }

  CellRef emitValue(const Expr &expression, CellRef output,
                    CellRef continuation) {
    if (const auto value = literals_.find(&expression);
        value != literals_.end())
      return copy(output, literalCell(value->second), continuation);
    switch (expression.kind) {
    case ExprKind::Unit:
      return copy(output, cells_->unit, continuation);
    case ExprKind::Identifier: {
      // An owner-relative field reads exactly like `term.field`: the
      // $owner local's int names the instance cell whose +d.step rank
      // #FIND walks for the +d.name-matched field holder. A cap$N
      // capture of a field finds it by the field's own name — the
      // alias's text is never the +d.name.
      if (ownerRefs_.contains(&expression)) {
        const auto field = capFieldRefs_.find(&expression);
        return findOp(ownerCell(),
                      field != capFieldRefs_.end() ? field->second
                                                   : expression.text,
                      output, continuation);
      }
      const auto source = resolved(expression);
      // A cap$N operand wraps its binding cell in a capture slot —
      // masterOf(slot) at run time — never an int64 snapshot.
      return copyValue(output, source, continuation,
                       captureCells_.contains(source));
    }
    case ExprKind::Fail:
      return jump(cells_->fail);
    case ExprKind::Unary: {
      const auto input = temporary();
      if (expression.text == "-") {
        const auto op = mint(OpcodeKind::Neg);
        bindIn(op, input);
        bindOut(op, output);
        spin(op, jump(continuation));
        return emit(*expression.children.front(), input, op);
      }
      const auto depth     = scalar(std::int64_t{0});
      const auto readDepth = mint(OpcodeKind::ChoiceDepth);
      bindOut(readDepth, depth);
      const auto choice  = mint(OpcodeKind::Choice);
      const auto success = copy(output, cells_->unit, continuation);
      bindIn(choice, reference(success));
      spin(readDepth, choice);
      spin(choice,
           emit(*expression.children.front(), input, cut(depth, cells_->fail)));
      return readDepth;
    }
    case ExprKind::Binary: {
      const auto &left  = *expression.children[0];
      const auto &right = *expression.children[1];
      const auto &op    = expression.text;
      if (op == "|") {
        const auto choice = mint(OpcodeKind::Choice);
        bindIn(choice, reference(emit(right, output, continuation)));
        spin(choice, emit(left, output, continuation));
        return choice;
      }
      if (op == "?") {
        const auto subjTemp   = temporary();
        const auto cursorTemp = temporary();
        const auto pushOp     = mint(OpcodeKind::ScanPush);
        bindIn(pushOp, subjTemp);
        bindOut(pushOp, cursorTemp);

        const auto depth     = scalar(std::int64_t{0});
        const auto readDepth = mint(OpcodeKind::ChoiceDepth);
        bindOut(readDepth, depth);

        const auto popSuccess = mint(OpcodeKind::ScanPop);
        bindIn(popSuccess, cursorTemp);
        spin(popSuccess, cut(depth, continuation));

        const auto popFail = mint(OpcodeKind::ScanPop);
        bindIn(popFail, cursorTemp);
        spin(popFail, jump(cells_->fail));

        const auto choice = mint(OpcodeKind::Choice);
        bindIn(choice, reference(popFail));

        const auto rightEntry = emit(right, output, popSuccess);
        spin(choice, rightEntry);
        spin(readDepth, choice);
        spin(pushOp, readDepth);

        return emit(left, subjTemp, pushOp);
      }
      const auto leftValue = temporary();
      if (op == "&")
        return emit(left, leftValue, emit(right, output, continuation));
      const auto rightValue = temporary();
      if (op == ":=" || op == "<-") {
        // An owner-relative field write: #SET walks the runtime owner's
        // +d.step rank for the +d.name-matched holder and writes the
        // produced value into it — resolved(left) names no cell here,
        // so this branch must precede every resolved() use below. The
        // op's false yield fails the statement: writing a field the
        // owner does not carry is a real fault, never a silent drop.
        if (left.kind == ExprKind::Identifier && ownerRefs_.contains(&left)) {
          const auto wrote = temporary();
          const auto set   = mint(OpcodeKind::FieldSet);
          bindIn(set, ownerCell());
          bindIn(set, textCell(left.text));
          bindIn(set, rightValue);
          bindOut(set, wrote);
          spin(set, jump(gate(wrote, copy(output, cells_->unit, continuation),
                              cells_->fail)));
          return emit(right, rightValue, set);
        }
        // A cells path materializes its rank under the bound head itself, so
        // assigning one runs it with the binding cell as the head instead of
        // copying a head's content.
        if (types_.at(&right) == Scalar::CellList)
          return emit(right, resolved(left),
                      copy(output, cells_->unit, continuation));
        return emit(right, rightValue,
                    copy(resolved(left), rightValue,
                         copy(output, cells_->unit, continuation)));
      }
      CellRef operationEntry = cells_->fail;
      if (comparison(op)) {
        const auto matches = temporary();
        operationEntry =
            arithmetic(operation(op), leftValue, rightValue, matches,
                       gate(matches, copy(output, rightValue, continuation),
                            cells_->fail));
      } else {
        operationEntry = arithmetic(operation(op), leftValue, rightValue,
                                    output, continuation);
        if (op == "/" || op == "%") {
          const auto isZero    = temporary();
          const auto effective = types_.at(&left) == Scalar::Bottom
                                     ? types_.at(&right)
                                     : types_.at(&left);
          if (effective == Scalar::Int) {
            const auto leftMin          = temporary();
            const auto rightNegativeOne = temporary();
            const auto overflows        = temporary();
            const auto minimum =
                scalar(std::numeric_limits<std::int64_t>::min());
            const auto both = arithmetic(
                OpcodeKind::And, leftMin, rightNegativeOne, overflows,
                gate(overflows, cells_->fail, operationEntry));
            operationEntry = arithmetic(
                OpcodeKind::Eq, leftValue, minimum, leftMin,
                arithmetic(OpcodeKind::Eq, rightValue, cells_->minusOne,
                           rightNegativeOne, both));
          }
          const auto zero =
              effective == Scalar::Float ? scalar(0.0) : cells_->zero;
          operationEntry =
              arithmetic(OpcodeKind::Eq, rightValue, zero, isZero,
                         gate(isZero, cells_->fail, operationEntry));
        }
      }
      return emit(left, leftValue, emit(right, rightValue, operationEntry));
    }
    case ExprKind::Block: {
      if (expression.children.empty())
        return copy(output, cells_->unit, continuation);
      auto next = continuation;
      for (std::size_t i = expression.children.size(); i-- > 0;)
        next = bounded(
            *expression.children[i],
            i + 1 == expression.children.size() ? output : temporary(), next);
      return next;
    }
    case ExprKind::Field: {
      // `term.field`: evaluate the base into a ref-box temp, #FIND its
      // +d.step member by +d.name, and the member's rendered value is
      // the expression's — the op yields false on a missing field so a
      // guard reads absent data as falsy rather than faulting.
      const auto subject = temporary();
      const auto member  = temporary();
      return emit(*expression.children.front(), subject,
                  findOp(subject, expression.text, member,
                         copy(output, member, continuation)));
    }
    case ExprKind::Record:
      // The value is the ref to the term cell preflight marshaled —
      // one image-side cell the whole run shares, like a string
      // literal; writes through it are not a construct yet.
      return copy(output,
                  literalCell(CellValue{
                      static_cast<std::int64_t>(recordCells_.at(&expression))}),
                  continuation);
    case ExprKind::Cells:
      return emitCells(expression, output, continuation);
    case ExprKind::Let: {
      const auto &init = *expression.children.front();
      const auto slot  = resolved(expression);
      const auto tail  = copy(output, cells_->unit, continuation);
      // A `let` over `static` binds the placeholder itself — the drain
      // restates it in place, and the overlay names the init's `Static`
      // node rather than any emitted cell.
      if (init.kind == ExprKind::Static)
        return copy(slot, emitStaticCell(init), tail);
      return emit(init, slot, tail);
    }
    case ExprKind::If: {
      const auto depth     = scalar(std::int64_t{0});
      const auto readDepth = mint(OpcodeKind::ChoiceDepth);
      bindOut(readDepth, depth);
      const auto choice = mint(OpcodeKind::Choice);
      const auto onFalse =
          expression.children.size() == 3
              ? emit(*expression.children[2], output, continuation)
              : copy(output, cells_->unit, continuation);
      bindIn(choice, reference(onFalse));
      const auto onTrue = emit(*expression.children[1], output, continuation);
      spin(choice,
           emit(*expression.children[0], temporary(), cut(depth, onTrue)));
      spin(readDepth, choice);
      return readDepth;
    }
    case ExprKind::Match: {
      const auto subject = temporary();
      // An exhausted chain falls to #Fail: the checker proves exhaustive
      // matches, so this is reached only when every arm's guard refuses.
      auto next = cells_->fail;
      for (std::size_t i = expression.children.size(); i-- > 1;) {
        const auto &arm     = *expression.children[i];
        const auto &pattern = *arm.children[0];
        auto entry          = emit(*arm.children[1], output, continuation);
        if (arm.guard) {
          const auto &guard = *arm.guard;
          const auto left   = temporary();
          const auto right  = temporary();
          const auto holds  = temporary();
          entry = emit(*guard.children[0], left,
                       emit(*guard.children[1], right,
                            arithmetic(operation(guard.text), left, right,
                                       holds, gate(holds, entry, next))));
        }
        if (pattern.kind == ExprKind::LogicVariable)
          entry = copy(resolved(pattern), subject, entry);
        if (pattern.kind == ExprKind::Integer ||
            pattern.kind == ExprKind::Float ||
            pattern.kind == ExprKind::String ||
            pattern.kind == ExprKind::Boolean) {
          const auto matches = temporary();
          entry = arithmetic(OpcodeKind::Eq, subject,
                             literalCell(literals_.at(&pattern)), matches,
                             gate(matches, entry, next));
        }
        next = entry;
      }
      return emit(*expression.children[0], subject, next);
    }
    case ExprKind::Static:
      return copy(output, emitStaticCell(expression), continuation);
    case ExprKind::Code: {
      // §6.1: mint (or reuse) this literal's fragment object — head +
      // descriptor + body graph — and yield the Code value as a
      // head-valued ref-box. The head ref carries a "run"-role
      // name-record (label → head writeback) — a fragment-owned ref
      // naming a labelled head is name-resolved, not program-side.
      const auto head = emitFragmentObject(expression);
      return copy(output, namedRef(head, fragmentLabel(expression), "run"),
                  continuation);
    }
    case ExprKind::Call: {
      const auto &callee = *expression.children.front();
      if (callee.kind == ExprKind::Identifier) {
        if (isBuiltinCall(callee.text))
          return emitBuiltinCall(expression, output, continuation);
        if (callee.text != "run")
          return emitMemberCall(expression, output, continuation);
      }
      return emitRun(expression, output, continuation);
    }
    case ExprKind::Splice:
      refuse(expression, DiagnosticCode::InvalidDeclaration,
             "a residual splice reached lowering; the expand stage "
             "substitutes every splice before compile");
    default:
      // Preflight has already refused every non-scalar constructor.
      return jump(cells_->fail);
    }
  }

  // §5.5's closed-name overlay: the `let`s in scope at a `static`
  // whose initializers are compile-time-manifest — the only free names
  // a pure+closed operand may read. Emission is continuation-ordered,
  // so the manifest can't be an emitted cell: a literal init carries
  // its `literals_` value, a `static` init names its `Static` node for
  // the drain to resolve first, and `let x := y` aliases through the
  // snapshot to whatever `y` initialized with. Every other initializer
  // is a runtime value: the name stays out, and the eval refuses it as
  // unbound — unreachable through the checker, which already judged
  // non-closed names not-closed.
  std::vector<ClosedLocal> closedOverlay(const Expr &staticExpr) const {
    std::vector<ClosedLocal> closed;
    const auto found = staticCaptures_.find(&staticExpr);
    if (found == staticCaptures_.end()) return closed;
    const auto &snapshot = found->second;
    for (const auto &[name, local] : snapshot) {
      const auto type = resultType(local.type);
      if (!type) continue;
      // Alias chains: `let x := y` is closed exactly when `y`'s own
      // initializer is — walk the snapshot until a non-Identifier init.
      const Expr *init = local.init;
      for (std::size_t hops = 0; init && init->kind == ExprKind::Identifier;
           ++hops) {
        if (hops > snapshot.size()) break; // alias cycles can't form
        const auto source = snapshot.find(init->text);
        init = source == snapshot.end() ? nullptr : source->second.init;
      }
      if (!init) continue;
      if (init->kind == ExprKind::Static)
        closed.push_back({name, *type, init});
      else if (const auto lit = literals_.find(init); lit != literals_.end())
        closed.push_back({name, *type, lit->second});
      else if (init->kind == ExprKind::Unit)
        closed.push_back({name, *type, CellValue{std::string{}}});
    }
    return closed;
  }

  // The `static` emit seam: with a deferred-eval sink (the program
  // walker's context) the operand's eval runs after the member walk —
  // every `def` head's `sym +d.values` edge exists by then, so member
  // calls resolve order-independently — and this emits a placeholder
  // cell the drain restates. Without a sink (statement compiles), the
  // eval is positional: it runs against the compile arena as it stands.
  CellRef emitStaticCell(const Expr &expression) {
    const auto expected = types_.at(expression.children.front().get());
    if (fragments_ && fragments_->pendingStatics) {
      const auto placeholder = own(core_.arena().makeCell());
      const auto checked     = resultType(expected);
      if (!checked)
        refuse(expression, DiagnosticCode::TypeMismatch,
               "'static' result type is not literal-expressible");
      fragments_->pendingStatics->push_back(
          PendingStatic{.expr    = &expression,
                        .target  = placeholder,
                        .type    = *checked,
                        .methods = fragments_->methods,
                        .closed  = closedOverlay(expression)});
      // The placeholder's literal record lands now — the body's
      // refs/literals records are sealed into +d.refs at this body's
      // own emit, long before the drain restates the content. An
      // opaque record is the honest shape: the cell's value is not a
      // fixed literal, and reify twins whatever it holds.
      if (body_) body_->literals.push_back({placeholder, std::nullopt});
      return placeholder;
    }
    auto bindings = bindings_;
    for (const auto &entry : closedOverlay(expression)) {
      const auto *value =
          std::get_if<zigzag::vortex::CellValue>(&entry.manifest);
      CellValue cell;
      if (value) {
        cell = *value;
      } else {
        // A `static` init evaluates inline — the eager path has no
        // pending records to resolve it through.
        auto folded = evalStatic(*std::get<const Expr *>(entry.manifest),
                                 scalarType(entry.type), bindings,
                                 fragments_ ? fragments_->methods : nullptr);
        if (!folded)
          refuse(expression, folded.error().code, folded.error().detail);
        cell = *folded;
      }
      // Eval residue — `literal`, not `literalCell`: the binding cell
      // isn't image content the recipe should replay.
      bindings[entry.name] =
          CompilerBinding{literal(cell), entry.type, /*stateSide=*/false};
    }
    auto value = evalStatic(expression, expected, bindings,
                            fragments_ ? fragments_->methods : nullptr);
    if (!value) refuse(expression, value.error().code, value.error().detail);
    return literalCell(*value);
  }

  // Link boxes under `head` as a +d.grab/+d.step rank at emit time. The
  // head cell is minted by the caller and has no prior rank, so direct
  // links are safe.
  void materialize(CellRef head, const std::vector<CellRef> &elements) {
    CellRef tail = head;
    for (const CellRef element : elements) {
      const auto dim = tail == head ? core_.dims().grab : core_.dims().step;
      recLink(tail, dim, false, element);
      zigzag::expectWritten(
          core_.arena().link(tail, dim, zigzag::DimVector::POS, element));
      tail = element;
    }
  }

  CellRef emitCells(const Expr &expression, CellRef output,
                    CellRef continuation) {
    const auto &path = paths_.at(&expression);
    std::optional<CellRef> subject;
    bool follow = false;
    switch (path.anchor.kind) {
    case CellsPath::Anchor::Kind::Home:
      subject = core_.home();
      break;
    case CellsPath::Anchor::Kind::LiteralCell:
      subject = CellRef(path.anchor.cellId);
      break;
    case CellsPath::Anchor::Kind::Variable:
      // The binding is a ref-box; the runtime reads through it.
      subject = externalReferences_.at(&expression);
      follow  = true;
      break;
    }

    // Creation output cells are minted up front so a later step can subject
    // itself to a cell an earlier step only links at run time.
    std::vector<std::vector<CellRef>> created(path.steps.size());
    for (std::size_t i = 0; i < path.steps.size(); ++i)
      for (std::size_t j = 0; j < path.steps[i].literals.size(); ++j)
        created[i].push_back(temporary());

    const auto dimConstant = [this](std::string_view name) -> CellRef {
      const auto dim = core_.findOrMintDimension(name);
      return dimRef(dim, name);
    };

    // A cells path subject that is a capture binding reads through a
    // capture slot like any other operand.
    const auto subjectSlot = [this, &subject](CellRef op) {
      if (captureCells_.contains(*subject))
        bindInCapture(op, *subject);
      else
        bindIn(op, *subject);
    };

    std::vector<CellRef> ops;
    for (std::size_t i = 0; i < path.steps.size(); ++i) {
      const auto &step = path.steps[i];
      if (!step.create) {
        const auto op = mint(OpcodeKind::Linked);
        subjectSlot(op);
        bindIn(op, dimConstant(step.dim));
        bindIn(op, step.neg ? cells_->minusOne : cells_->zero);
        // The canonical wing table makes Linked exactly four-in: the
        // follow operand is always bound even when false.
        bindIn(op, literalCell(follow));
        if (step.hasFilter) {
          bindIn(op, dimConstant(step.filterDim));
          bindIn(op, step.filterNeg ? cells_->minusOne : cells_->zero);
        }
        bindOut(op, output);
        ops.push_back(op);
        continue;
      }
      for (std::size_t j = 0; j < step.literals.size(); ++j) {
        const CellRef fresh = created[i][j];
        const auto create   = mint(OpcodeKind::New);
        subjectSlot(create);
        bindIn(create, dimConstant(step.dim));
        bindIn(create, step.neg ? cells_->minusOne : cells_->zero);
        bindIn(create, literalCell(true));
        bindOut(create, fresh);
        ops.push_back(create);
        // A bare % mints an unnamed cell; writing "" would leave empty-
        // string content instead of no content.
        if (!step.literals[j].empty()) {
          const auto write = mint(OpcodeKind::Value);
          bindIn(write, fresh);
          bindIn(write, cells_->zero);
          bindIn(write, cells_->minusOne);
          bindIn(write, literalCell(step.literals[j]));
          bindOut(write, temporary());
          ops.push_back(write);
        }
        // Several creations on one step are rank siblings off the subject;
        // a single creation hands itself to the next step as subject.
        if (step.literals.size() == 1) subject = fresh;
      }
    }

    if (path.steps.empty() || path.steps.back().create) {
      // No traversal step: the result is known at emit time, so the boxes
      // are linked under the head statically.
      std::vector<CellRef> elements;
      if (path.steps.empty()) {
        if (follow) {
          // cells"$v": copy the ref out of the binding box at run time.
          const auto box  = temporary();
          const auto read = mint(OpcodeKind::Value);
          bindIn(read, box);
          bindIn(read, cells_->zero);
          bindIn(read, cells_->minusOne);
          if (captureCells_.contains(*subject))
            bindInCapture(read, *subject);
          else
            bindIn(read, *subject);
          bindOut(read, temporary());
          ops.push_back(read);
          elements.push_back(box);
        } else {
          elements.push_back(reference(*subject));
        }
      } else {
        for (const CellRef cell : created.back())
          elements.push_back(reference(cell));
      }
      materialize(output, elements);
    }

    CellRef entry = continuation;
    for (std::size_t i = ops.size(); i-- > 0;) {
      spin(ops[i], jump(entry));
      entry = ops[i];
    }
    return ops.empty() ? jump(entry) : entry;
  }

  // The shared statement/fragment wrapper shape: produced=false entry
  // copy → [prologue] → ChoiceDepth read → Choice(fail-arm) → body →
  // Cut → produced=true → tail. `emitTail` supplies the tail op —
  // Halt for a statement, Return for a fragment body. `prologue` maps
  // the readDepth continuation to any op chain that must run first —
  // the fragment parameter resolves; identity for a statement.
  template <typename MakeTail, typename MakePrologue>
  CellRef emitWrapped(const Expr &expression, CellRef output, CellRef produced,
                      MakeTail &&emitTail, MakePrologue &&prologue) {
    const auto falseValue = scalar(false);
    const auto trueValue  = scalar(true);
    const auto depth      = scalar(std::int64_t{0});
    const auto tail       = emitTail();
    const auto onFailure  = copy(produced, falseValue, tail);
    const auto onSuccess  = cut(depth, copy(produced, trueValue, tail));
    const auto choice     = mint(OpcodeKind::Choice);
    bindIn(choice, reference(onFailure));
    spin(choice, emit(expression, output, onSuccess));
    const auto readDepth = mint(OpcodeKind::ChoiceDepth);
    bindOut(readDepth, depth);
    spin(readDepth, choice);
    return copy(produced, falseValue, prologue(readDepth));
  }

  CompiledStatement emitStatement(const Expr &expression, Scalar type) {
    cells_ = EmissionCells{scalar(std::int64_t{0}), scalar(std::int64_t{-1}),
                           temporary(), mint(OpcodeKind::Fail)};
    localCells_.reserve(localCount_);
    for (std::size_t i = 0; i < localCount_; ++i)
      localCells_.push_back(temporary());
    const auto output   = temporary();
    const auto produced = scalar(false);
    const auto entry    = emitWrapped(
        expression, output, produced, [&] { return mint(OpcodeKind::Halt); },
        [](CellRef next) { return next; });
    // A `run`/code"…" node's semantic type (Code<T>, the fragment's T)
    // beats the scalar arm's Cell — the callsite and the descriptor
    // report the same Type the checker computed (§5.4/:696's map).
    if (const auto rich = resultTypes_.find(&expression);
        rich != resultTypes_.end())
      return {entry, output, produced, rich->second};
    return {entry, output, produced, resultType(type)};
  }

  // §6.1's body emission: same wrapper as a statement but with a
  // #Return tail and the name-keyed #RESOLVE prologue that fills the
  // declared parameters' locals off the cursor's +d.vars channel (the
  // run callsite's #BIND ops land them there — the VM's one vars
  // channel, since a pushed frame holds only the return op). The
  // body's own output / produced cells double as the
  // descriptor-published result/produced cells — the descriptor's
  // "result"/"produced" fields point at them, so no epilogue copy is
  // needed.
  FragmentBody emitFragmentBody(const Expr &body, Scalar type) {
    FragmentBody fragment;
    body_  = &fragment;
    cells_ = EmissionCells{scalar(std::int64_t{0}), scalar(std::int64_t{-1}),
                           temporary(), mint(OpcodeKind::Fail)};
    localCells_.reserve(localCount_);
    for (std::size_t i = 0; i < localCount_; ++i)
      localCells_.push_back(temporary());
    const auto output   = temporary();
    const auto produced = scalar(false);
    const auto entry    = emitWrapped(
        body, output, produced, [&] { return mint(OpcodeKind::Return); },
        [&](CellRef next) {
          // RESOLVE p0 → RESOLVE p1 → … → next, declaration order.
          for (std::size_t i = paramCount_; i-- > 0;)
            next = resolveOp(paramNames_[i], localCells_[i], next);
          return next;
        });
    fragment.entry      = entry;
    fragment.result     = output;
    fragment.produced   = produced;
    fragment.resultType = resultType(type);
    body_               = nullptr;
    return fragment;
  }

  // §5.3's frame-local overlay for a nested code"…" literal — the
  // in-scope `let`/parameter names recorded at preflight (scopes_ is
  // popped before emit), joined to their frame cells. A cap$N capture
  // of a local resolves through it in compileFragment's env join; the
  // emitted capture slot then reads the caller's own slot cell at run
  // time (§5.3's shared-cell semantics — the value the caller wrote,
  // never a snapshot).
  CompilerBindings frameBindings(const Expr &literal) const {
    CompilerBindings frame;
    const auto snapshot = frameCaptures_.find(&literal);
    if (snapshot == frameCaptures_.end()) return frame;
    for (const auto &[name, local] : snapshot->second)
      if (const auto type = resultType(local.type))
        frame.emplace(name, CompilerBinding{localCells_.at(local.index), *type,
                                            /*stateSide=*/false});
    return frame;
  }

  // Mint or reuse the fragment object for a surviving code"…" literal
  // — compileFragment (fragment.cpp) owns the head/descriptor/record
  // emission and the per-literal cache on the context.
  CellRef emitFragmentObject(const Expr &expression) {
    const auto frame = frameBindings(expression);
    auto object      = xanadu::vagent::compileFragment(
        expression, *fragments_, *compiler_, &frame, ownerFields_);
    if (!object) refuse(expression, object.error().code, object.error().detail);
    return object->head;
  }

  // §6.2's `run` callsite on the caller's own cursor: evaluate each
  // argument left-to-right into a temp, #BIND each declared parameter
  // name to its argument value, #CALL the fragment's entry op, and —
  // the VM lands Return on the Call's +d.spin target — gate on the
  // descriptor-published produced flag, then copy the published result
  // out. Convention/name cells pair the callsite into +d.refs: the
  // Call operand slot carries a "call"-role name record whose referent
  // is the entry op; a head-valued convention cell carries the
  // "run"-role record whose referent is the head.
  CellRef emitRun(const Expr &expression, CellRef output,
                  CellRef continuation) {
    const auto &operandExpr = *expression.children[1];
    if (operandExpr.kind != ExprKind::Code)
      return emitRunDynamic(expression, operandExpr, output, continuation);
    const auto *record = fragmentRecord(operandExpr);
    const auto frame   = frameBindings(operandExpr);
    auto object        = xanadu::vagent::compileFragment(
        operandExpr, *fragments_, *compiler_, &frame, ownerFields_);
    if (!object) refuse(expression, object.error().code, object.error().detail);

    // The entry/result/produced cells live in the callee's own graph;
    // from this emission's perspective they are external referents.
    const auto producedCell = object->produced;
    const auto resultCell   = object->result;
    const auto entryOp      = object->entry;
    const auto head         = object->head;

    // Continuation the #CALL's frame lands on (the VM lands Return on
    // the Call's +d.spin target): produced? copy the published result
    // to this expression's output : fail.
    const auto afterCall = gate(
        producedCell, copy(output, resultCell, continuation), cells_->fail);

    const auto callOp = mint(OpcodeKind::Call);
    const auto label  = fragmentLabel(operandExpr);
    // The Call operand decodes like Jump/Branch's: a slot naming a
    // ref-box cell whose int64 content is the entry op's position —
    // the VM renders the decoded cell and toInt()s it (vortex_vm.cpp:
    // 854-856), so the op cell itself must never sit in the operand
    // slot. The slot gets the "call"-role name record; the head-valued
    // convention cell rides the Call op's +d.values edge and gets the
    // "run"-role record. Together they are the §6.2 callsite pair.
    bindInNamed(callOp, reference(entryOp), label, "call", entryOp);
    const auto convention = namedRef(head, label, "run");
    link(callOp, core_.dims().values, false, convention);
    spin(callOp, afterCall);

    // #BIND pname argN in declaration order before the call — the
    // callee's #RESOLVE prologue reads them off the cursor's +d.vars
    // (the VM's one vars channel). Arguments evaluate left-to-right.
    const std::size_t arity = record->body.parameters.size();
    std::vector<CellRef> argTemps(arity);
    for (auto &temp : argTemps) temp = temporary();
    CellRef tail = callOp;
    for (std::size_t i = arity; i-- > 0;)
      tail = bindOp(record->body.parameters[i].name, argTemps[i], tail);
    if (arity != 0) {
      const auto &argsExpr = *expression.children[2];
      for (std::size_t i = arity; i-- > 0;)
        tail = emit(*argsExpr.children[i], argTemps[i], tail);
    }
    return tail;
  }

  // §6.2's dynamic arm — the operand is a Code value, a ref-box whose
  // int64 names a head, not a literal the emitter could pre-resolve.
  // RunCall derefs the head's +d.entry at run, descriptor-checks the
  // staged rank's arity, binds each value by the parameter's +d.name
  // label, pushes the frame whose successor is RunResume, and jumps;
  // RunResume is the produced/result gate — §6.2's post-Return half.
  CellRef emitRunDynamic(const Expr &expression, const Expr &operandExpr,
                         CellRef output, CellRef continuation) {
    const auto runResume = mint(OpcodeKind::RunResume);
    const auto box       = temporary();
    bindIn(runResume, box);
    bindIn(runResume, reference(cells_->fail));
    bindOut(runResume, output);
    spin(runResume, jump(continuation));

    const auto runCall = mint(OpcodeKind::RunCall);
    bindIn(runCall, box);
    bindIn(runCall, dimRef(core_.findOrMintDimension("d.entry"), "d.entry"));
    bindIn(runCall, reference(cells_->fail));
    spin(runCall, runResume);

    // Caller-staged arguments: evaluated left-to-right into temps and
    // ranked on +d.grab/+d.step — parameterCells' convention — so the
    // op walks the values beside the descriptor's parameter rank.
    std::vector<CellRef> argTemps;
    if (expression.children.size() == 3)
      for (std::size_t i = 0; i < expression.children[2]->children.size(); ++i)
        argTemps.push_back(temporary());
    const auto argsRank = own(core_.arena().makeCell());
    if (!argTemps.empty()) {
      link(argsRank, core_.dims().grab, false, argTemps.front());
      for (std::size_t i = 1; i < argTemps.size(); ++i)
        link(argTemps[i - 1], core_.dims().step, false, argTemps[i]);
    }
    bindIn(runCall, reference(argsRank));

    CellRef tail = runCall;
    if (expression.children.size() == 3)
      for (std::size_t i = argTemps.size(); i-- > 0;)
        tail = emit(*expression.children[2]->children[i], argTemps[i], tail);
    return emit(operandExpr, box, tail);
  }

  // A member `def` callsite — the emit side of §6.2's RunCall/
  // RunResume machinery with a *compile-time* operand: the module
  // symbol the member pre-pass minted. RunCall resolves
  // `sym +d.values → head` at run, so a caller lowered before its
  // callee still binds; positional args stage left-to-right on the
  // same +d.grab/+d.step rank the dynamic arm uses.
  CellRef emitMemberCall(const Expr &expression, CellRef output,
                         CellRef continuation) {
    const auto &callee = *expression.children.front();
    const auto &target = fragments_->methods->at(callee.text);

    const auto runResume = mint(OpcodeKind::RunResume);
    const auto box       = reference(target.sym);
    bindIn(runResume, box);
    bindIn(runResume, reference(cells_->fail));
    bindOut(runResume, output);
    spin(runResume, jump(continuation));

    const auto runCall = mint(OpcodeKind::RunCall);
    bindIn(runCall, box);
    bindIn(runCall, dimRef(core_.findOrMintDimension("d.entry"), "d.entry"));
    bindIn(runCall, reference(cells_->fail));
    spin(runCall, runResume);

    std::vector<CellRef> argTemps;
    for (std::size_t i = 1; i < expression.children.size(); ++i)
      argTemps.push_back(temporary());
    const auto argsRank = own(core_.arena().makeCell());
    if (!argTemps.empty()) {
      link(argsRank, core_.dims().grab, false, argTemps.front());
      for (std::size_t i = 1; i < argTemps.size(); ++i)
        link(argTemps[i - 1], core_.dims().step, false, argTemps[i]);
    }
    bindIn(runCall, reference(argsRank));

    CellRef tail = runCall;
    for (std::size_t i = argTemps.size(); i-- > 0;)
      tail = emit(*expression.children[i + 1], argTemps[i], tail);
    return tail;
  }

  CellRef emitBuiltinCall(const Expr &expression, CellRef output,
                          CellRef continuation) {
    const auto &callee = *expression.children.front();
    const auto name    = callee.text;

    if (name == "head_of") {
      return emit(*expression.children[1], output, continuation);
    }

    if (name == "program") {
      CellRef rootCell = zigzag::noCell;
      if (const auto found = bindings_.find("program");
          found != bindings_.end()) {
        rootCell = found->second.cell;
      } else if (fragments_ && fragments_->bindings) {
        if (const auto it = fragments_->bindings->find("program");
            it != fragments_->bindings->end()) {
          rootCell = it->second.cell;
        }
      }
      if (rootCell == zigzag::noCell) rootCell = reference(core_.home());
      return copy(output, rootCell, continuation);
    }

    if (name == "agent_cell") {
      CellRef agCell = zigzag::noCell;
      if (const auto found = bindings_.find("agent_cell");
          found != bindings_.end()) {
        agCell = found->second.cell;
      } else if (const auto found = bindings_.find("$agent");
                 found != bindings_.end()) {
        agCell = found->second.cell;
      } else if (fragments_ && fragments_->bindings) {
        if (const auto it = fragments_->bindings->find("agent_cell");
            it != fragments_->bindings->end()) {
          agCell = it->second.cell;
        } else if (const auto it = fragments_->bindings->find("$agent");
                   it != fragments_->bindings->end()) {
          agCell = it->second.cell;
        }
      }
      if (agCell == zigzag::noCell) agCell = reference(core_.home());
      return copy(output, agCell, continuation);
    }

    if (name == "boot_writer" || name == "boot_linker") {
      auto boot = ensureBootstrapPair(core_, vm_, core_.home());
      if (!boot)
        refuse(expression, DiagnosticCode::InvalidDeclaration, boot.error());
      const CellRef head = name == "boot_writer" ? boot->writer : boot->linker;
      return copy(output, reference(head), continuation);
    }

    if (name == "make_cell") {
      const auto valSlot = temporary();
      const auto fresh   = temporary();
      const auto newOp   = mint(OpcodeKind::New);
      bindIn(newOp, reference(core_.home()));
      bindIn(newOp, dimRef(core_.dims().step, "d.step"));
      bindIn(newOp, cells_->zero);
      bindIn(newOp, literalCell(true));
      bindOut(newOp, fresh);

      const auto writeOp = mint(OpcodeKind::Value);
      bindIn(writeOp, fresh);
      bindIn(writeOp, cells_->zero);
      bindIn(writeOp, cells_->minusOne);
      bindIn(writeOp, valSlot);
      bindOut(writeOp, temporary());

      spin(newOp, writeOp);
      spin(writeOp, jump(copy(output, reference(fresh), continuation)));
      return emit(*expression.children[1], valSlot, newOp);
    }

    // 1-in / 1-out builtins
    if (name == "str_len" || name == "int_to_str" || name == "str_to_int" ||
        name == "abs" || name == "trim" || name == "to_lower" ||
        name == "to_upper") {
      OpcodeKind kind = OpcodeKind::StrLen;
      if (name == "str_len")
        kind = OpcodeKind::StrLen;
      else if (name == "int_to_str")
        kind = OpcodeKind::IntToStr;
      else if (name == "str_to_int")
        kind = OpcodeKind::StrToInt;
      else if (name == "abs")
        kind = OpcodeKind::Abs;
      else if (name == "trim")
        kind = OpcodeKind::Trim;
      else if (name == "to_lower")
        kind = OpcodeKind::ToLower;
      else if (name == "to_upper")
        kind = OpcodeKind::ToUpper;

      const auto arg0 = temporary();
      const auto op   = unaryOp(kind, arg0, output, continuation);
      return emit(*expression.children[1], arg0, op);
    }

    // 2-in / 1-out builtins
    if (name == "byte_at" || name == "min" || name == "max") {
      OpcodeKind kind = OpcodeKind::ByteAt;
      if (name == "byte_at")
        kind = OpcodeKind::ByteAt;
      else if (name == "min")
        kind = OpcodeKind::Min;
      else if (name == "max")
        kind = OpcodeKind::Max;

      const auto arg0 = temporary();
      const auto arg1 = temporary();
      const auto op   = arithmetic(kind, arg0, arg1, output, continuation);
      return emit(*expression.children[1], arg0,
                  emit(*expression.children[2], arg1, op));
    }

    // 3-in / 1-out builtins
    if (name == "str_slice" || name == "clamp") {
      OpcodeKind kind =
          (name == "str_slice" ? OpcodeKind::StrSlice : OpcodeKind::Clamp);
      const auto arg0 = temporary();
      const auto arg1 = temporary();
      const auto arg2 = temporary();
      const auto op   = ternaryOp(kind, arg0, arg1, arg2, output, continuation);
      return emit(*expression.children[1], arg0,
                  emit(*expression.children[2], arg1,
                       emit(*expression.children[3], arg2, op)));
    }

    if (name == "tab" || name == "move") {
      const auto arg0 = temporary();
      const auto op =
          mint(name == "tab" ? OpcodeKind::ScanTab : OpcodeKind::ScanMove);
      bindIn(op, arg0);
      bindIn(op, reference(cells_->fail));
      bindOut(op, output);
      spin(op, jump(continuation));
      return emit(*expression.children[1], arg0, op);
    }

    if (name == "match" || name == "many" || name == "upto" || name == "any" ||
        name == "find") {
      OpcodeKind kind = OpcodeKind::ScanMatch;
      if (name == "match")
        kind = OpcodeKind::ScanMatch;
      else if (name == "many")
        kind = OpcodeKind::ScanMany;
      else if (name == "upto")
        kind = OpcodeKind::ScanUpto;
      else if (name == "any")
        kind = OpcodeKind::ScanAny;
      else if (name == "find")
        kind = OpcodeKind::ScanFind;

      const auto arg0 = temporary();
      const auto op   = mint(kind);
      bindIn(op, arg0);
      bindIn(op, reference(cells_->fail));
      bindOut(op, output);
      spin(op, jump(continuation));
      return emit(*expression.children[1], arg0, op);
    }

    if (name == "pos") {
      const auto op = mint(OpcodeKind::ScanPos);
      if (expression.children.size() > 1) {
        const auto arg0 = temporary();
        bindIn(op, arg0);
        bindIn(op, reference(cells_->fail));
        bindOut(op, output);
        spin(op, jump(continuation));
        return emit(*expression.children[1], arg0, op);
      }
      bindIn(op, reference(cells_->fail));
      bindOut(op, output);
      spin(op, jump(continuation));
      return op;
    }

    if (name == "subject") {
      const auto op = mint(OpcodeKind::ScanSubject);
      bindIn(op, reference(cells_->fail));
      bindOut(op, output);
      spin(op, jump(continuation));
      return op;
    }

    return jump(cells_->fail);
  }

  // The callsite label for +d.refs name-records — the fragment's own
  // label (declared #label verbatim, else the deterministic position
  // key) so genesis re-resolves the head by name. Same string
  // fragment.cpp hangs on the head's +d.name.
  std::string fragmentLabel(const Expr &expression) {
    const auto *record = fragmentRecord(expression);
    return fragmentLabelOf(
        keyOf(expression),
        record ? record->body.label : std::optional<std::string>{},
        fragments_ ? fragments_->module : std::optional<std::string>{});
  }

  // bindIn with a name-record on the slot: used for the `run` Call
  // operand so genesis can re-resolve the callsite's entry referent.
  // `operand` is the cell the slot names (the ref-box); `referent` is
  // what the name-record's re-resolution stands for — the callee's
  // entry op.
  CellRef bindInNamed(CellRef op, CellRef operand, std::string name,
                      std::string role, CellRef referent) {
    const auto slot =
        own(core_.arena().makeScalarCell(static_cast<std::int64_t>(operand)));
    wing(op, slot, false);
    if (body_)
      body_->refs.push_back({slot,
                             std::move(name),
                             std::move(role),
                             classify(referent),
                             static_cast<std::int64_t>(referent),
                             {}});
    return slot;
  }

  VortexCore &core_;
  VortexVM &vm_;
  const CompilerBindings &bindings_;
  ScalarCompilerLimits limits_;
  struct Local {
    std::size_t index;
    Scalar type;
    // The richer checker Type when the initializer carried one —
    // `let c := code"…"` binds Cell here but `Code<T>` on the
    // resultTypes_ channel; `run(c)` reads T back off this slot.
    std::optional<Type> rich{};
    // The let's initializer expression — parameters carry none. The
    // closed-name overlay reads it to tell a manifest value (literal,
    // `static`, closed alias) from a runtime binding.
    const Expr *init{};
  };
  std::vector<std::map<std::string, Local, std::less<>>> scopes_;
  std::size_t localCount_{0};
  std::vector<CellRef> localCells_;
  std::unordered_map<const Expr *, std::size_t> localReferences_;
  // §5.3's per-literal in-scope local snapshot — preflight's scopes_
  // is popped by emit, so each code"…" records its visible locals for
  // frameBindings to materialize into the env-join overlay.
  std::unordered_map<const Expr *, std::map<std::string, Local, std::less<>>>
      frameCaptures_;
  // Per-`static` in-scope local snapshot — same walk as frameCaptures_
  // (scopes_ is popped by emit); closedOverlay filters it to the
  // manifest-carrying entries at emit time.
  std::unordered_map<const Expr *, std::map<std::string, Local, std::less<>>>
      staticCaptures_;
  std::unordered_map<const Expr *, CellRef> externalReferences_;
  std::size_t nodeCount_{0};
  std::unordered_set<const Expr *> discarded_;
  std::unordered_map<const Expr *, Scalar> types_;
  std::unordered_map<const Expr *, CellValue> literals_;
  std::unordered_map<const Expr *, CellsPath> paths_;
  std::unordered_map<const Expr *, CellRef> recordCells_;
  struct EmissionCells {
    CellRef zero;
    CellRef minusOne;
    CellRef unit;
    CellRef fail;
  };
  std::optional<EmissionCells> cells_;

  // ---- Fragment-emission state (§6.1) ---------------------------------
  const FragmentContext *fragments_{nullptr};
  Compiler *compiler_{nullptr};
  bool recordOps_{false};
  // Every cell this Emitter minted — the internal/external referent
  // discriminator (map membership) the recipe replay keys on.
  std::unordered_set<CellRef> own_;
  // The live bookkeeping accumulator while a fragment body emits; null
  // on a statement image — statements record no recipe.
  FragmentBody *body_{nullptr};
  // Declared-parameter bookkeeping: parameters are locals 0..n-1,
  // resolved by name in the prologue; `referencesParam` refuses a
  // `static` that reads one.
  std::size_t paramCount_{0};
  std::vector<std::string> paramNames_;
  // §5.8's owner-relative member fields — set on member fragment
  // compiles only. ownerIndex_ is the local slot `$owner` bound into;
  // ownerRefs_ marks the expressions binding() resolved owner-side.
  static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();
  const OwnerFields *ownerFields_{nullptr};
  std::size_t ownerIndex_{npos};
  std::unordered_set<const Expr *> ownerRefs_;
  // §5.3's owner-field captures — compileFragment's env join rewrites
  // a cap$N naming an owner field to the field name here, and the
  // Identifier arm's find hop reads capFieldRefs_ for that name (the
  // alias's own text is never a field).
  const std::map<std::string, std::string, std::less<>> *capFieldNames_{
      nullptr};
  std::unordered_map<const Expr *, std::string> capFieldRefs_;
  CellRef ownerCell() const { return localCells_.at(ownerIndex_); }
  // Binding cells referenced through cap$N aliases — operands on them
  // lower to capture slots (clone-rank members + ref-capture records),
  // never plain ref reads.
  std::unordered_set<CellRef> captureCells_;
  // int64(dim) cells minted by dimRef — the dim-name channel for the
  // ref-capture record's +d.name edge.
  std::unordered_map<CellRef, std::string> dimNames_;
  // Semantic (rich) result types for `run` and code"…" nodes — the
  // scalar arm keeps Cell while the descriptor and callsite types
  // carry Code<T>/T.
  std::unordered_map<const Expr *, Type> resultTypes_;
};

} // namespace

std::expected<CompiledStatement, Diagnostic>
Compiler::compileStatement(const Expr &statement,
                           const CompilerBindings &bindings, ResultUse use,
                           const FragmentContext *fragments) {
  noteEmissionStart();
  return Emitter(core_, vm_, bindings, limits_, fragments, this,
                 /*recordOps=*/true)
      .compile(statement, use);
}

std::expected<FragmentBody, Diagnostic>
Compiler::compileFragmentBody(const FragmentBodyRequest &request) {
  if (!request.body)
    return std::unexpected(
        Diagnostic{DiagnosticCode::InvalidDeclaration,
                   "compileFragmentBody called without a body",
                   {}});
  noteEmissionStart();
  static const CompilerBindings kEmptyBindings;
  Emitter emitter(core_, vm_,
                  request.bindings ? *request.bindings : kEmptyBindings,
                  limits_, request.fragments, this,
                  /*recordOps=*/true);
  return emitter.compileFragment(*request.body, request.parameters, request.use,
                                 request.ownerFields, request.capFieldNames);
}

std::expected<void, std::vector<Diagnostic>>
Compiler::drainStatics(FragmentContext &context) {
  if (!context.pendingStatics) return {};
  // The sink detaches before evals run: a `static` inside an operand
  // evaluates eagerly at its own emit — the enclosing run needs the
  // folded cell before it starts — so nothing appends mid-drain.
  auto pending           = std::move(*context.pendingStatics);
  context.pendingStatics = nullptr;
  std::vector<Diagnostic> errors;
  // A `let x := static(…)` chains records: the dependent operand binds
  // `x` through the init's `Static` node, whose own record must fold
  // first. Records are keyed on their expression so resolution orders
  // by dependency, not by the continuation-ordered emission sequence.
  std::unordered_map<const Expr *, std::size_t> indexOf;
  for (std::size_t i = 0; i < pending.size(); ++i)
    indexOf.emplace(pending[i].expr, i);
  std::vector<std::optional<CellValue>> folded(pending.size());
  std::vector<bool> draining(pending.size(), false);
  const auto manifest = [&](const CellValue &value) {
    return std::visit(
        [&](const auto &item) -> CellRef {
          if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                       std::string>)
            return core_.arena().makeCell(item);
          else
            return core_.arena().makeScalarCell(item);
        },
        value);
  };
  const auto drain = [&](this auto &&self, std::size_t i) -> void {
    if (folded[i] || draining[i]) return;
    draining[i]        = true; // before resolving — guards init cycles
    const auto &record = pending[i];
    CompilerBindings bindings =
        context.bindings ? *context.bindings : CompilerBindings{};
    for (const auto &entry : record.closed) {
      std::optional<CellValue> cell;
      if (const auto *value =
              std::get_if<zigzag::vortex::CellValue>(&entry.manifest)) {
        cell = *value;
      } else {
        const auto *init = std::get<const Expr *>(entry.manifest);
        if (const auto dep = indexOf.find(init); dep != indexOf.end()) {
          self(dep->second);
          cell = folded[dep->second];
        }
        // The init has no pending record when its eval refused — the
        // binding then just omits the name and this operand's own
        // failure lands on its entry.name reference.
      }
      if (cell)
        bindings[entry.name] = CompilerBinding{manifest(*cell), entry.type,
                                               /*stateSide=*/false};
    }
    Emitter host(core_, vm_, bindings, limits_, &context, this, false);
    // `type` is the checked literal scalar; a non-scalar (an
    // unannotated `var`'s `<any>` seed) carries no contract, so the
    // eval accepts whichever literal-expressible arm it produces.
    auto value = host.evalStatic(*record.expr, scalarType(record.type),
                                 bindings, record.methods);
    if (!value) {
      errors.push_back(value.error());
      return;
    }
    // The placeholder's content restates in place — the op's operand
    // slot and the var record's +d.values edge already name the cell.
    if (!core_.writeValue(record.target, 0, std::nullopt, *value)) {
      errors.push_back(Diagnostic{DiagnosticCode::InvalidDeclaration,
                                  "'static' placeholder cell could not "
                                  "be written",
                                  record.expr->loc});
      return;
    }
    folded[i] = *value;
  };
  for (std::size_t i = 0; i < pending.size(); ++i) drain(i);
  if (!errors.empty()) return std::unexpected(std::move(errors));
  return {};
}

} // namespace xanadu::vagent
