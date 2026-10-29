/**
 * @file compiler.cpp
 * @brief Implementation of VPL Compiler translating AST to Vortex bytecode.
 */
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/vortex/assembly_transaction.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>

#include "common/xanadu/vpl/lexer.hpp"
#include "common/xanadu/vpl/parser.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"

namespace xanadu::vpl {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

namespace {

CellRef requireCompiledCell(const std::optional<CellRef> result) {
  if (!result) {
    throw std::invalid_argument("VPL expression has no compiled cell");
  }
  return *result;
}

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
  case OpcodeKind::Fail:
    return "FAIL";
  case OpcodeKind::Cut:
    return "CUT";
  case OpcodeKind::ChoiceDepth:
    return "CHOICEDEPTH";
  default:
    return "UNKNOWN";
  }
}

VPLCompiler::VPLCompiler(zigzag::vortex::VortexCore &core,
                         zigzag::vortex::VortexVM &vm)
    : core_(core), vm_(vm) {}

CellRef VPLCompiler::emitOp(OpcodeKind kind, std::string_view label) {
  CellRef op = vm_.mintOpcode(kind, label);
  if (!entryOp_) {
    entryOp_ = op;
  }
  if (currentOp_) {
    zigzag::expectWritten(
        core_.arena().link(*currentOp_, core_.dims().spin, false, op));
  }
  currentOp_ = op;
  allOps_.push_back(op);
  return op;
}

void VPLCompiler::emitInput(CellRef op, CellRef operand) {
  core_.bindInput(op, operand);
}

void VPLCompiler::emitOutput(CellRef op, CellRef target) {
  core_.bindOutput(op, target);
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

std::optional<CellRef> VPLCompiler::compileProgram(const Program &prog) {
  std::optional<CellRef> lastRes;
  for (const auto &expr : prog.expressions()) {
    if (!expr)
      throw std::invalid_argument("VPL program contains a missing expression");
    lastRes = compileNode(*expr);
    if (!lastRes)
      throw std::invalid_argument("VPL expression has no VM-compiled result");
    lastResultCells_.push_back(*lastRes);
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

  // Chain vector cells along +d.step via the shared batch-link primitive.
  zigzag::expectWritten(core_.arena().linkChain(cells, core_.dims().step,
                                                zigzag::DimVector::POS));

  return cells.front();
}

CellRef VPLCompiler::compileDimension(const DimensionExpr &expr) {
  DimRef dim = resolveDimension(expr.name());
  return emitConstant(static_cast<std::int64_t>(dim));
}

std::optional<CellRef>
VPLCompiler::compileIdentifier(const IdentifierExpr &expr) {
  auto it = varBindings_.find(expr.name());
  if (it != varBindings_.end()) {
    return it->second;
  }

  CellRef resOp = emitOp(OpcodeKind::Resolve, "#RESOLVE " + expr.name());
  emitInputConstant(resOp, expr.name());
  CellRef outCell = core_.arena().makeCell();
  emitOutput(resOp, outCell);
  return outCell;
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
  return valCell;
}

std::optional<CellRef> VPLCompiler::compileMonadic(const MonadicExpr &expr) {
  if (expr.hasCustomVerb()) {
    if (const auto *adv =
            dynamic_cast<const AdverbExpr *>(expr.customVerb().get())) {
      TokenKind adverb    = adv->adverb();
      TokenKind innerVerb = TokenKind::Plus;
      if (const auto *v =
              dynamic_cast<const VerbExpr *>(adv->operand().get())) {
        innerVerb = v->verb();
      }

      OpcodeKind opKind = OpcodeKind::Add;
      std::string_view opLabel =
          adverb == TokenKind::Reduce ? "#REDUCE_ADD" : "#SCAN_ADD";
      if (innerVerb == TokenKind::Plus) {
        // Addition is the only currently supported reduction primitive.
      } else if (innerVerb == TokenKind::Times) {
        opKind  = OpcodeKind::Mul;
        opLabel = (adverb == TokenKind::Reduce) ? "#REDUCE_MUL" : "#SCAN_MUL";
      } else if (innerVerb == TokenKind::Ceiling) {
        opKind  = OpcodeKind::Max;
        opLabel = (adverb == TokenKind::Reduce) ? "#REDUCE_MAX" : "#SCAN_MAX";
      } else if (innerVerb == TokenKind::Floor) {
        opKind  = OpcodeKind::Min;
        opLabel = (adverb == TokenKind::Reduce) ? "#REDUCE_MIN" : "#SCAN_MIN";
      } else {
        throw std::invalid_argument(
            "VPL VM reduce/scan supports only +, *, maximum, and minimum");
      }

      if (adverb == TokenKind::Reduce) {
        const auto *vec = dynamic_cast<const VectorExpr *>(expr.right().get());
        if (!vec)
          throw std::invalid_argument(
              "VPL VM reduce currently requires a literal vector");
        std::vector<CellRef> operandCells;
        for (const auto &el : vec->elements()) {
          if (!el)
            throw std::invalid_argument(
                "VPL reduction rank contains a missing element");
          operandCells.push_back(requireCompiledCell(compileNode(*el)));
        }

        if (operandCells.empty())
          throw std::invalid_argument("VPL reduce requires a nonempty rank");
        if (operandCells.size() == 1) return operandCells.front();

        CellRef acc = operandCells[0];
        for (std::size_t i = 1; i < operandCells.size(); ++i) {
          CellRef op = emitOp(opKind, opLabel);
          emitInput(op, acc);
          emitInput(op, operandCells[i]);
          CellRef out = core_.arena().makeCell();
          emitOutput(op, out);
          acc = out;
        }
        return acc;
      }

      if (adverb == TokenKind::Scan) {
        const auto *vec = dynamic_cast<const VectorExpr *>(expr.right().get());
        if (!vec)
          throw std::invalid_argument(
              "VPL VM scan currently requires a literal vector");
        std::vector<CellRef> operandCells;
        for (const auto &el : vec->elements()) {
          if (!el)
            throw std::invalid_argument("VPL scan rank has a missing element");
          operandCells.push_back(requireCompiledCell(compileNode(*el)));
        }

        if (operandCells.empty())
          throw std::invalid_argument("VPL scan requires a nonempty rank");

        std::vector<CellRef> scanCells;
        CellRef acc = operandCells[0];
        scanCells.push_back(acc);

        for (std::size_t i = 1; i < operandCells.size(); ++i) {
          CellRef op = emitOp(opKind, opLabel);
          emitInput(op, acc);
          emitInput(op, operandCells[i]);
          CellRef out = core_.arena().makeCell();
          emitOutput(op, out);
          scanCells.push_back(out);
          acc = out;
        }

        for (std::size_t i = 0; i + 1 < scanCells.size(); ++i) {
          zigzag::expectWritten(core_.arena().link(
              scanCells[i], core_.dims().step, false, scanCells[i + 1]));
        }
        return scanCells.front();
      }

      if (adverb == TokenKind::Each) {
        throw std::invalid_argument("VPL VM each adverb is unsupported");
      }
    }
    throw std::invalid_argument("unsupported VPL custom verb in VM compiler");
  }

  if (expr.verb() == TokenKind::Iota || expr.verb() == TokenKind::Catenate ||
      expr.verb() == TokenKind::Enclose || expr.verb() == TokenKind::Disclose ||
      expr.verb() == TokenKind::Rho || expr.verb() == TokenKind::Transpose ||
      expr.verb() == TokenKind::ReverseFirst ||
      expr.verb() == TokenKind::ReverseLast ||
      expr.verb() == TokenKind::Power || expr.verb() == TokenKind::Times ||
      expr.verb() == TokenKind::Floor || expr.verb() == TokenKind::Ceiling) {
    throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
  }

  if (!expr.right())
    throw std::invalid_argument("VPL monadic verb requires an operand");

  const std::optional<CellRef> rCell = compileNode(*expr.right());
  if (!rCell)
    throw std::invalid_argument("VPL monadic operand has no VM result");

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
    throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
  }
}

CellRef VPLCompiler::compileDyadic(const DyadicExpr &expr) {
  if (expr.verb() == TokenKind::Iota || expr.verb() == TokenKind::Catenate ||
      expr.verb() == TokenKind::Rho || expr.verb() == TokenKind::Take ||
      expr.verb() == TokenKind::Drop || expr.verb() == TokenKind::Scrub ||
      expr.verb() == TokenKind::Power) {
    throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
  }

  if (!expr.left() || !expr.right())
    throw std::invalid_argument("VPL dyadic verb requires two operands");
  const auto lCell = compileNode(*expr.left());
  const auto rCell = compileNode(*expr.right());
  if (!lCell || !rCell)
    throw std::invalid_argument("VPL dyadic operands must compile to values");

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
    throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
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
  (void)expr;
  throw std::invalid_argument("VPL conjunctions are unsupported by the VM");
}

std::optional<CellRef> VPLCompiler::compileIndexing(const IndexingExpr &expr) {
  (void)expr;
  throw std::invalid_argument("VPL indexing is unsupported by the VM");
}

std::optional<CellRef> VPLCompiler::compileQuad(const QuadExpr &expr) {
  (void)expr;
  throw std::invalid_argument("VPL quad IO is unsupported by the VM");
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
  lastResultCells_.clear();
  optimize_ = options.optimize;

  try {
    const auto resultCell = compileProgram(program);
    if (lastResultCells_.empty() && resultCell) {
      lastResultCells_.push_back(*resultCell);
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
        .success          = true,
        .entryOpcode      = entryOp_,
        .errorMessage     = "",
        .generatedOpcodes = allOps_,
        .disassembly      = std::move(dis),
        .terminalOpcode   = terminal,
        .cursor           = cursor,
        .resultSlots      = lastResultCells_,
    };
    transaction.commit();
    return result;
  } catch (const std::exception &err) {
    entryOp_.reset();
    currentOp_.reset();
    allOps_.clear();
    lastResultCells_.clear();
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
  xanadu::MicroversionId ver = parent;
  if (!store.homeCell()) {
    ver = store.sliceGenesis(ver);
  }

  const auto &arena = core_.arena();
  std::unordered_map<CellRef, CellRef> cellMap;
  cellMap[core_.home()] = store.homeCell().value();

  auto manifold = store.rebuildManifold(ver);

  // 1. Replicate dimensions
  std::unordered_map<DimRef, DimRef> dimMap;
  for (CellRef c = 1; c <= arena.cellCount(); ++c) {
    if (!arena.contains(c)) continue;
    std::string name = arena.textOf(c);
    if (!name.empty() && name.starts_with("d.")) {
      // name is non-empty here, the one way getOrCreate() can refuse a store.
      if (const auto dim = zigzag::DimensionRegistry::instance().getOrCreate(
              store, ver, manifold, name)) {
        dimMap[c]  = *dim;
        cellMap[c] = *dim;
      }
    }
  }

  // 2. Mint cells
  for (CellRef c = 1; c <= arena.cellCount(); ++c) {
    if (!arena.contains(c)) continue;
    if (cellMap.contains(c)) continue;

    if (auto dVal = arena.asDouble(c)) {
      ver = store.makeScalarCell(ver, *dVal);
    } else if (auto iVal = arena.asInt64(c)) {
      ver = store.makeScalarCell(ver, *iVal);
    } else if (auto bVal = arena.asBool(c)) {
      ver = store.makeScalarCell(ver, *bVal);
    } else {
      ver = store.makeCell(ver, arena.textOf(c));
    }
    cellMap[c] = store.cellRefOf(ver).value();
    manifold.advanceOrRefold(store, ver);
  }

  // 3. Link edges (posward links only)
  for (CellRef c = 1; c <= arena.cellCount(); ++c) {
    if (!arena.contains(c) || !cellMap.contains(c)) continue;
    CellRef from = cellMap.at(c);

    for (const auto &[dimRef, mappedDim] : dimMap) {
      const auto target = arena.linked(c, dimRef, DimVector::POS);
      if (target && cellMap.contains(*target)) {
        CellRef to = cellMap.at(*target);
        ver        = store.setLink(ver, from, mappedDim, DimVector::POS, to);
        manifold.advanceOrRefold(store, ver);
      }
    }
  }

  return ver;
}

} // namespace xanadu::vpl
