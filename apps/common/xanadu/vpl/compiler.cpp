/**
 * @file compiler.cpp
 * @brief Implementation of VPL Compiler translating AST to Vortex bytecode.
 */
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/vortex/assembly_transaction.hpp"
#include "common/xanadu/vortex/program_export.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <unordered_set>

#include "common/xanadu/vpl/lexer.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"
#include "common/xanadu/vpl/parser.hpp"

namespace xanadu::vpl {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

namespace {

std::string_view verbName(TokenKind kind) noexcept {
  switch (kind) {
  case TokenKind::Catenate:
    return ",";
  case TokenKind::Iota:
    return "⍳ (i.)";
  case TokenKind::Rho:
    return "⍴ ($)";
  case TokenKind::Transpose:
    return "⍉ (|:)";
  case TokenKind::ReverseFirst:
    return "⌽ (|.)";
  case TokenKind::ReverseLast:
    return "⊖ (|..)";
  case TokenKind::Take:
    return "↑ ({.)";
  case TokenKind::Drop:
    return "↓ (}.)";
  case TokenKind::Enclose:
    return "⊂ (<)";
  case TokenKind::Disclose:
    return "⊃ (>)";
  case TokenKind::Member:
    return "∊ (e.)";
  case TokenKind::Where:
    return "⍸ (I.)";
  case TokenKind::Match:
    return "≡ (-:)";
  case TokenKind::Tally:
    return "≢ (#)";
  case TokenKind::GradeUp:
    return "⍋ (/:)";
  case TokenKind::GradeDown:
    return "⍒ (\\:)";
  case TokenKind::Plus:
    return "+";
  case TokenKind::Minus:
    return "-";
  case TokenKind::Times:
    return "× (*)";
  case TokenKind::Divide:
    return "÷ (%)";
  case TokenKind::Power:
    return "* (^)";
  case TokenKind::Magnitude:
    return "|";
  case TokenKind::Ceiling:
    return "⌈ (>.)";
  case TokenKind::Floor:
    return "⌊ (<.)";
  case TokenKind::Equal:
    return "=";
  case TokenKind::NotEqual:
    return "≠ (~=)";
  case TokenKind::LessThan:
    return "<";
  case TokenKind::LessEqual:
    return "≤ (<=)";
  case TokenKind::GreaterThan:
    return ">";
  case TokenKind::GreaterEqual:
    return "≥ (>=)";
  case TokenKind::Not:
    return "∼ (~)";
  case TokenKind::And:
    return "∧ (&)";
  case TokenKind::Or:
    return "∨ (|)";
  case TokenKind::Reduce:
    return "/";
  case TokenKind::Scan:
    return "\\";
  case TokenKind::Each:
    return "¨ (each.)";
  case TokenKind::Compress:
    return "⌿ (copy.)";
  case TokenKind::OuterProduct:
    return "∘. (o.)";
  case TokenKind::RankOp:
    return "⍤ (\")";
  case TokenKind::Key:
    return "⌸ (key.)";
  case TokenKind::Hypertime:
    return "⍟ (time.)";
  case TokenKind::Scrub:
    return "⍫ (scrub.)";
  case TokenKind::Transclude:
    return "⌺ (trans.)";
  case TokenKind::Assign:
    return "← (=.)";
  case TokenKind::QuadRead:
    return "⎕READ";
  case TokenKind::QuadSplit:
    return "⎕SPLIT";
  case TokenKind::Quad:
    return "⎕";
  default:
    return "unknown";
  }
}

void formatASTNode(std::ostringstream &oss, const AstNode &node,
                   const std::string &indent, bool isLast) {
  oss << indent << (isLast ? "└── " : "├── ");
  std::string nextIndent = indent + (isLast ? "    " : "│   ");

  if (const auto *prog = dynamic_cast<const Program *>(&node)) {
    oss << "Program (" << prog->expressions().size() << " statements)\n";
    const auto &exprs = prog->expressions();
    for (std::size_t i = 0; i < exprs.size(); ++i) {
      if (exprs[i]) {
        formatASTNode(oss, *exprs[i], nextIndent, i + 1 == exprs.size());
      }
    }
  } else if (const auto *sc = dynamic_cast<const ScalarExpr *>(&node)) {
    oss << "ScalarExpr: ";
    if (sc->isString()) {
      oss << "\"" << sc->stringValue() << "\"\n";
    } else if (sc->isFloat()) {
      oss << sc->floatValue() << "\n";
    } else {
      oss << sc->intValue() << "\n";
    }
  } else if (const auto *vec = dynamic_cast<const VectorExpr *>(&node)) {
    oss << "VectorExpr (" << vec->elements().size() << " elements)\n";
    const auto &elems = vec->elements();
    for (std::size_t i = 0; i < elems.size(); ++i) {
      if (elems[i]) {
        formatASTNode(oss, *elems[i], nextIndent, i + 1 == elems.size());
      }
    }
  } else if (const auto *dim = dynamic_cast<const DimensionExpr *>(&node)) {
    oss << "DimensionExpr: " << dim->name() << "\n";
  } else if (const auto *id = dynamic_cast<const IdentifierExpr *>(&node)) {
    oss << "IdentifierExpr: " << id->name() << "\n";
  } else if (const auto *vb = dynamic_cast<const VerbExpr *>(&node)) {
    oss << "VerbExpr: " << verbName(vb->verb()) << "\n";
  } else if (const auto *mon = dynamic_cast<const MonadicExpr *>(&node)) {
    if (mon->hasCustomVerb()) {
      oss << "MonadicExpr (custom):\n";
      formatASTNode(oss, *mon->customVerb(), nextIndent, !mon->right());
    } else {
      oss << "MonadicExpr: " << verbName(mon->verb()) << "\n";
    }
    if (mon->right()) {
      formatASTNode(oss, *mon->right(), nextIndent, true);
    }
  } else if (const auto *dya = dynamic_cast<const DyadicExpr *>(&node)) {
    if (dya->hasCustomVerb()) {
      oss << "DyadicExpr (custom):\n";
      formatASTNode(oss, *dya->customVerb(), nextIndent, false);
    } else {
      oss << "DyadicExpr: " << verbName(dya->verb()) << "\n";
    }
    if (dya->left()) {
      formatASTNode(oss, *dya->left(), nextIndent, false);
    }
    if (dya->right()) {
      formatASTNode(oss, *dya->right(), nextIndent, true);
    }
  } else if (const auto *adv = dynamic_cast<const AdverbExpr *>(&node)) {
    oss << "AdverbExpr: " << verbName(adv->adverb()) << "\n";
    if (adv->operand()) {
      formatASTNode(oss, *adv->operand(), nextIndent, true);
    }
  } else if (const auto *conj = dynamic_cast<const ConjunctionExpr *>(&node)) {
    oss << "ConjunctionExpr: " << verbName(conj->conjunction()) << "\n";
    if (conj->left()) {
      formatASTNode(oss, *conj->left(), nextIndent, false);
    }
    if (conj->right()) {
      formatASTNode(oss, *conj->right(), nextIndent, true);
    }
  } else if (const auto *asg = dynamic_cast<const AssignExpr *>(&node)) {
    oss << "AssignExpr: " << asg->target() << "\n";
    if (asg->value()) {
      formatASTNode(oss, *asg->value(), nextIndent, true);
    }
  } else if (const auto *idx = dynamic_cast<const IndexingExpr *>(&node)) {
    oss << "IndexingExpr (" << idx->indices().size() << " indices)\n";
    if (idx->target()) {
      formatASTNode(oss, *idx->target(), nextIndent, idx->indices().empty());
    }
    const auto &inds = idx->indices();
    for (std::size_t i = 0; i < inds.size(); ++i) {
      if (inds[i]) {
        formatASTNode(oss, *inds[i], nextIndent, i + 1 == inds.size());
      }
    }
  } else if (const auto *qd = dynamic_cast<const QuadExpr *>(&node)) {
    oss << "QuadExpr: " << verbName(qd->quadKind()) << "\n";
    if (qd->arg()) {
      formatASTNode(oss, *qd->arg(), nextIndent, true);
    }
  } else {
    oss << "AstNode (unknown)\n";
  }
}

} // namespace

std::string_view
VPLCompiler::opcodeMnemonic(zigzag::vortex::OpcodeKind kind) noexcept {
  switch (kind) {
  case OpcodeKind::Add:
    return "ADD";
  case OpcodeKind::Sub:
    return "SUB";
  case OpcodeKind::Mul:
    return "MUL";
  case OpcodeKind::Div:
    return "DIV";
  case OpcodeKind::DivMod:
    return "DIVMOD";
  case OpcodeKind::Mod:
    return "MOD";
  case OpcodeKind::Neg:
    return "NEG";
  case OpcodeKind::Eq:
    return "EQ";
  case OpcodeKind::Neq:
    return "NEQ";
  case OpcodeKind::Lt:
    return "LT";
  case OpcodeKind::Lte:
    return "LTE";
  case OpcodeKind::Gt:
    return "GT";
  case OpcodeKind::Gte:
    return "GTE";
  case OpcodeKind::And:
    return "AND";
  case OpcodeKind::Or:
    return "OR";
  case OpcodeKind::Not:
    return "NOT";
  case OpcodeKind::Link:
    return "LINK";
  case OpcodeKind::Break:
    return "BREAK";
  case OpcodeKind::New:
    return "NEW";
  case OpcodeKind::Value:
    return "VALUE";
  case OpcodeKind::Splice:
    return "SPLICE";
  case OpcodeKind::Clone:
    return "CLONE";
  case OpcodeKind::Jump:
    return "JUMP";
  case OpcodeKind::Branch:
    return "BRANCH";
  case OpcodeKind::Call:
    return "CALL";
  case OpcodeKind::Return:
    return "RETURN";
  case OpcodeKind::Halt:
    return "HALT";
  case OpcodeKind::Nop:
    return "NOP";
  case OpcodeKind::Bind:
    return "BIND";
  case OpcodeKind::Resolve:
    return "RESOLVE";
  case OpcodeKind::Assert:
    return "ASSERT";
  case OpcodeKind::Abs:
    return "ABS";
  case OpcodeKind::Min:
    return "MIN";
  case OpcodeKind::Max:
    return "MAX";
  case OpcodeKind::Clamp:
    return "CLAMP";
  case OpcodeKind::Trim:
    return "TRIM";
  case OpcodeKind::ToLower:
    return "TOLOWER";
  case OpcodeKind::ToUpper:
    return "TOUPPER";
  case OpcodeKind::Unify:
    return "UNIFY";
  case OpcodeKind::IsVar:
    return "ISVAR";
  case OpcodeKind::MakeVar:
    return "MAKEVAR";
  case OpcodeKind::MakeTerm:
    return "MAKETERM";
  case OpcodeKind::Deref:
    return "DEREF";
  case OpcodeKind::Choice:
    return "CHOICE";
  case OpcodeKind::ScopedChoice:
    return "SCOPEDCHOICE";
  case OpcodeKind::VqlFieldProbe:
    return "VQLFIELDPROBE";
  case OpcodeKind::Fail:
    return "FAIL";
  case OpcodeKind::Cut:
    return "CUT";
  case OpcodeKind::ChoiceDepth:
    return "CHOICEDEPTH";
  case OpcodeKind::Linked:
    return "LINKED";
  case OpcodeKind::Find:
    return "FIND";
  case OpcodeKind::FieldSet:
    return "SET";
  case OpcodeKind::RunCall:
    return "RUNCALL";
  case OpcodeKind::RunResume:
    return "RUNRESUME";
  default:
    return "UNKNOWN";
  }
}

VPLCompiler::VPLCompiler(zigzag::vortex::VortexCore &core,
                         zigzag::vortex::VortexVM &vm)
    : core_(core), vm_(vm) {}

std::vector<CellRef> VPLCompiler::dependenciesOf(const CellRef cell) const {
  if (const auto found = cellDependencies_.find(cell);
      found != cellDependencies_.end()) {
    return found->second;
  }
  if (core_.arena().resolveForeign(cell)) return {cell};
  return {};
}

std::vector<CellRef> VPLCompiler::rankDependenciesOf(const CellRef cell) const {
  if (const auto found = rankDependencies_.find(cell);
      found != rankDependencies_.end()) {
    return found->second;
  }
  return dependenciesOf(cell);
}

CellRef VPLCompiler::mintOpcode(OpcodeKind kind, std::string_view label) {
  return vm_.mintOpcode(kind, label);
}

CellRef VPLCompiler::emitOp(OpcodeKind kind, std::string_view label) {
  CellRef op = mintOpcode(kind, label);
  appendOpcode(op);
  return op;
}

void VPLCompiler::appendOpcode(CellRef op) {
  if (!entryOp_) {
    entryOp_ = op;
  }
  if (currentOp_) {
    zigzag::expectWritten(
        core_.arena().link(*currentOp_, core_.dims().spin, false, op));
  }
  currentOp_ = op;
  allOps_.push_back(op);
}

void VPLCompiler::emitInput(CellRef op, CellRef operand) {
  core_.bindInput(op, operand);
  mergeDependencies(cellDependencies_[op], dependenciesOf(operand));
}

void VPLCompiler::emitOutput(CellRef op, CellRef target) {
  core_.bindOutput(op, target);
  cellDependencies_[target] = dependenciesOf(op);
}

CellRef VPLCompiler::emitConstant(const CellValue &val,
                                  std::optional<CellRef> designatedCell) {
  CellRef c = designatedCell ? *designatedCell : core_.arena().makeCell();
  core_.writeValue(c, 0, std::nullopt, val);
  return c;
}

CellRef VPLCompiler::emitInputConstant(CellRef op, const CellValue &val) {
  CellRef c = emitConstant(val);
  emitInput(op, c);
  return c;
}

DimRef VPLCompiler::resolveDimension(std::string_view name) {
  const auto &dims = core_.dims();
  if (name == "d.stores") return dims.stores;
  if (name == "d.name") return dims.name;
  if (name == "d.role") return dims.role;
  if (name == "d.clone") return dims.clone;
  if (name == "d.dims") return dims.dims;
  if (name == "d.grab") return dims.grab;
  if (name == "d.step") return dims.step;
  if (name == "d.spin") return dims.spin;
  if (name == "d.stack") return dims.stack;
  if (name == "d.contract") return dims.contract;
  if (name == "d.cursors") return dims.cursors;
  if (name == "d.cache") return dims.cache;
  if (name == "d.vars") return dims.vars;
  if (name == "d.values") return dims.values;
  if (name == "d.pinning-cursors") return dims.pinningCursors;
  if (name == "d.stdlib") return dims.stdlib;
  if (name == "d.clause") return dims.clause;

  return core_.findOrMintDimension(name);
}

std::optional<CellValue>
VPLCompiler::tryFoldConstant(const AstNode &node) const {
  if (const auto *sc = dynamic_cast<const ScalarExpr *>(&node)) {
    if (sc->isString()) return sc->stringValue();
    if (sc->isFloat()) return sc->floatValue();
    return sc->intValue();
  }

  if (const auto *mon = dynamic_cast<const MonadicExpr *>(&node)) {
    if (!mon->right()) return std::nullopt;
    auto foldedR = tryFoldConstant(*mon->right());
    if (!foldedR) return std::nullopt;

    if (mon->verb() == TokenKind::Plus) {
      return foldedR;
    }
    if (mon->verb() == TokenKind::Minus) {
      if (std::holds_alternative<double>(*foldedR)) {
        return -std::get<double>(*foldedR);
      }
      if (std::holds_alternative<std::int64_t>(*foldedR)) {
        return -std::get<std::int64_t>(*foldedR);
      }
    }
    if (mon->verb() == TokenKind::Magnitude) {
      if (std::holds_alternative<double>(*foldedR)) {
        return std::abs(std::get<double>(*foldedR));
      }
      if (std::holds_alternative<std::int64_t>(*foldedR)) {
        const auto value = std::get<std::int64_t>(*foldedR);
        if (value != std::numeric_limits<std::int64_t>::min())
          return std::abs(value);
      }
    }
    if (mon->verb() == TokenKind::Not) {
      return !zigzag::vortex::VortexCore::evaluateTruthiness(*foldedR);
    }
  }

  if (const auto *dya = dynamic_cast<const DyadicExpr *>(&node)) {
    if (!dya->left() || !dya->right()) return std::nullopt;
    auto lVal = tryFoldConstant(*dya->left());
    auto rVal = tryFoldConstant(*dya->right());
    if (!lVal || !rVal) return std::nullopt;
    if (std::holds_alternative<std::string>(*lVal) ||
        std::holds_alternative<std::string>(*rVal)) {
      return std::nullopt;
    }

    bool isFloat = std::holds_alternative<double>(*lVal) ||
                   std::holds_alternative<double>(*rVal);

    auto toD = [](const CellValue &v) -> double {
      if (std::holds_alternative<double>(v)) return std::get<double>(v);
      if (std::holds_alternative<std::int64_t>(v))
        return static_cast<double>(std::get<std::int64_t>(v));
      if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? 1.0 : 0.0;
      return 0.0;
    };

    auto toI = [](const CellValue &v) -> std::int64_t {
      if (std::holds_alternative<std::int64_t>(v))
        return std::get<std::int64_t>(v);
      if (std::holds_alternative<double>(v))
        return static_cast<std::int64_t>(std::get<double>(v));
      if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? 1 : 0;
      return 0;
    };

    switch (dya->verb()) {
    case TokenKind::Plus: {
      if (!isFloat) {
        std::int64_t result{};
        if (__builtin_add_overflow(toI(*lVal), toI(*rVal), &result))
          return std::nullopt;
        return result;
      }
      return isFloat ? CellValue(toD(*lVal) + toD(*rVal))
                     : CellValue(toI(*lVal) + toI(*rVal));
    }
    case TokenKind::Minus: {
      if (!isFloat) {
        std::int64_t result{};
        if (__builtin_sub_overflow(toI(*lVal), toI(*rVal), &result))
          return std::nullopt;
        return result;
      }
      return isFloat ? CellValue(toD(*lVal) - toD(*rVal))
                     : CellValue(toI(*lVal) - toI(*rVal));
    }
    case TokenKind::Times: {
      if (!isFloat) {
        std::int64_t result{};
        if (__builtin_mul_overflow(toI(*lVal), toI(*rVal), &result))
          return std::nullopt;
        return result;
      }
      return isFloat ? CellValue(toD(*lVal) * toD(*rVal))
                     : CellValue(toI(*lVal) * toI(*rVal));
    }
    case TokenKind::Divide: {
      double r = toD(*rVal);
      if (r == 0.0) return std::nullopt;
      return CellValue(toD(*lVal) / r);
    }
    case TokenKind::Magnitude: {
      std::int64_t mod = toI(*rVal);
      if (mod == 0 ||
          (toI(*lVal) == std::numeric_limits<std::int64_t>::min() && mod == -1))
        return std::nullopt;
      return CellValue(toI(*lVal) % mod);
    }
    case TokenKind::Ceiling:
      return isFloat ? CellValue(std::max(toD(*lVal), toD(*rVal)))
                     : CellValue(std::max(toI(*lVal), toI(*rVal)));
    case TokenKind::Floor:
      return isFloat ? CellValue(std::min(toD(*lVal), toD(*rVal)))
                     : CellValue(std::min(toI(*lVal), toI(*rVal)));
    case TokenKind::Equal:
      return CellValue(*lVal == *rVal);
    case TokenKind::NotEqual:
      return CellValue(*lVal != *rVal);
    case TokenKind::LessThan:
      return CellValue(toD(*lVal) < toD(*rVal));
    case TokenKind::LessEqual:
      return CellValue(toD(*lVal) <= toD(*rVal));
    case TokenKind::GreaterThan:
      return CellValue(toD(*lVal) > toD(*rVal));
    case TokenKind::GreaterEqual:
      return CellValue(toD(*lVal) >= toD(*rVal));
    case TokenKind::And:
      return CellValue(zigzag::vortex::VortexCore::evaluateTruthiness(*lVal) &&
                       zigzag::vortex::VortexCore::evaluateTruthiness(*rVal));
    case TokenKind::Or:
      return CellValue(zigzag::vortex::VortexCore::evaluateTruthiness(*lVal) ||
                       zigzag::vortex::VortexCore::evaluateTruthiness(*rVal));
    default:
      break;
    }
  }

  return std::nullopt;
}

std::optional<CellRef> VPLCompiler::compileNode(const AstNode &node) {
  if (optimize_) {
    auto folded = tryFoldConstant(node);
    if (folded) {
      return emitConstant(*folded);
    }
  }

  if (const auto *prog = dynamic_cast<const Program *>(&node)) {
    return compileProgram(*prog);
  }
  if (const auto *sc = dynamic_cast<const ScalarExpr *>(&node)) {
    return compileScalar(*sc);
  }
  if (const auto *vec = dynamic_cast<const VectorExpr *>(&node)) {
    return compileVector(*vec);
  }
  if (const auto *dim = dynamic_cast<const DimensionExpr *>(&node)) {
    return compileDimension(*dim);
  }
  if (const auto *id = dynamic_cast<const IdentifierExpr *>(&node)) {
    return compileIdentifier(*id);
  }
  if (const auto *vb = dynamic_cast<const VerbExpr *>(&node)) {
    return compileVerb(*vb);
  }
  if (const auto *mon = dynamic_cast<const MonadicExpr *>(&node)) {
    return compileMonadic(*mon);
  }
  if (const auto *dya = dynamic_cast<const DyadicExpr *>(&node)) {
    return compileDyadic(*dya);
  }
  if (const auto *adv = dynamic_cast<const AdverbExpr *>(&node)) {
    return compileAdverb(*adv);
  }
  if (const auto *conj = dynamic_cast<const ConjunctionExpr *>(&node)) {
    return compileConjunction(*conj);
  }
  if (const auto *asg = dynamic_cast<const AssignExpr *>(&node)) {
    return compileAssign(*asg);
  }
  if (const auto *idx = dynamic_cast<const IndexingExpr *>(&node)) {
    return compileIndexing(*idx);
  }
  if (const auto *qd = dynamic_cast<const QuadExpr *>(&node)) {
    return compileQuad(*qd);
  }

  throw std::invalid_argument("unsupported VPL AST node in VM compiler");
}

void VPLCompiler::recordResultFacts(const CellRef cell) {
  lastResultEnclosed_ = enclosedCells_.contains(cell);
  if (const auto dims = dimensionValues_.find(cell);
      dims != dimensionValues_.end()) {
    lastResultDimensionValues_ = dims->second;
  } else {
    lastResultDimensionValues_.reset();
  }
}

std::optional<CellRef> VPLCompiler::compileProgram(const Program &prog) {
  std::optional<CellRef> lastRes;
  for (const auto &expr : prog.expressions()) {
    if (!expr)
      throw std::invalid_argument("VPL program contains a missing expression");
    lastRes = compileNode(*expr);
    if (!lastRes)
      throw std::invalid_argument("VPL expression has no VM-compiled result");
    lastResultCells_.clear();
    lastResultCells_.push_back(*lastRes);
    lastResultAxes_.clear();
    if (const auto axes = rankAxes_.find(*lastRes); axes != rankAxes_.end()) {
      lastResultAxes_ = axes->second;
    }
    lastResultDependencies_ = rankDependenciesOf(*lastRes);
    recordResultFacts(*lastRes);
  }
  return lastRes;
}

CellRef VPLCompiler::compileScalar(const ScalarExpr &expr) {
  if (expr.isString()) {
    return emitConstant(expr.stringValue());
  }
  if (expr.isFloat()) {
    return emitConstant(expr.floatValue());
  }
  return emitConstant(expr.intValue());
}

std::optional<CellRef> VPLCompiler::compileVector(const VectorExpr &expr) {
  const auto &elements = expr.elements();
  if (elements.empty())
    throw std::invalid_argument("empty VPL vectors are unsupported by the VM");

  std::vector<CellRef> cells;
  cells.reserve(elements.size());
  for (const auto &elem : elements) {
    if (!elem)
      throw std::invalid_argument("VPL vector contains a missing element");
    const auto cell = compileNode(*elem);
    if (!cell)
      throw std::invalid_argument("VPL vector element has no VM result");
    cells.push_back(*cell);
  }

  if (cells.empty())
    throw std::invalid_argument("VPL vector has no VM-compiled elements");

  if (std::ranges::all_of(cells, [&](const CellRef cell) {
        return dimensionValues_.contains(cell);
      })) {
    LowerContext ctx(*this);
    return lower::lowerDimensionVector(ctx, expr, cells);
  }
  if (std::ranges::any_of(cells, [&](const CellRef cell) {
        return rankAxes_.contains(cell);
      })) {
    LowerContext ctx(*this);
    return lower::lowerVectorOfRanks(ctx, expr, cells);
  }
  const auto axis = core_.dims().step;
  zigzag::expectWritten(
      core_.arena().linkChain(cells, axis, zigzag::DimVector::POS));
  rankAxes_[cells.front()] = {zigzag::DirectedDim{axis, DimVector::POS}};
  auto &dependencies       = rankDependencies_[cells.front()];
  for (const auto cell : cells) {
    core_.arena().consumeWork();
    mergeDependencies(dependencies, dependenciesOf(cell));
  }

  return cells.front();
}

CellRef VPLCompiler::compileDimension(const DimensionExpr &expr) {
  LowerContext ctx(*this);
  return lower::lowerDimension(ctx, expr);
}

std::optional<CellRef>
VPLCompiler::compileIdentifier(const IdentifierExpr &expr) {
  if (expr.name() == "H") return core_.home();
  auto it = varBindings_.find(expr.name());
  if (it != varBindings_.end()) {
    if (it->second) {
      if (const auto axes = varAxes_.find(expr.name());
          axes != varAxes_.end() && !axes->second.empty()) {
        rankAxes_[*it->second] = axes->second;
      }
      if (varEnclosed_.contains(expr.name())) {
        enclosedCells_.insert(*it->second);
      }
      if (const auto dims = varDimensionValues_.find(expr.name());
          dims != varDimensionValues_.end()) {
        dimensionValues_[*it->second] = dims->second;
      }
    }
    return it->second;
  }

  throw std::invalid_argument("unbound VPL identifier '" + expr.name() + "'");
}

CellRef VPLCompiler::compileVerb(const VerbExpr &expr) {
  (void)expr;
  throw std::invalid_argument("VPL first-class verb values are unsupported");
}

std::optional<CellRef> VPLCompiler::compileAssign(const AssignExpr &expr) {
  std::optional<CellRef> valCell =
      expr.value() ? compileNode(*expr.value()) : std::nullopt;
  if (!valCell)
    throw std::invalid_argument("VPL assignment requires a compiled value");
  CellRef bindOp = emitOp(OpcodeKind::Bind, "#BIND " + expr.target());
  emitInputConstant(bindOp, expr.target());
  emitInput(bindOp, *valCell);
  varBindings_[expr.target()] = valCell;
  const auto axes             = rankAxes_.find(*valCell);
  const std::vector<zigzag::DirectedDim> bindingAxes =
      axes == rankAxes_.end() ? std::vector<zigzag::DirectedDim>{}
                              : axes->second;
  if (bindingAxes.empty()) {
    varAxes_.erase(expr.target());
  } else {
    varAxes_[expr.target()] = bindingAxes;
  }
  const bool enclosed = enclosedCells_.contains(*valCell);
  if (enclosed) {
    varEnclosed_.insert(expr.target());
  } else {
    varEnclosed_.erase(expr.target());
  }
  std::optional<std::vector<zigzag::DirectedDim>> dimensionValues;
  if (const auto dims = dimensionValues_.find(*valCell);
      dims != dimensionValues_.end()) {
    dimensionValues                    = dims->second;
    varDimensionValues_[expr.target()] = dims->second;
  } else {
    varDimensionValues_.erase(expr.target());
  }
  assignedBindings_[expr.target()] = {
      .cell            = *valCell,
      .axes            = bindingAxes,
      .dependencies    = rankDependenciesOf(*valCell),
      .enclosed        = enclosed,
      .dimensionValues = std::move(dimensionValues),
  };
  return valCell;
}

std::optional<CellRef> VPLCompiler::compileMonadic(const MonadicExpr &expr) {
  if (expr.hasCustomVerb()) {
    LowerContext ctx(*this);
    if (const auto *adv =
            dynamic_cast<const AdverbExpr *>(expr.customVerb().get());
        adv != nullptr && adv->adverb() == TokenKind::Each) {
      return lower::lowerEachMonadic(ctx, expr, *adv);
    }
    if (const auto *conj =
            dynamic_cast<const ConjunctionExpr *>(expr.customVerb().get())) {
      if (conj->conjunction() == TokenKind::OuterProduct) {
        return lower::lowerOuterProductMonadic(ctx, expr, *conj);
      }
      return lower::lowerConjunctionMonadic(ctx, expr, *conj);
    }
    return lower::lowerFoldMonadic(ctx, expr);
  }

  {
    LowerContext ctx(*this);
    switch (expr.verb()) {
    case TokenKind::Iota:
      return lower::lowerIotaMonadic(ctx, expr);
    case TokenKind::Rho:
      return lower::lowerRhoMonadic(ctx, expr);
    case TokenKind::Transpose:
      return lower::lowerTransposeMonadic(ctx, expr);
    case TokenKind::ReverseFirst:
    case TokenKind::ReverseLast:
      return lower::lowerReverseMonadic(ctx, expr);
    case TokenKind::Power:
      return lower::lowerPowerMonadic(ctx, expr);
    case TokenKind::Times:
      return lower::lowerSignumMonadic(ctx, expr);
    case TokenKind::Floor:
      return lower::lowerFloorMonadic(ctx, expr);
    case TokenKind::Ceiling:
      return lower::lowerCeilingMonadic(ctx, expr);
    case TokenKind::Catenate:
      return lower::lowerRavelMonadic(ctx, expr);
    case TokenKind::Enclose:
      return lower::lowerEncloseMonadic(ctx, expr);
    case TokenKind::Disclose:
      return lower::lowerDiscloseMonadic(ctx, expr);
    default:
      break;
    }
  }

  if (!expr.right())
    throw std::invalid_argument("VPL monadic verb requires an operand");

  const std::optional<CellRef> rCell = compileNode(*expr.right());
  if (!rCell)
    throw std::invalid_argument("VPL monadic operand has no VM result");

  {
    LowerContext ctx(*this);
    switch (expr.verb()) {
    case TokenKind::Tally:
      return lower::lowerTallyMonadic(ctx, expr, *rCell);
    case TokenKind::GradeUp:
    case TokenKind::GradeDown:
      return lower::lowerGradeMonadic(ctx, expr, *rCell);
    case TokenKind::Where:
      return lower::lowerWhereMonadic(ctx, expr, *rCell);
    case TokenKind::Match:
      return lower::lowerMatchMonadic(ctx, expr, *rCell);
    case TokenKind::Hypertime:
      return lower::lowerHypertimeMonadic(ctx, expr, *rCell);
    case TokenKind::Transclude:
      return lower::lowerTranscludeMonadic(ctx, expr, *rCell);
    case TokenKind::Scrub:
      return lower::lowerScrubMonadic(ctx, expr, *rCell);
    default:
      break;
    }
    if (expr.verb() != TokenKind::Plus && rankAxes_.contains(*rCell)) {
      switch (expr.verb()) {
      case TokenKind::Minus:
      case TokenKind::Magnitude:
      case TokenKind::Not:
      case TokenKind::Divide:
        return lower::lowerMonadicArithmeticOnRank(ctx, expr, *rCell);
      default:
        // No card owns this verb on a rank (Member, Take, Drop, Compress,
        // comparisons, ... used monadically): keep today's refusal.
        lower::refuse(lower::msg::monadicOnRank);
      }
    }
  }

  switch (expr.verb()) {
  case TokenKind::Plus:
    // Identity
    return rCell;

  case TokenKind::Minus: {
    CellRef op  = emitOp(OpcodeKind::Neg, "#NEG");
    CellRef out = core_.arena().makeCell();
    if (rCell) emitInput(op, *rCell);
    emitOutput(op, out);
    return out;
  }
  case TokenKind::Magnitude: {
    CellRef op  = emitOp(OpcodeKind::Abs, "#ABS");
    CellRef out = core_.arena().makeCell();
    if (rCell) emitInput(op, *rCell);
    emitOutput(op, out);
    return out;
  }
  case TokenKind::Not: {
    CellRef op  = emitOp(OpcodeKind::Not, "#NOT");
    CellRef out = core_.arena().makeCell();
    if (rCell) emitInput(op, *rCell);
    emitOutput(op, out);
    return out;
  }
  case TokenKind::Divide: {
    // Reciprocal: 1.0 / rCell
    CellRef one = emitConstant(1.0);
    CellRef op  = emitOp(OpcodeKind::Div, "#RECIPROCAL");
    emitInput(op, one);
    if (rCell) emitInput(op, *rCell);
    CellRef out = core_.arena().makeCell();
    emitOutput(op, out);
    return out;
  }
  default:
    lower::refuse(lower::msg::monadicUnsupported);
  }
}

CellRef VPLCompiler::compileDyadic(const DyadicExpr &expr) {
  LowerContext ctx(*this);
  // A custom verb has verb() == Error, so the plain-verb list below never
  // matches it. Its operands are compiled and checked in the same order as for
  // a plain verb, so every message is the one the generic path always gave.
  if (!expr.hasCustomVerb()) {
    switch (expr.verb()) {
    case TokenKind::Iota:
      return lower::lowerIotaDyadic(ctx, expr);
    case TokenKind::Rho:
      return isDimensionOperand(expr.right().get())
                 ? lower::lowerRhoViewDyadic(ctx, expr)
                 : lower::lowerRhoDyadic(ctx, expr);
    case TokenKind::Take:
      return lower::lowerTakeDyadic(ctx, expr);
    case TokenKind::Drop:
      return lower::lowerDropDyadic(ctx, expr);
    case TokenKind::Power:
      return lower::lowerPowerDyadic(ctx, expr);
    case TokenKind::Catenate:
      return lower::lowerCatenateDyadic(ctx, expr);
    case TokenKind::Scrub:
      return lower::lowerScrubDyadic(ctx, expr);
    default:
      break;
    }
  }

  if (!expr.left() || !expr.right())
    throw std::invalid_argument("VPL dyadic verb requires two operands");
  const auto lCell = compileNode(*expr.left());
  const auto rCell = compileNode(*expr.right());
  if (!lCell || !rCell)
    throw std::invalid_argument("VPL dyadic operands must compile to values");

  if (expr.hasCustomVerb()) {
    const AstNode *custom = expr.customVerb().get();
    if (const auto *adv = dynamic_cast<const AdverbExpr *>(custom)) {
      if (adv->adverb() == TokenKind::Each)
        return lower::lowerEachDyadic(ctx, expr, *adv, *lCell, *rCell);
      if (adv->adverb() == TokenKind::Reduce ||
          adv->adverb() == TokenKind::Scan)
        return lower::lowerFoldDyadic(ctx, expr, *adv, *lCell, *rCell);
    } else if (const auto *conj =
                   dynamic_cast<const ConjunctionExpr *>(custom)) {
      if (conj->conjunction() == TokenKind::OuterProduct)
        return lower::lowerOuterProductDyadic(ctx, expr, *conj, *lCell, *rCell);
      return lower::lowerConjunctionDyadic(ctx, expr, *conj, *lCell, *rCell);
    }
    // An adverb no card owns (Compress used as an adverb): today's refusal.
    if (rankAxes_.contains(*lCell) || rankAxes_.contains(*rCell)) {
      lower::refuse(lower::msg::dyadicOnRank);
    }
    lower::refuse(lower::msg::dyadicUnsupported);
  }

  switch (expr.verb()) {
  case TokenKind::Transpose:
    return lower::lowerTransposeDyadic(ctx, expr, *lCell, *rCell);
  case TokenKind::Member:
    return lower::lowerMemberDyadic(ctx, expr, *lCell, *rCell);
  case TokenKind::Match:
    return lower::lowerMatchDyadic(ctx, expr, *lCell, *rCell);
  case TokenKind::Compress:
    return lower::lowerCompressDyadic(ctx, expr, *lCell, *rCell);
  default:
    break;
  }
  if (rankAxes_.contains(*lCell) || rankAxes_.contains(*rCell)) {
    switch (expr.verb()) {
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Times:
    case TokenKind::Divide:
    case TokenKind::Magnitude:
    case TokenKind::Ceiling:
    case TokenKind::Floor:
    case TokenKind::Equal:
    case TokenKind::NotEqual:
    case TokenKind::LessThan:
    case TokenKind::LessEqual:
    case TokenKind::GreaterThan:
    case TokenKind::GreaterEqual:
    case TokenKind::And:
    case TokenKind::Or:
      return lower::lowerDyadicArithmeticOnRank(ctx, expr, *lCell, *rCell);
    default:
      // No card owns this verb on ranks (Where, Tally, Hypertime, ...):
      // keep today's refusal.
      lower::refuse(lower::msg::dyadicOnRank);
    }
  }

  OpcodeKind kind;
  std::string_view l;

  switch (expr.verb()) {
  case TokenKind::Plus:
    kind = OpcodeKind::Add;
    l    = "#ADD";
    break;
  case TokenKind::Minus:
    kind = OpcodeKind::Sub;
    l    = "#SUB";
    break;
  case TokenKind::Times:
    kind = OpcodeKind::Mul;
    l    = "#MUL";
    break;
  case TokenKind::Divide:
    kind = OpcodeKind::Div;
    l    = "#DIV";
    break;
  case TokenKind::Magnitude:
    kind = OpcodeKind::Mod;
    l    = "#MOD";
    break;
  case TokenKind::Ceiling:
    kind = OpcodeKind::Max;
    l    = "#MAX";
    break;
  case TokenKind::Floor:
    kind = OpcodeKind::Min;
    l    = "#MIN";
    break;
  case TokenKind::Equal:
    kind = OpcodeKind::Eq;
    l    = "#EQ";
    break;
  case TokenKind::NotEqual:
    kind = OpcodeKind::Neq;
    l    = "#NEQ";
    break;
  case TokenKind::LessThan:
    kind = OpcodeKind::Lt;
    l    = "#LT";
    break;
  case TokenKind::LessEqual:
    kind = OpcodeKind::Lte;
    l    = "#LTE";
    break;
  case TokenKind::GreaterThan:
    kind = OpcodeKind::Gt;
    l    = "#GT";
    break;
  case TokenKind::GreaterEqual:
    kind = OpcodeKind::Gte;
    l    = "#GTE";
    break;
  case TokenKind::And:
    kind = OpcodeKind::And;
    l    = "#AND";
    break;
  case TokenKind::Or:
    kind = OpcodeKind::Or;
    l    = "#OR";
    break;
  default:
    lower::refuse(lower::msg::dyadicUnsupported);
  }

  CellRef op = emitOp(kind, l);
  if (lCell) emitInput(op, *lCell);
  if (rCell) emitInput(op, *rCell);
  CellRef out = core_.arena().makeCell();
  emitOutput(op, out);
  return out;
}

CellRef VPLCompiler::compileAdverb(const AdverbExpr &expr) {
  (void)expr;
  throw std::invalid_argument("unsupported VPL adverb in VM compiler");
}

CellRef VPLCompiler::compileConjunction(const ConjunctionExpr &expr) {
  LowerContext ctx(*this);
  return lower::lowerConjunctionExpr(ctx, expr);
}

std::optional<CellRef> VPLCompiler::compileIndexing(const IndexingExpr &expr) {
  LowerContext ctx(*this);
  return lower::lowerIndexing(ctx, expr);
}

std::optional<CellRef> VPLCompiler::compileQuad(const QuadExpr &expr) {
  LowerContext ctx(*this);
  return lower::lowerQuad(ctx, expr);
}

CellRef VPLCompiler::linkAsExecutable(CellRef entryOp) {
  // Link posward off home along +d.spin
  zigzag::expectWritten(
      core_.arena().link(core_.home(), core_.dims().spin, false, entryOp));
  // Spawn main execution cursor
  return vm_.spawnCursor(entryOp, "main");
}

void VPLCompiler::linkAsLibrary(CellRef entryOp, std::string_view moduleName,
                                std::string_view symbolName) {
  core_.exportSymbol(core_.getOrCreateModule(moduleName), symbolName, entryOp);
}

CompilationResult VPLCompiler::compile(const Program &program,
                                       const CompilationOptions &options) {
  zigzag::vortex::AssemblyTransaction transaction(vm_);
  entryOp_.reset();
  currentOp_.reset();
  allOps_.clear();
  varBindings_.clear();
  varAxes_.clear();
  rankAxes_.clear();
  rankDependencies_.clear();
  cellDependencies_.clear();
  assignedBindings_.clear();
  enclosedCells_.clear();
  dimensionValues_.clear();
  varEnclosed_.clear();
  varDimensionValues_.clear();
  printRequested_     = false;
  lastResultEnclosed_ = false;
  lastResultDimensionValues_.reset();
  lastResultCells_.clear();
  lastResultAxes_.clear();
  lastResultDependencies_.clear();
  optimize_ = options.optimize;

  for (const auto &[name, binding] : options.initialBindings) {
    std::optional<CellRef> cell = binding.origin();
    if (!cell && binding.isScalar()) {
      if (binding.isString()) {
        cell = emitConstant(binding.scalarString());
      } else if (binding.isFloat()) {
        cell = emitConstant(binding.scalarFloat());
      } else {
        cell = emitConstant(binding.scalarInt());
      }
    }
    varBindings_[name]                    = cell;
    varAxes_[name]                        = binding.axes();
    std::vector<CellRef> rankDependencies = binding.dependencies();
    if (cell) {
      auto members = binding.collectCells(core_.arena());
      if (members.empty()) members.push_back(*cell);
      for (const auto member : members) {
        core_.arena().consumeWork();
        std::vector<CellRef> memberDependencies;
        if (binding.axes().empty()) {
          memberDependencies = binding.dependencies();
        }
        if (core_.arena().resolveForeign(member)) {
          const std::vector<CellRef> source{member};
          mergeDependencies(memberDependencies, source);
        }
        if (!memberDependencies.empty()) {
          mergeDependencies(cellDependencies_[member], memberDependencies);
        }
        mergeDependencies(rankDependencies, memberDependencies);
      }
      if (!binding.axes().empty()) {
        rankDependencies_[*cell] = std::move(rankDependencies);
      }
    }
    if (cell && !binding.axes().empty()) {
      rankAxes_[*cell] = binding.axes();
    }
    if (cell && binding.isEnclosed()) {
      enclosedCells_.insert(*cell);
      varEnclosed_.insert(name);
    }
  }

  try {
    const auto resultCell = compileProgram(program);
    if (lastResultCells_.empty() && resultCell) {
      lastResultCells_.push_back(*resultCell);
      if (const auto axes = rankAxes_.find(*resultCell);
          axes != rankAxes_.end()) {
        lastResultAxes_ = axes->second;
      }
      lastResultDependencies_ = rankDependenciesOf(*resultCell);
      recordResultFacts(*resultCell);
    }

    std::optional<CellRef> terminal;
    std::optional<CellRef> cursor;
    if (options.targetLibrary) {
      CellRef retOp = emitOp(OpcodeKind::Return, "#RETURN");
      for (CellRef r : lastResultCells_) {
        emitInput(retOp, r);
      }
      terminal = retOp;
      linkAsLibrary(entryOp_.value(), options.moduleName, options.symbolName);
    } else {
      CellRef haltOp = emitOp(OpcodeKind::Halt, "#HALT");
      for (CellRef r : lastResultCells_) {
        emitInput(haltOp, r);
      }
      terminal = haltOp;
      cursor   = linkAsExecutable(entryOp_.value());
    }

    std::string dis = disassemble(entryOp_);
    CompilationResult result{
        .success               = true,
        .entryOpcode           = entryOp_,
        .errorMessage          = "",
        .generatedOpcodes      = allOps_,
        .disassembly           = std::move(dis),
        .terminalOpcode        = terminal,
        .cursor                = cursor,
        .resultSlots           = lastResultCells_,
        .resultAxes            = lastResultAxes_,
        .resultDependencies    = lastResultDependencies_,
        .cellDependencies      = cellDependencies_,
        .assignedBindings      = assignedBindings_,
        .resultEnclosed        = lastResultEnclosed_,
        .resultPrint           = printRequested_,
        .resultDimensionValues = lastResultDimensionValues_,
    };
    transaction.commit();
    return result;
  } catch (const std::exception &err) {
    entryOp_.reset();
    currentOp_.reset();
    allOps_.clear();
    lastResultCells_.clear();
    lastResultAxes_.clear();
    lastResultDependencies_.clear();
    cellDependencies_.clear();
    rankDependencies_.clear();
    assignedBindings_.clear();
    enclosedCells_.clear();
    dimensionValues_.clear();
    lastResultEnclosed_ = false;
    lastResultDimensionValues_.reset();
    printRequested_ = false;
    return CompilationResult{
        .success          = false,
        .entryOpcode      = std::nullopt,
        .errorMessage     = std::string("Compilation error: ") + err.what(),
        .generatedOpcodes = {},
        .disassembly      = "",
    };
  }
}

CompilationResult VPLCompiler::compile(const AstNode &ast,
                                       const CompilationOptions &options) {
  if (const auto *prog = dynamic_cast<const Program *>(&ast)) {
    return compile(*prog, options);
  }
  std::vector<std::shared_ptr<AstNode>> exprs;
  exprs.push_back(
      std::shared_ptr<AstNode>(const_cast<AstNode *>(&ast), [](AstNode *) {}));
  Program prog(ast.location(), std::move(exprs));
  return compile(prog, options);
}

CompilationResult VPLCompiler::compile(std::string_view source,
                                       const CompilationOptions &options) {
  try {
    Parser parser(source);
    auto program = parser.parseProgram();
    if (!program) {
      return CompilationResult{
          .success          = false,
          .entryOpcode      = std::nullopt,
          .errorMessage     = "Parser returned null AST",
          .generatedOpcodes = {},
          .disassembly      = "",
      };
    }
    return compile(*program, options);
  } catch (const std::exception &err) {
    return CompilationResult{
        .success          = false,
        .entryOpcode      = std::nullopt,
        .errorMessage     = std::string("Compilation error: ") + err.what(),
        .generatedOpcodes = {},
        .disassembly      = "",
    };
  }
}

std::string VPLCompiler::disassemble(std::optional<CellRef> entryOp,
                                     std::size_t maxInstructions) const {
  if (!entryOp) return "(empty opcode stream)";

  std::ostringstream out;
  CellRef cur = *entryOp;
  std::unordered_set<CellRef> visited;

  while (maxInstructions-- > 0) {
    if (!visited.insert(cur).second) {
      out << "  ... (cycle detected to cell " << cur << ")\n";
      break;
    }

    auto kindOpt          = vm_.getOpcodeKind(cur);
    std::string_view mnem = kindOpt ? opcodeMnemonic(*kindOpt) : "UNKNOWN";

    std::string label = core_.arena().textOf(cur);

    out << "  [0x" << std::hex << std::setw(4) << std::setfill('0') << cur
        << std::dec << "] #" << std::left << std::setw(8) << std::setfill(' ')
        << mnem;

    if (!label.empty()) {
      out << " \"" << label << "\"";
    }

    // Inputs
    std::vector<CellRef> inCells = core_.inputsOf(cur);
    if (!inCells.empty()) {
      out << " in: [";
      for (std::size_t i = 0; i < inCells.size(); ++i) {
        if (i > 0) out << ", ";
        CellRef inC = inCells[i];
        out << "c#" << inC;
        std::string txt = core_.arena().textOf(inC);
        if (!txt.empty()) {
          out << "(\"" << txt << "\")";
        } else if (auto iVal = core_.arena().asInt64(inC)) {
          out << "(" << *iVal << ")";
        } else if (auto dVal = core_.arena().asDouble(inC)) {
          out << "(" << *dVal << ")";
        }
      }
      out << "]";
    }

    // Outputs
    std::vector<CellRef> outCells = core_.outputsOf(cur);
    if (!outCells.empty()) {
      out << " -> out: [";
      for (std::size_t i = 0; i < outCells.size(); ++i) {
        if (i > 0) out << ", ";
        out << "c#" << outCells[i];
      }
      out << "]";
    }

    out << "\n";

    const auto next = core_.arena().linked(cur, core_.dims().spin, false);
    if (!next) break;
    cur = *next;
  }

  return out.str();
}

std::string VPLCompiler::renderAST(const AstNode &node) {
  std::ostringstream oss;
  formatASTNode(oss, node, "", true);
  return oss.str();
}

xanadu::MicroversionId
VPLCompiler::exportToStore(xanadu::Store &store,
                           const xanadu::MicroversionId &parent) const {
  return zigzag::vortex::exportProgram(core_, vm_, allOps_, store,
                                       parent.isZero() ? std::nullopt
                                                       : std::optional{parent});
}

} // namespace xanadu::vpl
