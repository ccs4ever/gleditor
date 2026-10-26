/**
 * @file compiler.cpp
 * @brief Implementation of VQL Compiler translating AST to Vortex bytecode.
 */
#include "common/xanadu/vql/compiler.hpp"

#include <cstdio>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "common/xanadu/vql/lexer.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"

namespace xanadu::vql {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

std::string_view
VQLCompiler::opcodeMnemonic(zigzag::vortex::OpcodeKind kind) noexcept {
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

VQLCompiler::VQLCompiler(zigzag::vortex::VortexCore &core,
                         zigzag::vortex::VortexVM &vm)
    : core_(core), vm_(vm) {}

CellRef VQLCompiler::emitOp(OpcodeKind kind, std::string_view label) {
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

void VQLCompiler::emitInput(CellRef op, CellRef operand) {
  core_.bindInput(op, operand);
}

void VQLCompiler::emitOutput(CellRef op, CellRef target) {
  core_.bindOutput(op, target);
}

CellRef VQLCompiler::emitConstant(const CellValue &val,
                                  std::optional<CellRef> designatedCell) {
  CellRef c = designatedCell ? *designatedCell : core_.arena().makeCell();
  core_.writeValue(c, 0, std::nullopt, val);
  return c;
}

CellRef VQLCompiler::emitInputConstant(CellRef op, const CellValue &val) {
  CellRef c = emitConstant(val);
  emitInput(op, c);
  return c;
}

CompilationResult VQLCompiler::compile(const QueryExpression &query,
                                       const CompilationOptions &options) {
  entryOp_.reset();
  currentOp_.reset();
  allOps_.clear();
  varBindings_.clear();
  lastResultCells_.clear();

  try {
    const auto resultCell = compileQuery(query);
    if (lastResultCells_.empty() && resultCell) {
      lastResultCells_.push_back(*resultCell);
    }
  } catch (const std::invalid_argument &unsupported) {
    return CompilationResult{
        .success          = false,
        .entryOpcode      = std::nullopt,
        .errorMessage     = unsupported.what(),
        .generatedOpcodes = {},
        .disassembly      = "",
    };
  }
  if (options.targetLibrary) {
    CellRef retOp = emitOp(OpcodeKind::Return, "#RETURN");
    for (CellRef r : lastResultCells_) {
      emitInput(retOp, r);
    }
    linkAsLibrary(entryOp_.value(), options.moduleName, options.symbolName);
  } else {
    CellRef haltOp = emitOp(OpcodeKind::Halt, "#HALT");
    for (CellRef r : lastResultCells_) {
      emitInput(haltOp, r);
    }
    linkAsExecutable(entryOp_.value());
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

DimRef VQLCompiler::resolveDimension(std::string_view name) {
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

CompilationResult VQLCompiler::compile(std::string_view queryString,
                                       const CompilationOptions &options) {
  try {
    Parser parser(queryString);
    QueryExpression ast = parser.parseQuery();
    return compile(ast, options);
  } catch (const ParseError &err) {
    return CompilationResult{
        .success          = false,
        .entryOpcode      = std::nullopt,
        .errorMessage     = std::string("Parse error: ") + err.what(),
        .generatedOpcodes = {},
        .disassembly      = "",
    };
  }
}

std::optional<CellRef> VQLCompiler::compileQuery(const QueryExpression &query) {
  if (std::holds_alternative<PathExpression>(query.expr)) {
    return compilePathExpression(std::get<PathExpression>(query.expr),
                                 core_.home());
  }
  return compileExecutionBlock(std::get<ExecutionBlock>(query.expr));
}

std::optional<CellRef>
VQLCompiler::compileExecutionBlock(const ExecutionBlock &block) {
  // 1. Process bindings (for, let)
  for (const auto &binding : block.bindings) {
    if (std::holds_alternative<LetClause>(binding)) {
      const auto &let = std::get<LetClause>(binding);
      std::optional<CellRef> valCell;
      if (std::holds_alternative<PathExpression>(let.target)) {
        valCell = compilePathExpression(std::get<PathExpression>(let.target),
                                        core_.home());
      } else if (std::holds_alternative<ValueExpr>(let.target)) {
        valCell =
            compileValueExpr(std::get<ValueExpr>(let.target), core_.home());
      }
      CellRef bindOp = emitOp(OpcodeKind::Bind, "#BIND " + let.varName);
      emitInputConstant(bindOp, let.varName);
      if (valCell) {
        emitInput(bindOp, *valCell);
      }
      varBindings_[let.varName] = valCell;
    } else if (std::holds_alternative<ForClause>(binding)) {
      const auto &forCl   = std::get<ForClause>(binding);
      const auto inStream = compilePathExpression(forCl.inPath, core_.home());
      CellRef bindOp = emitOp(OpcodeKind::Bind, "#FOR_BIND " + forCl.varName);
      emitInputConstant(bindOp, forCl.varName);
      if (inStream) {
        emitInput(bindOp, *inStream);
      }
      varBindings_[forCl.varName] = inStream;
    }
  }

  // 2. Process WhereClause if present
  if (block.where) {
    auto condCell = compileBooleanExpr(block.where->condition, core_.home());
    if (!condCell) condCell = emitConstant(false);
    CellRef branchOp = emitOp(OpcodeKind::Branch, "#WHERE_FILTER");
    emitInput(branchOp, *condCell);
  }

  // 3. Process ActionClause
  return std::visit(
      [this](const auto &act) -> std::optional<CellRef> {
        using T = std::decay_t<decltype(act)>;
        if constexpr (std::is_same_v<T, ReturnClause>) {
          std::optional<CellRef> lastRes;
          for (const auto &retItem : act.items) {
            if (std::holds_alternative<PathExpression>(retItem.item)) {
              lastRes = compilePathExpression(
                  std::get<PathExpression>(retItem.item), core_.home());
            } else if (std::holds_alternative<FieldWeave>(retItem.item)) {
              const auto &fw = std::get<FieldWeave>(retItem.item);
              std::optional<CellRef> resCell = core_.arena().makeCell();
              for (const auto &step : fw.steps) {
                if (!resCell) break;
                resCell = compilePathStep(step, *resCell);
              }
              lastRes = resCell;
            }
          }
          return lastRes;
        } else if constexpr (std::is_same_v<T, EffectClause>) {
          for (const auto &effItem : act.items) {
            std::visit(
                [this](const auto &sub) {
                  using SubT = std::decay_t<decltype(sub)>;
                  if constexpr (std::is_same_v<SubT, PathExpression>) {
                    compilePathExpression(sub, core_.home());
                  } else if constexpr (std::is_same_v<SubT, LetClause>) {
                    std::optional<CellRef> valCell;
                    if (std::holds_alternative<PathExpression>(sub.target)) {
                      valCell = compilePathExpression(
                          std::get<PathExpression>(sub.target), core_.home());
                    } else if (std::holds_alternative<ValueExpr>(sub.target)) {
                      valCell = compileValueExpr(
                          std::get<ValueExpr>(sub.target), core_.home());
                    }
                    CellRef bindOp =
                        emitOp(OpcodeKind::Bind, "#WEAVE_BIND " + sub.varName);
                    emitInputConstant(bindOp, sub.varName);
                    if (valCell) {
                      emitInput(bindOp, *valCell);
                    }
                  }
                },
                effItem.item);
          }
          return core_.home();
        } else if constexpr (std::is_same_v<T, ConditionalClause>) {
          auto condCell = compileBooleanExpr(act.condition, core_.home());
          if (!condCell) condCell = emitConstant(false);
          CellRef branchOp = emitOp(OpcodeKind::Branch, "#IF_BRANCH");
          emitInput(branchOp, *condCell);
          return condCell;
        } else {
          throw std::logic_error("unsupported VQL execution clause");
        }
      },
      block.action.clause);
}

std::optional<CellRef>
VQLCompiler::compilePathExpression(const PathExpression &path,
                                   CellRef ctxCell) {
  if (!path.anchorPredicates.empty()) {
    // Refused rather than compiled without them, which would answer every
    // anchor cell whatever the predicate said.
    throw std::invalid_argument(
        "predicates on an anchor are not compiled to Vortex yet; run the "
        "query with --engine direct");
  }
  const auto start = path.anchor.kind == AnchorKind::Context
                         ? std::optional<CellRef>{ctxCell}
                         : compileAnchor(path.anchor);
  if (!start) {
    lastResultCells_.clear();
    return std::nullopt;
  }
  std::vector<CellRef> stream{*start};

  for (const auto &step : path.steps) {
    std::vector<CellRef> nextStream;

    if (std::holds_alternative<SignedDimensionStep>(step.selector)) {
      const auto &sd = std::get<SignedDimensionStep>(step.selector);
      DimRef dim     = resolveDimension(sd.dimName);

      if (sd.creates.empty()) {
        for (CellRef current : stream) {
          CellRef linkOp =
              emitOp(OpcodeKind::Link, std::string("#TRAVERSE ") + sd.dimName);
          emitInput(linkOp, current);
          emitInputConstant(linkOp, static_cast<std::int64_t>(dim));
          emitInputConstant(linkOp,
                            static_cast<std::int64_t>(toSign(sd.direction)));

          CellRef nextCell = core_.arena().makeCell();
          emitOutput(linkOp, nextCell);
          nextStream.push_back(nextCell);
        }
      } else {
        for (CellRef current : stream) {
          std::vector<CellRef> createdCells;
          CellRef attachPoint = current;
          for (const auto &createVal : sd.creates) {
            CellRef newOp = emitOp(OpcodeKind::New, "#MINT_CELL");
            emitInput(newOp, attachPoint);
            emitInputConstant(newOp, static_cast<std::int64_t>(dim));
            emitInputConstant(newOp, static_cast<std::int64_t>(sd.direction));

            CellRef freshCell = core_.arena().makeCell();
            emitOutput(newOp, freshCell);

            if (createVal.kind == CreateValue::Kind::Literal) {
              CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
              emitInput(valOp, freshCell);
              emitInputConstant(valOp, static_cast<std::int64_t>(0));
              emitInputConstant(valOp, static_cast<std::int64_t>(-1));
              emitInputConstant(valOp, createVal.literal);
            } else if (createVal.kind == CreateValue::Kind::Expression &&
                       createVal.expr) {
              const auto valData = compileValueExpr(*createVal.expr, current);
              if (valData) {
                CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
                emitInput(valOp, freshCell);
                emitInputConstant(valOp, static_cast<std::int64_t>(0));
                emitInputConstant(valOp, static_cast<std::int64_t>(-1));
                emitInput(valOp, *valData);
              }
            }
            createdCells.push_back(freshCell);
            attachPoint = freshCell;
          }

          if (step.yieldMode == YieldMode::Both) {
            nextStream.push_back(current);
            nextStream.insert(nextStream.end(), createdCells.begin(),
                              createdCells.end());
          } else if (step.yieldMode == YieldMode::Keep) {
            nextStream.push_back(current);
          } else if (step.yieldMode == YieldMode::Last) {
            if (!createdCells.empty()) {
              nextStream.push_back(createdCells.back());
            }
          } else {
            nextStream.insert(nextStream.end(), createdCells.begin(),
                              createdCells.end());
          }
        }
      }
    } else if (std::holds_alternative<FunctionInvocation>(step.selector)) {
      const auto &fn = std::get<FunctionInvocation>(step.selector);
      if (fn.name == "value" && !fn.args.empty()) {
        for (CellRef current : stream) {
          const auto argVal = compileValueExpr(fn.args[0], current);
          if (!argVal) continue;
          CellRef valOp = emitOp(OpcodeKind::Value, "#VALUE");
          emitInput(valOp, current);
          emitInputConstant(valOp, static_cast<std::int64_t>(0));
          emitInputConstant(valOp, static_cast<std::int64_t>(-1));
          emitInput(valOp, *argVal);
          nextStream.push_back(current);
        }
      } else if (fn.name == "link" && fn.args.size() >= 2) {
        // Existing-Target Fan-Out (§4.7)
        std::string dimName;
        zigzag::DimVector direction = zigzag::DimVector::POS;
        if (std::holds_alternative<ScalarLiteral>(fn.args[0].kind)) {
          const auto &lit = std::get<ScalarLiteral>(fn.args[0].kind);
          if (std::holds_alternative<std::string>(lit.value)) {
            std::string str = std::get<std::string>(lit.value);
            if (!str.empty() && str[0] == '-') {
              direction = zigzag::DimVector::NEG;
              dimName   = str.substr(1);
            } else if (!str.empty() && str[0] == '+') {
              direction = zigzag::DimVector::POS;
              dimName   = str.substr(1);
            } else {
              dimName = str;
            }
          }
        } else if (std::holds_alternative<std::shared_ptr<PathExpression>>(
                       fn.args[0].kind)) {
          const auto &p =
              *std::get<std::shared_ptr<PathExpression>>(fn.args[0].kind);
          if (!p.steps.empty() && std::holds_alternative<SignedDimensionStep>(
                                      p.steps[0].selector)) {
            const auto &sd = std::get<SignedDimensionStep>(p.steps[0].selector);
            dimName        = sd.dimName;
            direction      = sd.direction;
          }
        }

        DimRef dim            = resolveDimension(dimName);
        const auto targetCell = compileValueExpr(fn.args[1], core_.home());

        std::vector<CellRef> attachedClones;
        if (targetCell) {
          for (CellRef current : stream) {
            CellRef cloneOp = emitOp(OpcodeKind::Clone, "#CLONE_GENERATOR");
            emitInput(cloneOp, *targetCell);
            CellRef cloneCell = core_.arena().makeCell();
            emitOutput(cloneOp, cloneCell);

            CellRef linkOp =
                emitOp(OpcodeKind::Link, "#LINK_ATTACH " + dimName);
            emitInput(linkOp, current);
            emitInputConstant(linkOp, static_cast<std::int64_t>(dim));
            emitInputConstant(linkOp,
                              static_cast<std::int64_t>(toSign(direction)));
            emitInput(linkOp, cloneCell);
            attachedClones.push_back(cloneCell);
          }
        }

        if (step.yieldMode == YieldMode::Both) {
          nextStream = stream;
          nextStream.insert(nextStream.end(), attachedClones.begin(),
                            attachedClones.end());
        } else if (step.yieldMode == YieldMode::Keep) {
          nextStream = stream;
        } else if (step.yieldMode == YieldMode::Last) {
          if (!attachedClones.empty()) {
            nextStream.push_back(attachedClones.back());
          }
        } else {
          nextStream = attachedClones;
        }
      }
    }

    // Deref master
    if (step.derefMaster) {
      for (auto &c : nextStream) {
        CellRef derefOp = emitOp(OpcodeKind::Link, "#DEREF_CLONE_MASTER");
        emitInput(derefOp, c);
        emitInputConstant(derefOp,
                          static_cast<std::int64_t>(core_.dims().clone));
        emitInputConstant(derefOp, static_cast<std::int64_t>(-1));
        CellRef masterCell = core_.arena().makeCell();
        emitOutput(derefOp, masterCell);
        c = masterCell;
      }
    }

    // Predicates
    for (const auto &pred : step.predicates) {
      for (CellRef c : nextStream) {
        auto condCell = compileBooleanExpr(pred, c);
        if (!condCell) condCell = emitConstant(false);
        CellRef branchOp = emitOp(OpcodeKind::Branch, "#FILTER_BRANCH");
        emitInput(branchOp, *condCell);
      }
    }

    // Range Clamp [start, end]
    if (step.rangeClamp.has_value() && !nextStream.empty()) {
      // step is a const-bound reference with no reassignment since the
      // has_value() check just above.
      const auto &clamp =
          *step.rangeClamp; // NOLINT(bugprone-unchecked-optional-access)
      auto total     = static_cast<std::int64_t>(nextStream.size());
      std::int64_t s = clamp.start;
      std::int64_t e = clamp.end.value_or(s);

      std::int64_t from = (s < 0) ? (total + s) : (s - 1);
      std::int64_t to   = (e < 0) ? (total + e) : (e - 1);

      from = std::clamp<std::int64_t>(from, 0, total - 1);
      to   = std::clamp<std::int64_t>(to, 0, total - 1);

      if (from <= to) {
        nextStream = std::vector<CellRef>(nextStream.begin() + from,
                                          nextStream.begin() + to + 1);
      } else {
        nextStream.clear();
      }
    }

    stream = std::move(nextStream);
  }

  if (path.cloneTail && !stream.empty()) {
    for (CellRef master : stream) {
      for (const auto &op : path.cloneTail->operands) {
        std::optional<CellRef> opCell;
        if (op.path) {
          opCell = compilePathExpression(*op.path, core_.home());
        } else if (op.bareCreate) {
          CellRef newOp = emitOp(OpcodeKind::New, "#CLONE_TAIL_NEW");
          emitInput(newOp, master);
          emitInputConstant(newOp,
                            static_cast<std::int64_t>(core_.dims().clone));
          emitInputConstant(newOp, static_cast<std::int64_t>(1));
          opCell = core_.arena().makeCell();
          emitOutput(newOp, *opCell);
          if (op.bareCreate->kind == CreateValue::Kind::Literal) {
            CellRef valOp = emitOp(OpcodeKind::Value, "#CLONE_TAIL_INIT");
            emitInput(valOp, *opCell);
            emitInputConstant(valOp, static_cast<std::int64_t>(0));
            emitInputConstant(valOp, static_cast<std::int64_t>(-1));
            emitInputConstant(valOp, op.bareCreate->literal);
          } else if (op.bareCreate->kind == CreateValue::Kind::Expression &&
                     op.bareCreate->expr) {
            const auto valData =
                compileValueExpr(*op.bareCreate->expr, core_.home());
            if (valData) {
              CellRef valOp = emitOp(OpcodeKind::Value, "#CLONE_TAIL_INIT");
              emitInput(valOp, *opCell);
              emitInputConstant(valOp, static_cast<std::int64_t>(0));
              emitInputConstant(valOp, static_cast<std::int64_t>(-1));
              emitInput(valOp, *valData);
            }
          }
        }

        if (opCell) {
          CellRef linkOp = emitOp(OpcodeKind::Link, "#CLONE_JOIN");
          emitInput(linkOp, master);
          emitInputConstant(linkOp,
                            static_cast<std::int64_t>(core_.dims().clone));
          emitInputConstant(linkOp, static_cast<std::int64_t>(1));
          emitInput(linkOp, *opCell);
        }
      }
    }
  }

  lastResultCells_ = stream;
  if (stream.empty()) return std::nullopt;
  return stream.front();
}

std::optional<CellRef> VQLCompiler::compileAnchor(const AnchorNode &anchor) {
  CellRef res{};
  switch (anchor.kind) {
  case AnchorKind::Home:
    res = core_.home();
    break;
  case AnchorKind::NamedStore: {
    CellRef scanOp =
        emitOp(OpcodeKind::Resolve, "#RESOLVE_STORE " + anchor.name);
    emitInputConstant(scanOp, anchor.name);
    res = core_.arena().makeCell();
    emitOutput(scanOp, res);
    break;
  }
  case AnchorKind::Root:
  case AnchorKind::Cursor:
  case AnchorKind::NamedCursor:
  case AnchorKind::AllCursors:
  case AnchorKind::AllNamedCursors:
    res = core_.home();
    break;
  case AnchorKind::Variable: {
    auto it = varBindings_.find(anchor.name);
    if (it != varBindings_.end()) {
      if (!it->second) return std::nullopt;
      res = *it->second;
    } else {
      CellRef resOp = emitOp(OpcodeKind::Resolve, "#RESOLVE " + anchor.name);
      emitInputConstant(resOp, anchor.name);
      res = core_.arena().makeCell();
      emitOutput(resOp, res);
    }
    break;
  }
  case AnchorKind::LiteralCellId:
    if (anchor.cellId > std::numeric_limits<CellRef>::max()) {
      return std::nullopt;
    }
    res = static_cast<CellRef>(anchor.cellId);
    if (!core_.arena().contains(res)) return std::nullopt;
    break;
  case AnchorKind::Context:
    res = core_.home();
    break;
  case AnchorKind::Create: {
    res = core_.arena().makeCell();
    if (anchor.createValue.has_value()) {
      const auto &cv = *anchor.createValue;
      if (cv.kind == CreateValue::Kind::Literal) {
        CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
        emitInput(valOp, res);
        emitInputConstant(valOp, static_cast<std::int64_t>(0));
        emitInputConstant(valOp, static_cast<std::int64_t>(-1));
        emitInputConstant(valOp, cv.literal);
      } else if (cv.kind == CreateValue::Kind::Expression && cv.expr) {
        const auto valData = compileValueExpr(*cv.expr, core_.home());
        if (valData) {
          CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
          emitInput(valOp, res);
          emitInputConstant(valOp, static_cast<std::int64_t>(0));
          emitInputConstant(valOp, static_cast<std::int64_t>(-1));
          emitInput(valOp, *valData);
        }
      }
    }
    break;
  }
  case AnchorKind::StoreGenerator:
  case AnchorKind::StoreAnchor:
  case AnchorKind::Microversion:
    res = core_.home();
    break;
  }

  if (anchor.derefMaster) {
    CellRef derefOp = emitOp(OpcodeKind::Link, "#DEREF_MASTER");
    emitInput(derefOp, res);
    emitInputConstant(derefOp, static_cast<std::int64_t>(core_.dims().clone));
    emitInputConstant(derefOp, static_cast<std::int64_t>(-1));
    CellRef master = core_.arena().makeCell();
    emitOutput(derefOp, master);
    res = master;
  }
  return res;
}

std::optional<CellRef> VQLCompiler::compilePathStep(const PathStep &step,
                                                    CellRef inStreamCell) {
  PathExpression dummy;
  dummy.anchor.kind = AnchorKind::Context;
  dummy.steps.push_back(step);
  return compilePathExpression(dummy, inStreamCell);
}

std::optional<CellRef> VQLCompiler::compileBooleanExpr(const BooleanExpr &expr,
                                                       CellRef ctxCell) {
  if (expr.terms.empty()) {
    return emitConstant(true);
  }

  std::optional<CellRef> resultCell;
  for (const auto &term : expr.terms) {
    std::optional<CellRef> termCell;
    if (term.factors.empty()) termCell = emitConstant(true);
    for (const auto &factor : term.factors) {
      std::optional<CellRef> factCell;
      if (std::holds_alternative<ComparisonExpr>(factor.test)) {
        factCell =
            compileComparison(std::get<ComparisonExpr>(factor.test), ctxCell);
      } else if (std::holds_alternative<PredicateTest>(factor.test)) {
        const auto &pt = std::get<PredicateTest>(factor.test);
        factCell       = std::visit(
            [this, ctxCell](const auto &t) -> std::optional<CellRef> {
              using T = std::decay_t<decltype(t)>;
              if constexpr (std::is_same_v<T,
                                                 std::shared_ptr<PathExpression>>) {
                if (t) {
                  const auto pathCell = compilePathExpression(*t, ctxCell);
                  return pathCell ? pathCell
                                        : std::optional<CellRef>{emitConstant(false)};
                }
                return emitConstant(false);
              } else if constexpr (std::is_same_v<T, bool>) {
                return ctxCell;
              } else if constexpr (std::is_same_v<T,
                                                        std::shared_ptr<ValueExpr>>) {
                if (t) {
                  const auto valueCell = compileValueExpr(*t, ctxCell);
                  return valueCell
                                   ? valueCell
                                   : std::optional<CellRef>{emitConstant(false)};
                }
                return emitConstant(false);
              } else if constexpr (std::is_same_v<T, std::shared_ptr<
                                                               FunctionInvocation>>) {
                if (t) {
                  CellRef callOp = emitOp(OpcodeKind::Call, "#CALL " + t->name);
                  CellRef outC   = core_.arena().makeCell();
                  emitOutput(callOp, outC);
                  return outC;
                }
                return emitConstant(false);
              } else {
                return emitConstant(true);
              }
            },
            pt.kind);
      }

      if (!factCell) continue;
      if (factor.negated) {
        CellRef notOp = emitOp(OpcodeKind::Not, "#NOT");
        emitInput(notOp, *factCell);
        CellRef notOut = core_.arena().makeCell();
        emitOutput(notOp, notOut);
        factCell = notOut;
      }

      if (!termCell) {
        termCell = factCell;
      } else {
        CellRef andOp = emitOp(OpcodeKind::And, "#AND");
        emitInput(andOp, *termCell);
        emitInput(andOp, *factCell);
        CellRef andOut = core_.arena().makeCell();
        emitOutput(andOp, andOut);
        termCell = andOut;
      }
    }

    if (!termCell) termCell = emitConstant(false);
    if (!resultCell) {
      resultCell = *termCell;
    } else {
      CellRef orOp = emitOp(OpcodeKind::Or, "#OR");
      emitInput(orOp, *resultCell);
      emitInput(orOp, *termCell);
      CellRef orOut = core_.arena().makeCell();
      emitOutput(orOp, orOut);
      resultCell = orOut;
    }
  }

  if (!resultCell) return emitConstant(false);
  return resultCell;
}

std::optional<CellRef>
VQLCompiler::compileComparison(const ComparisonExpr &comp, CellRef ctxCell) {
  const auto lhs = comp.left ? compileValueExpr(*comp.left, ctxCell)
                             : std::optional<CellRef>{core_.home()};
  const auto rhs = comp.right ? compileValueExpr(*comp.right, ctxCell)
                              : std::optional<CellRef>{core_.home()};
  if (!lhs || !rhs) return std::nullopt;

  OpcodeKind kind = OpcodeKind::Eq;
  switch (comp.op) {
  case CompOp::Equal:
    kind = OpcodeKind::Eq;
    break;
  case CompOp::NotEqual:
    kind = OpcodeKind::Neq;
    break;
  case CompOp::LessThan:
    kind = OpcodeKind::Lt;
    break;
  case CompOp::LessEqual:
    kind = OpcodeKind::Lte;
    break;
  case CompOp::GreaterThan:
    kind = OpcodeKind::Gt;
    break;
  case CompOp::GreaterEqual:
    kind = OpcodeKind::Gte;
    break;
  }

  CellRef op =
      emitOp(kind, std::string("#COMP ") + std::string(opcodeMnemonic(kind)));
  emitInput(op, *lhs);
  emitInput(op, *rhs);

  CellRef outCell = core_.arena().makeCell();
  emitOutput(op, outCell);
  return outCell;
}

std::optional<CellRef> VQLCompiler::compileValueExpr(const ValueExpr &expr,
                                                     CellRef ctxCell) {
  return std::visit(
      [this, ctxCell](const auto &val) -> std::optional<CellRef> {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, ScalarLiteral>) {
          return std::visit(
              [this](const auto &lit) -> CellRef {
                return emitConstant(CellValue(lit));
              },
              val.value);
        } else if constexpr (std::is_same_v<T, std::string>) {
          // VariableRef
          auto it = varBindings_.find(val);
          if (it != varBindings_.end()) {
            return it->second;
          }
          CellRef resOp = emitOp(OpcodeKind::Resolve, "#RESOLVE " + val);
          emitInputConstant(resOp, val);
          CellRef outCell = core_.arena().makeCell();
          emitOutput(resOp, outCell);
          return outCell;
        } else if constexpr (std::is_same_v<T,
                                            std::shared_ptr<PathExpression>>) {
          if (val) {
            return compilePathExpression(*val, ctxCell);
          }
          return core_.home();
        } else if constexpr (std::is_same_v<
                                 T, std::shared_ptr<FunctionInvocation>>) {
          if (val) {
            CellRef callOp  = emitOp(OpcodeKind::Call, "#CALL " + val->name);
            CellRef outCell = core_.arena().makeCell();
            emitOutput(callOp, outCell);
            return outCell;
          }
          return core_.home();
        } else if constexpr (std::is_same_v<T, std::shared_ptr<BooleanExpr>>) {
          if (val) {
            return compileBooleanExpr(*val, ctxCell);
          }
          return core_.home();
        } else {
          return core_.home();
        }
      },
      expr.kind);
}

void VQLCompiler::linkAsExecutable(CellRef entryOp) {
  // Link posward off home along +d.spin
  zigzag::expectWritten(
      core_.arena().link(core_.home(), core_.dims().spin, false, entryOp));
  // Spawn main execution cursor
  vm_.spawnCursor(entryOp, "main");
}

void VQLCompiler::linkAsLibrary(CellRef entryOp, std::string_view moduleName,
                                std::string_view symbolName) {
  core_.exportSymbol(core_.getOrCreateModule(moduleName), symbolName, entryOp);
}

std::string VQLCompiler::disassemble(std::optional<CellRef> entryOp,
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

xanadu::MicroversionId
VQLCompiler::exportToStore(xanadu::Store &store,
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

} // namespace xanadu::vql
