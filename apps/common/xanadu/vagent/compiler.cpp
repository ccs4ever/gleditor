#include "common/xanadu/vagent/compiler.hpp"

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

enum class Scalar : std::uint8_t { Int, Float, Str, Bool, Unit, Bottom };

std::optional<Scalar> scalarType(const Type &type) {
  if (!type.arguments.empty()) return std::nullopt;
  if (type.name == "Int") return Scalar::Int;
  if (type.name == "Float") return Scalar::Float;
  if (type.name == "Str") return Scalar::Str;
  if (type.name == "Bool") return Scalar::Bool;
  if (type.name == "()") return Scalar::Unit;
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
  case Scalar::Bottom:
    return std::nullopt;
  }
  return std::nullopt;
}

bool comparison(std::string_view op) {
  return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
         op == ">=";
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
          ScalarCompilerLimits limits)
      : core_(core), vm_(vm), bindings_(bindings), limits_(limits) {}

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

private:
  [[noreturn]] static void refuse(const Expr &expression, DiagnosticCode code,
                                  std::string detail) {
    throw Diagnostic{code, std::move(detail), expression.loc};
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
        return local->second.type;
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
    const auto value = core_.render(found->second.cell).value();
    const bool matches =
        (*type == Scalar::Int && std::holds_alternative<std::int64_t>(value)) ||
        (*type == Scalar::Float && std::holds_alternative<double>(value)) ||
        (*type == Scalar::Bool && std::holds_alternative<bool>(value)) ||
        (*type == Scalar::Str && std::holds_alternative<std::string>(value)) ||
        (*type == Scalar::Unit && std::holds_alternative<std::string>(value) &&
         std::get<std::string>(value).empty());
    if (!matches)
      refuse(expression, DiagnosticCode::TypeMismatch,
             "binding value does not have its declared scalar type");
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
        !expression.names.empty())
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
        type = rightType == Scalar::Bottom ? Scalar::Bottom : Scalar::Unit;
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
      scopes_.back().emplace(expression.text, Local{index, value});
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

  CellRef mint(OpcodeKind kind) {
    return vm_.mintOpcode(kind, "vagent.scalar");
  }
  CellRef temporary() { return core_.arena().makeCell(); }
  CellRef reference(CellRef target) {
    return core_.arena().makeScalarCell(static_cast<std::int64_t>(target));
  }
  void spin(CellRef from, CellRef to) {
    zigzag::expectWritten(core_.arena().link(from, core_.dims().spin,
                                             zigzag::DimVector::POS, to));
  }
  CellRef jump(CellRef target) {
    const auto op = mint(OpcodeKind::Jump);
    core_.bindInput(op, reference(target));
    return op;
  }
  CellRef copy(CellRef target, CellRef source, CellRef continuation) {
    const auto op = mint(OpcodeKind::Value);
    core_.bindInput(op, target);
    core_.bindInput(op, cells_->zero);
    core_.bindInput(op, cells_->minusOne);
    core_.bindInput(op, source);
    spin(op, jump(continuation));
    return op;
  }
  CellRef arithmetic(OpcodeKind kind, CellRef left, CellRef right,
                     CellRef output, CellRef continuation) {
    const auto op = mint(kind);
    core_.bindInput(op, left);
    core_.bindInput(op, right);
    core_.bindOutput(op, output);
    spin(op, jump(continuation));
    return op;
  }
  CellRef gate(CellRef condition, CellRef onTrue, CellRef onFalse) {
    const auto branch = mint(OpcodeKind::Branch);
    core_.bindInput(branch, condition);
    core_.bindInput(branch, reference(onTrue));
    spin(branch, jump(onFalse));
    return branch;
  }
  CellRef cut(CellRef depth, CellRef continuation) {
    const auto op = mint(OpcodeKind::Cut);
    core_.bindInput(op, depth);
    spin(op, jump(continuation));
    return op;
  }
  CellRef literal(const CellValue &value) {
    return std::visit(
        [&](const auto &item) -> CellRef {
          if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                       std::string>)
            return core_.arena().makeCell(item);
          else
            return core_.arena().makeScalarCell(item);
        },
        value);
  }

  CellRef resolved(const Expr &expression) const {
    const auto local = localReferences_.find(&expression);
    return local != localReferences_.end()
               ? localCells_.at(local->second)
               : externalReferences_.at(&expression);
  }

  CellRef bounded(const Expr &expression, CellRef output,
                  CellRef continuation) {
    const auto depth  = core_.arena().makeScalarCell(std::int64_t{0});
    const auto choice = mint(OpcodeKind::Choice);
    core_.bindInput(choice, reference(cells_->fail));
    spin(choice, emit(expression, output, cut(depth, continuation)));
    const auto readDepth = mint(OpcodeKind::ChoiceDepth);
    core_.bindOutput(readDepth, depth);
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
      return copy(output, literal(value->second), continuation);
    switch (expression.kind) {
    case ExprKind::Unit:
      return copy(output, cells_->unit, continuation);
    case ExprKind::Identifier:
      return copy(output, resolved(expression), continuation);
    case ExprKind::Fail:
      return jump(cells_->fail);
    case ExprKind::Unary: {
      const auto input = temporary();
      if (expression.text == "-") {
        const auto op = mint(OpcodeKind::Neg);
        core_.bindInput(op, input);
        core_.bindOutput(op, output);
        spin(op, jump(continuation));
        return emit(*expression.children.front(), input, op);
      }
      const auto depth     = core_.arena().makeScalarCell(std::int64_t{0});
      const auto readDepth = mint(OpcodeKind::ChoiceDepth);
      core_.bindOutput(readDepth, depth);
      const auto choice  = mint(OpcodeKind::Choice);
      const auto success = copy(output, cells_->unit, continuation);
      core_.bindInput(choice, reference(success));
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
        core_.bindInput(choice, reference(emit(right, output, continuation)));
        spin(choice, emit(left, output, continuation));
        return choice;
      }
      const auto leftValue = temporary();
      if (op == "&")
        return emit(left, leftValue, emit(right, output, continuation));
      const auto rightValue = temporary();
      if (op == ":=" || op == "<-") {
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
            const auto minimum          = core_.arena().makeScalarCell(
                std::numeric_limits<std::int64_t>::min());
            const auto both = arithmetic(
                OpcodeKind::And, leftMin, rightNegativeOne, overflows,
                gate(overflows, cells_->fail, operationEntry));
            operationEntry = arithmetic(
                OpcodeKind::Eq, leftValue, minimum, leftMin,
                arithmetic(OpcodeKind::Eq, rightValue, cells_->minusOne,
                           rightNegativeOne, both));
          }
          const auto zero = effective == Scalar::Float
                                ? core_.arena().makeScalarCell(0.0)
                                : cells_->zero;
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
    case ExprKind::Let:
      return emit(*expression.children.front(), resolved(expression),
                  copy(output, cells_->unit, continuation));
    case ExprKind::If: {
      const auto depth     = core_.arena().makeScalarCell(std::int64_t{0});
      const auto readDepth = mint(OpcodeKind::ChoiceDepth);
      core_.bindOutput(readDepth, depth);
      const auto choice = mint(OpcodeKind::Choice);
      const auto onFalse =
          expression.children.size() == 3
              ? emit(*expression.children[2], output, continuation)
              : copy(output, cells_->unit, continuation);
      core_.bindInput(choice, reference(onFalse));
      const auto onTrue = emit(*expression.children[1], output, continuation);
      spin(choice,
           emit(*expression.children[0], temporary(), cut(depth, onTrue)));
      spin(readDepth, choice);
      return readDepth;
    }
    default:
      // Preflight has already refused every non-scalar constructor.
      return jump(cells_->fail);
    }
  }

  CompiledStatement emitStatement(const Expr &expression, Scalar type) {
    cells_ = EmissionCells{core_.arena().makeScalarCell(std::int64_t{0}),
                           core_.arena().makeScalarCell(std::int64_t{-1}),
                           temporary(), mint(OpcodeKind::Fail)};
    localCells_.reserve(localCount_);
    for (std::size_t i = 0; i < localCount_; ++i)
      localCells_.push_back(temporary());
    const auto falseValue = core_.arena().makeScalarCell(false);
    const auto trueValue  = core_.arena().makeScalarCell(true);
    const auto output     = temporary();
    const auto produced   = core_.arena().makeScalarCell(false);
    const auto depth      = core_.arena().makeScalarCell(std::int64_t{0});
    const auto halt       = mint(OpcodeKind::Halt);
    const auto onFailure  = copy(produced, falseValue, halt);
    const auto onSuccess  = cut(depth, copy(produced, trueValue, halt));
    const auto choice     = mint(OpcodeKind::Choice);
    core_.bindInput(choice, reference(onFailure));
    spin(choice, emit(expression, output, onSuccess));
    const auto readDepth = mint(OpcodeKind::ChoiceDepth);
    core_.bindOutput(readDepth, depth);
    spin(readDepth, choice);
    const auto entry = copy(produced, falseValue, readDepth);
    return {entry, output, produced, resultType(type)};
  }

  VortexCore &core_;
  VortexVM &vm_;
  const CompilerBindings &bindings_;
  ScalarCompilerLimits limits_;
  struct Local {
    std::size_t index;
    Scalar type;
  };
  std::vector<std::map<std::string, Local, std::less<>>> scopes_;
  std::size_t localCount_{0};
  std::vector<CellRef> localCells_;
  std::unordered_map<const Expr *, std::size_t> localReferences_;
  std::unordered_map<const Expr *, CellRef> externalReferences_;
  std::size_t nodeCount_{0};
  std::unordered_set<const Expr *> discarded_;
  std::unordered_map<const Expr *, Scalar> types_;
  std::unordered_map<const Expr *, CellValue> literals_;
  struct EmissionCells {
    CellRef zero;
    CellRef minusOne;
    CellRef unit;
    CellRef fail;
  };
  std::optional<EmissionCells> cells_;
};

} // namespace

std::expected<CompiledStatement, Diagnostic>
Compiler::compileStatement(const Expr &statement,
                           const CompilerBindings &bindings, ResultUse use) {
  return Emitter(core_, vm_, bindings, limits_).compile(statement, use);
}

} // namespace xanadu::vagent
