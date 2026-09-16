/**
 * @file compiler.cpp
 * @brief Implementation of VPL Compiler translating AST to Vortex bytecode.
 */
#include "common/xanadu/vpl/compiler.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_set>

#include "common/xanadu/vpl/lexer.hpp"
#include "common/xanadu/vpl/parser.hpp"

namespace xanadu::vpl {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::noCell;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

namespace {

std::string_view verbName(TokenKind kind) noexcept {
  switch (kind) {
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
  default:
    return "UNKNOWN";
  }
}

VPLCompiler::VPLCompiler(zigzag::vortex::VortexCore &core,
                         zigzag::vortex::VortexVM &vm)
    : core_(core), vm_(vm) {}

CellRef VPLCompiler::emitOp(OpcodeKind kind, std::string_view label) {
  CellRef op = vm_.mintOpcode(kind, label);
  if (entryOp_ == noCell) {
    entryOp_ = op;
  }
  if (currentOp_ != noCell) {
    core_.arena().link(currentOp_, core_.dims().spin, false, op);
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
  core_.value(c, 0, -1, val);
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

  // Walk dims.dims to see if dimension was previously minted
  CellRef curr = dims.dims;
  std::unordered_set<CellRef> visited;
  while (curr != noCell && visited.insert(curr).second) {
    if (core_.arena().textOf(curr) == name) {
      return curr;
    }
    curr = core_.arena().linked(curr, dims.dims, DimVector::POS);
  }

  return core_.mintDimension(name);
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
        return std::abs(std::get<std::int64_t>(*foldedR));
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
    case TokenKind::Plus:
      return isFloat ? CellValue(toD(*lVal) + toD(*rVal))
                     : CellValue(toI(*lVal) + toI(*rVal));
    case TokenKind::Minus:
      return isFloat ? CellValue(toD(*lVal) - toD(*rVal))
                     : CellValue(toI(*lVal) - toI(*rVal));
    case TokenKind::Times:
      return isFloat ? CellValue(toD(*lVal) * toD(*rVal))
                     : CellValue(toI(*lVal) * toI(*rVal));
    case TokenKind::Divide: {
      double r = toD(*rVal);
      return r != 0.0 ? CellValue(toD(*lVal) / r) : CellValue(0.0);
    }
    case TokenKind::Magnitude: {
      std::int64_t mod = toI(*rVal);
      return mod != 0 ? CellValue(toI(*lVal) % mod)
                      : CellValue(static_cast<std::int64_t>(0));
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

CellRef VPLCompiler::compileNode(const AstNode &node) {
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

  return noCell;
}

CellRef VPLCompiler::compileProgram(const Program &prog) {
  CellRef lastRes = noCell;
  for (const auto &expr : prog.expressions()) {
    if (expr) {
      lastRes = compileNode(*expr);
      if (lastRes != noCell) {
        lastResultCells_.push_back(lastRes);
      }
    }
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

CellRef VPLCompiler::compileVector(const VectorExpr &expr) {
  const auto &elements = expr.elements();
  if (elements.empty()) return noCell;

  std::vector<CellRef> cells;
  cells.reserve(elements.size());
  for (const auto &elem : elements) {
    if (elem) {
      cells.push_back(compileNode(*elem));
    }
  }

  if (cells.empty()) return noCell;

  // Chain vector cells along +d.step
  for (std::size_t i = 0; i + 1 < cells.size(); ++i) {
    if (cells[i] != noCell && cells[i + 1] != noCell) {
      core_.arena().link(cells[i], core_.dims().step, false, cells[i + 1]);
    }
  }

  return cells.front();
}

CellRef VPLCompiler::compileDimension(const DimensionExpr &expr) {
  DimRef dim = resolveDimension(expr.name());
  return emitConstant(static_cast<std::int64_t>(dim));
}

CellRef VPLCompiler::compileIdentifier(const IdentifierExpr &expr) {
  auto it = varBindings_.find(expr.name());
  if (it != varBindings_.end() && it->second != noCell) {
    return it->second;
  }

  CellRef resOp = emitOp(OpcodeKind::Resolve, "#RESOLVE " + expr.name());
  emitInputConstant(resOp, expr.name());
  CellRef outCell = core_.arena().makeCell();
  emitOutput(resOp, outCell);
  return outCell;
}

CellRef VPLCompiler::compileVerb(const VerbExpr &expr) {
  return emitConstant(std::string(verbName(expr.verb())));
}

CellRef VPLCompiler::compileAssign(const AssignExpr &expr) {
  CellRef valCell = expr.value() ? compileNode(*expr.value()) : noCell;
  CellRef bindOp  = emitOp(OpcodeKind::Bind, "#BIND " + expr.target());
  emitInputConstant(bindOp, expr.target());
  if (valCell != noCell) {
    emitInput(bindOp, valCell);
  }
  varBindings_[expr.target()] = valCell;
  return valCell;
}

CellRef VPLCompiler::compileMonadic(const MonadicExpr &expr) {
  if (expr.hasCustomVerb()) {
    if (const auto *adv =
            dynamic_cast<const AdverbExpr *>(expr.customVerb().get())) {
      TokenKind adverb    = adv->adverb();
      TokenKind innerVerb = TokenKind::Plus;
      if (const auto *v =
              dynamic_cast<const VerbExpr *>(adv->operand().get())) {
        innerVerb = v->verb();
      }

      OpcodeKind opKind        = OpcodeKind::Add;
      std::string_view opLabel = "#REDUCE_ADD";
      if (innerVerb == TokenKind::Times) {
        opKind  = OpcodeKind::Mul;
        opLabel = (adverb == TokenKind::Reduce) ? "#REDUCE_MUL" : "#SCAN_MUL";
      } else if (innerVerb == TokenKind::Ceiling) {
        opKind  = OpcodeKind::Max;
        opLabel = (adverb == TokenKind::Reduce) ? "#REDUCE_MAX" : "#SCAN_MAX";
      } else if (innerVerb == TokenKind::Floor) {
        opKind  = OpcodeKind::Min;
        opLabel = (adverb == TokenKind::Reduce) ? "#REDUCE_MIN" : "#SCAN_MIN";
      } else {
        opLabel = (adverb == TokenKind::Reduce) ? "#REDUCE_ADD" : "#SCAN_ADD";
      }

      if (adverb == TokenKind::Reduce) {
        std::vector<CellRef> operandCells;
        if (const auto *vec =
                dynamic_cast<const VectorExpr *>(expr.right().get())) {
          for (const auto &el : vec->elements()) {
            if (el) operandCells.push_back(compileNode(*el));
          }
        } else if (expr.right()) {
          operandCells.push_back(compileNode(*expr.right()));
        }

        if (operandCells.empty()) return core_.home();
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
        std::vector<CellRef> operandCells;
        if (const auto *vec =
                dynamic_cast<const VectorExpr *>(expr.right().get())) {
          for (const auto &el : vec->elements()) {
            if (el) operandCells.push_back(compileNode(*el));
          }
        } else if (expr.right()) {
          operandCells.push_back(compileNode(*expr.right()));
        }

        if (operandCells.empty()) return core_.home();

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
          core_.arena().link(scanCells[i], core_.dims().step, false,
                             scanCells[i + 1]);
        }
        return scanCells.front();
      }

      if (adverb == TokenKind::Each) {
        CellRef r  = expr.right() ? compileNode(*expr.right()) : core_.home();
        CellRef op = emitOp(OpcodeKind::Call, "#EACH");
        emitInput(op, r);
        CellRef out = core_.arena().makeCell();
        emitOutput(op, out);
        return out;
      }
    }
  }

  CellRef rCell = expr.right() ? compileNode(*expr.right()) : noCell;

  switch (expr.verb()) {
  case TokenKind::Iota: {
    // Monadic Iota: ⍳N -> Mint N cells on d.1
    std::int64_t n = 0;
    if (rCell != noCell) {
      if (auto iVal = core_.arena().asInt64(rCell)) {
        n = *iVal;
      }
    }
    DimRef dim   = resolveDimension("d.1");
    CellRef prev = core_.home();
    CellRef head = noCell;
    for (std::int64_t i = 1; i <= n; ++i) {
      CellRef fresh = core_.arena().makeCell();
      if (head == noCell) head = fresh;
      CellRef newOp = emitOp(OpcodeKind::New, "#MINT_CELL");
      emitInput(newOp, prev);
      emitInputConstant(newOp, static_cast<std::int64_t>(dim));
      emitInputConstant(newOp, static_cast<std::int64_t>(1));
      emitOutput(newOp, fresh);

      CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
      emitInput(valOp, fresh);
      emitInputConstant(valOp, static_cast<std::int64_t>(0));
      emitInputConstant(valOp, static_cast<std::int64_t>(-1));
      emitInputConstant(valOp, i);

      prev = fresh;
    }
    return head != noCell ? head : core_.home();
  }
  case TokenKind::Plus:
    // Identity
    return rCell;

  case TokenKind::Minus: {
    CellRef op  = emitOp(OpcodeKind::Neg, "#NEG");
    CellRef out = core_.arena().makeCell();
    if (rCell != noCell) emitInput(op, rCell);
    emitOutput(op, out);
    return out;
  }
  case TokenKind::Magnitude: {
    CellRef op  = emitOp(OpcodeKind::Abs, "#ABS");
    CellRef out = core_.arena().makeCell();
    if (rCell != noCell) emitInput(op, rCell);
    emitOutput(op, out);
    return out;
  }
  case TokenKind::Not: {
    CellRef op  = emitOp(OpcodeKind::Not, "#NOT");
    CellRef out = core_.arena().makeCell();
    if (rCell != noCell) emitInput(op, rCell);
    emitOutput(op, out);
    return out;
  }
  case TokenKind::Divide: {
    // Reciprocal: 1.0 / rCell
    CellRef one = emitConstant(1.0);
    CellRef op  = emitOp(OpcodeKind::Div, "#RECIPROCAL");
    emitInput(op, one);
    if (rCell != noCell) emitInput(op, rCell);
    CellRef out = core_.arena().makeCell();
    emitOutput(op, out);
    return out;
  }
  case TokenKind::Enclose: {
    // ⊂A: Enclose view into cell via OpcodeKind::Value
    CellRef box = core_.arena().makeCell();
    CellRef op  = emitOp(OpcodeKind::Value, "#ENCLOSE");
    emitInput(op, box);
    emitInputConstant(op, static_cast<std::int64_t>(0));
    emitInputConstant(op, static_cast<std::int64_t>(-1));
    if (rCell != noCell) emitInput(op, rCell);
    emitOutput(op, box);
    return box;
  }
  case TokenKind::Disclose: {
    // ⊃C: Resolve clone along -d.clone to master
    CellRef master = core_.arena().makeCell();
    CellRef op     = emitOp(OpcodeKind::Link, "#DISCLOSE");
    if (rCell != noCell) emitInput(op, rCell);
    emitInputConstant(op, static_cast<std::int64_t>(core_.dims().clone));
    emitInputConstant(op, static_cast<std::int64_t>(-1));
    emitOutput(op, master);
    return master;
  }
  case TokenKind::Rho: {
    // ⍴A: Monadic shape
    CellRef op  = emitOp(OpcodeKind::Link, "#SHAPE");
    CellRef out = core_.arena().makeCell();
    if (rCell != noCell) emitInput(op, rCell);
    emitInputConstant(op, static_cast<std::int64_t>(core_.dims().spin));
    emitInputConstant(op, static_cast<std::int64_t>(1));
    emitOutput(op, out);
    return out;
  }
  case TokenKind::Transpose:
  case TokenKind::ReverseFirst:
  case TokenKind::ReverseLast:
    // Transpose and reverse are O(1) view operations
    return rCell;

  default:
    return rCell != noCell ? rCell : core_.home();
  }
}

CellRef VPLCompiler::compileDyadic(const DyadicExpr &expr) {
  if (expr.verb() == TokenKind::Iota) {
    // Dyadic Iota: N ⍳ dim -> Mint N cells on dimension from home
    DimRef dim = resolveDimension("d.1");
    if (const auto *dimExpr =
            dynamic_cast<const DimensionExpr *>(expr.right().get())) {
      dim = resolveDimension(dimExpr->name());
    } else if (expr.right()) {
      CellRef rCell = compileNode(*expr.right());
      if (auto dVal = core_.arena().asInt64(rCell)) {
        dim = static_cast<DimRef>(*dVal);
      }
    }

    std::int64_t count = 0;
    if (const auto *leftScalar =
            dynamic_cast<const ScalarExpr *>(expr.left().get())) {
      count = leftScalar->intValue();
    } else if (expr.left()) {
      CellRef lCell = compileNode(*expr.left());
      if (auto iVal = core_.arena().asInt64(lCell)) {
        count = *iVal;
      }
    }

    CellRef prev = core_.home();
    CellRef head = noCell;
    for (std::int64_t i = 1; i <= count; ++i) {
      CellRef fresh = core_.arena().makeCell();
      if (head == noCell) head = fresh;
      CellRef newOp = emitOp(OpcodeKind::New, "#MINT_CELL");
      emitInput(newOp, prev);
      emitInputConstant(newOp, static_cast<std::int64_t>(dim));
      emitInputConstant(newOp, static_cast<std::int64_t>(1));
      emitOutput(newOp, fresh);

      CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
      emitInput(valOp, fresh);
      emitInputConstant(valOp, static_cast<std::int64_t>(0));
      emitInputConstant(valOp, static_cast<std::int64_t>(-1));
      emitInputConstant(valOp, i);

      prev = fresh;
    }
    return head != noCell ? head : core_.home();
  }

  CellRef lCell = expr.left() ? compileNode(*expr.left()) : noCell;
  CellRef rCell = expr.right() ? compileNode(*expr.right()) : noCell;

  OpcodeKind kind    = OpcodeKind::Nop;
  std::string_view l = "#NOP";

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
  case TokenKind::Rho:
    kind = OpcodeKind::Link;
    l    = "#RESHAPE";
    break;
  case TokenKind::Take:
    kind = OpcodeKind::Link;
    l    = "#TAKE";
    break;
  case TokenKind::Drop:
    kind = OpcodeKind::Link;
    l    = "#DROP";
    break;
  case TokenKind::Scrub:
    kind = OpcodeKind::Call;
    l    = "#SCRUB";
    break;
  default:
    kind = OpcodeKind::Nop;
    l    = "#NOP";
    break;
  }

  CellRef op = emitOp(kind, l);
  if (lCell != noCell) emitInput(op, lCell);
  if (rCell != noCell) emitInput(op, rCell);
  CellRef out = core_.arena().makeCell();
  emitOutput(op, out);
  return out;
}

CellRef VPLCompiler::compileAdverb(const AdverbExpr &expr) {
  if (expr.adverb() == TokenKind::Reduce) {
    // Fold along rank
    if (const auto *vec =
            dynamic_cast<const VectorExpr *>(expr.operand().get())) {
      const auto &elems = vec->elements();
      if (elems.empty()) return core_.home();
      if (elems.size() == 1) return compileNode(*elems[0]);

      CellRef acc = compileNode(*elems[0]);
      for (std::size_t i = 1; i < elems.size(); ++i) {
        CellRef cur = compileNode(*elems[i]);
        CellRef op  = emitOp(OpcodeKind::Add, "#REDUCE_ADD");
        emitInput(op, acc);
        emitInput(op, cur);
        CellRef out = core_.arena().makeCell();
        emitOutput(op, out);
        acc = out;
      }
      return acc;
    }
  } else if (expr.adverb() == TokenKind::Scan) {
    // Fold emitting intermediate cells
    if (const auto *vec =
            dynamic_cast<const VectorExpr *>(expr.operand().get())) {
      const auto &elems = vec->elements();
      if (elems.empty()) return core_.home();

      std::vector<CellRef> scanCells;
      CellRef acc = compileNode(*elems[0]);
      scanCells.push_back(acc);

      for (std::size_t i = 1; i < elems.size(); ++i) {
        CellRef cur = compileNode(*elems[i]);
        CellRef op  = emitOp(OpcodeKind::Add, "#SCAN_ADD");
        emitInput(op, acc);
        emitInput(op, cur);
        CellRef out = core_.arena().makeCell();
        emitOutput(op, out);
        scanCells.push_back(out);
        acc = out;
      }

      // Link scan cells along +d.step
      for (std::size_t i = 0; i + 1 < scanCells.size(); ++i) {
        core_.arena().link(scanCells[i], core_.dims().step, false,
                           scanCells[i + 1]);
      }
      return scanCells.front();
    }
  }

  CellRef opRef = expr.operand() ? compileNode(*expr.operand()) : noCell;
  CellRef op    = emitOp(OpcodeKind::Call, "#ADVERB");
  if (opRef != noCell) emitInput(op, opRef);
  CellRef out = core_.arena().makeCell();
  emitOutput(op, out);
  return out;
}

CellRef VPLCompiler::compileConjunction(const ConjunctionExpr &expr) {
  CellRef l  = expr.left() ? compileNode(*expr.left()) : noCell;
  CellRef r  = expr.right() ? compileNode(*expr.right()) : noCell;
  CellRef op = emitOp(OpcodeKind::Call, "#CONJUNCTION");
  if (l != noCell) emitInput(op, l);
  if (r != noCell) emitInput(op, r);
  CellRef out = core_.arena().makeCell();
  emitOutput(op, out);
  return out;
}

CellRef VPLCompiler::compileIndexing(const IndexingExpr &expr) {
  CellRef target = expr.target() ? compileNode(*expr.target()) : noCell;
  if (target == noCell) return core_.home();

  CellRef cur = target;
  for (const auto &idx : expr.indices()) {
    if (!idx) continue;
    CellRef idxCell    = compileNode(*idx);
    std::int64_t steps = 1;
    if (auto iVal = core_.arena().asInt64(idxCell)) {
      steps = *iVal;
    }
    for (std::int64_t s = 0; s < steps; ++s) {
      CellRef op = emitOp(OpcodeKind::Link, "#INDEX_WALK");
      emitInput(op, cur);
      emitInputConstant(op, static_cast<std::int64_t>(core_.dims().step));
      emitInputConstant(op, static_cast<std::int64_t>(1));
      CellRef next = core_.arena().makeCell();
      emitOutput(op, next);
      cur = next;
    }
  }
  return cur;
}

CellRef VPLCompiler::compileQuad(const QuadExpr &expr) {
  CellRef arg = expr.arg() ? compileNode(*expr.arg()) : noCell;

  if (expr.quadKind() == TokenKind::Quad) {
    CellRef op = emitOp(OpcodeKind::Nop, "#PRINT");
    if (arg != noCell) emitInput(op, arg);
    return arg != noCell ? arg : core_.home();
  }
  if (expr.quadKind() == TokenKind::QuadRead) {
    CellRef op = emitOp(OpcodeKind::Call, "#READ");
    if (arg != noCell) emitInput(op, arg);
    CellRef out = core_.arena().makeCell();
    emitOutput(op, out);
    return out;
  }
  if (expr.quadKind() == TokenKind::QuadSplit) {
    CellRef op = emitOp(OpcodeKind::Call, "#SPLIT");
    if (arg != noCell) emitInput(op, arg);
    CellRef out = core_.arena().makeCell();
    emitOutput(op, out);
    return out;
  }

  return core_.home();
}

void VPLCompiler::linkAsExecutable(CellRef entryOp) {
  if (entryOp == noCell) return;
  // Link posward off home along +d.spin
  core_.arena().link(core_.home(), core_.dims().spin, false, entryOp);
  // Spawn main execution cursor
  vm_.spawnCursor(entryOp, "main");
}

void VPLCompiler::linkAsLibrary(CellRef entryOp, std::string_view moduleName,
                                std::string_view symbolName) {
  if (entryOp == noCell) return;

  // Find or create module along +d.stdlib
  CellRef cur  = core_.arena().linked(core_.home(), core_.dims().stdlib, false);
  CellRef prev = core_.home();
  CellRef modCell   = noCell;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    if (core_.arena().textOf(cur) == moduleName) {
      modCell = cur;
      break;
    }
    prev = cur;
    cur  = core_.arena().linked(cur, core_.dims().stdlib, false);
  }

  if (modCell == noCell) {
    modCell = core_.arena().makeCell(moduleName);
    if (prev == core_.home()) {
      core_.arena().link(core_.home(), core_.dims().stdlib, false, modCell);
    } else {
      core_.arena().link(prev, core_.dims().stdlib, false, modCell);
    }
  }

  // Export symbol under module along +d.vars with entry opcode on +d.values
  CellRef symCell = core_.arena().makeCell(symbolName);
  core_.arena().link(symCell, core_.dims().values, false, entryOp);

  CellRef firstVar = core_.arena().linked(modCell, core_.dims().vars, false);
  if (firstVar == noCell) {
    core_.arena().link(modCell, core_.dims().vars, false, symCell);
  } else {
    CellRef curVar       = firstVar;
    std::size_t varLimit = core_.arena().cellCount() + 1;
    while (varLimit-- > 0) {
      CellRef next = core_.arena().linked(curVar, core_.dims().vars, false);
      if (next == noCell) {
        core_.arena().link(curVar, core_.dims().vars, false, symCell);
        break;
      }
      curVar = next;
    }
  }
}

CompilationResult VPLCompiler::compile(const Program &program,
                                       const CompilationOptions &options) {
  entryOp_   = noCell;
  currentOp_ = noCell;
  allOps_.clear();
  varBindings_.clear();
  lastResultCells_.clear();
  optimize_ = options.optimize;

  CellRef resultCell = compileProgram(program);
  if (lastResultCells_.empty() && resultCell != noCell) {
    lastResultCells_.push_back(resultCell);
  }

  if (options.targetLibrary) {
    CellRef retOp = emitOp(OpcodeKind::Return, "#RETURN");
    for (CellRef r : lastResultCells_) {
      emitInput(retOp, r);
    }
    linkAsLibrary(entryOp_, options.moduleName, options.symbolName);
  } else {
    CellRef haltOp = emitOp(OpcodeKind::Halt, "#HALT");
    for (CellRef r : lastResultCells_) {
      emitInput(haltOp, r);
    }
    linkAsExecutable(entryOp_);
  }

  std::string dis = disassemble(entryOp_);
  return CompilationResult{
      .success          = true,
      .entryOpcode      = entryOp_,
      .errorMessage     = "",
      .generatedOpcodes = allOps_,
      .disassembly      = std::move(dis),
  };
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
          .entryOpcode      = noCell,
          .errorMessage     = "Parser returned null AST",
          .generatedOpcodes = {},
          .disassembly      = "",
      };
    }
    return compile(*program, options);
  } catch (const std::exception &err) {
    return CompilationResult{
        .success          = false,
        .entryOpcode      = noCell,
        .errorMessage     = std::string("Compilation error: ") + err.what(),
        .generatedOpcodes = {},
        .disassembly      = "",
    };
  }
}

std::string VPLCompiler::disassemble(CellRef entryOp,
                                     std::size_t maxInstructions) const {
  if (entryOp == noCell) return "(empty opcode stream)";

  std::ostringstream out;
  CellRef cur = entryOp;
  std::unordered_set<CellRef> visited;

  while (cur != noCell && maxInstructions-- > 0) {
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

    cur = core_.arena().linked(cur, core_.dims().spin, false);
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
  if (store.homeCell() == zigzag::noCell) {
    ver = store.sliceGenesis(ver);
  }

  const auto &arena = core_.arena();
  std::unordered_map<CellRef, CellRef> cellMap;
  cellMap[core_.home()] = store.homeCell();

  auto manifold = store.rebuildManifold(ver);

  // 1. Replicate dimensions
  std::unordered_map<DimRef, DimRef> dimMap;
  for (CellRef c = 1; c <= arena.cellCount(); ++c) {
    if (!arena.contains(c)) continue;
    std::string name = arena.textOf(c);
    if (!name.empty() && name.starts_with("d.")) {
      DimRef existing = manifold.dimensionNamed(name, store);
      if (existing != zigzag::noCell) {
        dimMap[c] = existing;
      } else {
        auto minted = store.makeDimension(ver, name, &manifold);
        ver         = minted.version;
        dimMap[c]   = minted.dim;
        manifold    = store.rebuildManifold(ver);
      }
      cellMap[c] = dimMap[c];
    }
  }

  // 2. Mint cells
  for (CellRef c = 1; c <= arena.cellCount(); ++c) {
    if (!arena.contains(c)) continue;
    if (cellMap.count(c)) continue;

    if (auto dVal = arena.asDouble(c)) {
      ver = store.makeScalarCell(ver, *dVal);
    } else if (auto iVal = arena.asInt64(c)) {
      ver = store.makeScalarCell(ver, *iVal);
    } else if (auto bVal = arena.asBool(c)) {
      ver = store.makeScalarCell(ver, *bVal);
    } else {
      ver = store.makeCell(ver, arena.textOf(c));
    }
    cellMap[c] = store.cellRefOf(ver);
    static_cast<void>(manifold.advance(store, ver));
  }

  // 3. Link edges (posward links only)
  for (CellRef c = 1; c <= arena.cellCount(); ++c) {
    if (!arena.contains(c) || !cellMap.count(c)) continue;
    CellRef from = cellMap.at(c);

    for (const auto &[dimRef, mappedDim] : dimMap) {
      CellRef target = arena.linked(c, dimRef, DimVector::POS);
      if (target != zigzag::noCell && cellMap.count(target)) {
        CellRef to = cellMap.at(target);
        ver =
            store.setLink(ver, from, mappedDim, DimVector::POS, to, &manifold);
        static_cast<void>(manifold.advance(store, ver));
      }
    }
  }

  return ver;
}

} // namespace xanadu::vpl
