/**
 * @file compiler.cpp
 * @brief Implementation of VQL Compiler translating AST to Vortex bytecode.
 */
#include "common/xanadu/vql/compiler.hpp"

#include <cstdio>
#include <iomanip>
#include <sstream>
#include <unordered_set>

#include "common/xanadu/vql/lexer.hpp"
#include "common/xanadu/vql/parser.hpp"

namespace xanadu::vql {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::noCell;
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
  default:
    return "UNKNOWN";
  }
}

VQLCompiler::VQLCompiler(zigzag::vortex::VortexCore &core,
                         zigzag::vortex::VortexVM &vm)
    : core_(core), vm_(vm) {}

CellRef VQLCompiler::emitOp(OpcodeKind kind, std::string_view label) {
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

void VQLCompiler::emitInput(CellRef op, CellRef operand) {
  core_.bindInput(op, operand);
}

void VQLCompiler::emitOutput(CellRef op, CellRef target) {
  core_.bindOutput(op, target);
}

CellRef VQLCompiler::emitConstant(const CellValue &val,
                                  std::optional<CellRef> designatedCell) {
  CellRef c = designatedCell ? *designatedCell : core_.arena().makeCell();
  core_.value(c, 0, -1, val);
  return c;
}

CellRef VQLCompiler::emitInputConstant(CellRef op, const CellValue &val) {
  CellRef c = emitConstant(val);
  emitInput(op, c);
  return c;
}

CompilationResult VQLCompiler::compile(const QueryExpression &query,
                                       const CompilationOptions &options) {
  entryOp_   = noCell;
  currentOp_ = noCell;
  allOps_.clear();
  varBindings_.clear();
  lastResultCells_.clear();

  CellRef resultCell = compileQuery(query);
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

  // Walk dims.dims to see if dimension was previously minted
  CellRef curr = dims.dims;
  std::unordered_set<CellRef> visited;
  while (curr != noCell && visited.insert(curr).second) {
    if (core_.arena().textOf(curr) == name) {
      return curr;
    }
    curr = core_.arena().linked(curr, dims.dims, false);
  }

  return core_.mintDimension(name);
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
        .entryOpcode      = noCell,
        .errorMessage     = std::string("Parse error: ") + err.what(),
        .generatedOpcodes = {},
        .disassembly      = "",
    };
  }
}

CellRef VQLCompiler::compileQuery(const QueryExpression &query) {
  if (std::holds_alternative<PathExpression>(query.expr)) {
    return compilePathExpression(std::get<PathExpression>(query.expr),
                                 core_.home());
  }
  return compileExecutionBlock(std::get<ExecutionBlock>(query.expr));
}

CellRef VQLCompiler::compileExecutionBlock(const ExecutionBlock &block) {
  // 1. Process bindings (for, let)
  for (const auto &binding : block.bindings) {
    if (std::holds_alternative<LetClause>(binding)) {
      const auto &let = std::get<LetClause>(binding);
      CellRef valCell = noCell;
      if (std::holds_alternative<PathExpression>(let.target)) {
        valCell = compilePathExpression(std::get<PathExpression>(let.target),
                                        core_.home());
      } else if (std::holds_alternative<ValueExpr>(let.target)) {
        valCell =
            compileValueExpr(std::get<ValueExpr>(let.target), core_.home());
      }
      CellRef bindOp = emitOp(OpcodeKind::Bind, "#BIND " + let.varName);
      emitInputConstant(bindOp, let.varName);
      if (valCell != noCell) {
        emitInput(bindOp, valCell);
      }
      varBindings_[let.varName] = valCell;
    } else if (std::holds_alternative<ForClause>(binding)) {
      const auto &forCl = std::get<ForClause>(binding);
      CellRef inStream  = compilePathExpression(forCl.inPath, core_.home());
      CellRef bindOp = emitOp(OpcodeKind::Bind, "#FOR_BIND " + forCl.varName);
      emitInputConstant(bindOp, forCl.varName);
      if (inStream != noCell) {
        emitInput(bindOp, inStream);
      }
      varBindings_[forCl.varName] = inStream;
    }
  }

  // 2. Process WhereClause if present
  if (block.where) {
    CellRef condCell = compileBooleanExpr(block.where->condition, core_.home());
    CellRef branchOp = emitOp(OpcodeKind::Branch, "#WHERE_FILTER");
    emitInput(branchOp, condCell);
  }

  // 3. Process ActionClause
  return std::visit(
      [this](const auto &act) -> CellRef {
        using T = std::decay_t<decltype(act)>;
        if constexpr (std::is_same_v<T, ReturnClause>) {
          CellRef lastRes = noCell;
          for (const auto &retItem : act.items) {
            if (std::holds_alternative<PathExpression>(retItem.item)) {
              lastRes = compilePathExpression(
                  std::get<PathExpression>(retItem.item), core_.home());
            } else if (std::holds_alternative<FieldWeave>(retItem.item)) {
              const auto &fw  = std::get<FieldWeave>(retItem.item);
              CellRef resCell = core_.arena().makeCell();
              for (const auto &step : fw.steps) {
                resCell = compilePathStep(step, resCell);
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
                    CellRef valCell = noCell;
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
                    if (valCell != noCell) {
                      emitInput(bindOp, valCell);
                    }
                  }
                },
                effItem.item);
          }
          return core_.home();
        } else if constexpr (std::is_same_v<T, ConditionalClause>) {
          CellRef condCell = compileBooleanExpr(act.condition, core_.home());
          CellRef branchOp = emitOp(OpcodeKind::Branch, "#IF_BRANCH");
          emitInput(branchOp, condCell);
          return condCell;
        } else {
          return noCell;
        }
      },
      block.action.clause);
}

CellRef VQLCompiler::compilePathExpression(const PathExpression &path,
                                           CellRef ctxCell) {
  CellRef start = compileAnchor(path.anchor);
  if (start == noCell) {
    start = ctxCell != noCell ? ctxCell : core_.home();
  }

  std::vector<CellRef> stream = {start};

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
          emitInputConstant(linkOp, static_cast<std::int64_t>(sd.direction));

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
              CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
              emitInput(valOp, freshCell);
              emitInputConstant(valOp, static_cast<std::int64_t>(0));
              emitInputConstant(valOp, static_cast<std::int64_t>(-1));
              CellRef valData = compileValueExpr(*createVal.expr, current);
              emitInput(valOp, valData);
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
          CellRef valOp = emitOp(OpcodeKind::Value, "#VALUE");
          emitInput(valOp, current);
          emitInputConstant(valOp, static_cast<std::int64_t>(0));
          emitInputConstant(valOp, static_cast<std::int64_t>(-1));
          CellRef argVal = compileValueExpr(fn.args[0], current);
          emitInput(valOp, argVal);
          nextStream.push_back(current);
        }
      } else if (fn.name == "link" && fn.args.size() >= 2) {
        // Existing-Target Fan-Out (§4.7)
        std::string dimName;
        int direction = 1;
        if (std::holds_alternative<ScalarLiteral>(fn.args[0].kind)) {
          const auto &lit = std::get<ScalarLiteral>(fn.args[0].kind);
          if (std::holds_alternative<std::string>(lit.value)) {
            std::string str = std::get<std::string>(lit.value);
            if (!str.empty() && str[0] == '-') {
              direction = -1;
              dimName   = str.substr(1);
            } else if (!str.empty() && str[0] == '+') {
              direction = 1;
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

        DimRef dim         = resolveDimension(dimName);
        CellRef targetCell = compileValueExpr(fn.args[1], core_.home());

        std::vector<CellRef> attachedClones;
        for (CellRef current : stream) {
          CellRef cloneOp = emitOp(OpcodeKind::Clone, "#CLONE_GENERATOR");
          emitInput(cloneOp, targetCell);
          CellRef cloneCell = core_.arena().makeCell();
          emitOutput(cloneOp, cloneCell);

          CellRef linkOp = emitOp(OpcodeKind::Link, "#LINK_ATTACH " + dimName);
          emitInput(linkOp, current);
          emitInputConstant(linkOp, static_cast<std::int64_t>(dim));
          emitInputConstant(linkOp, static_cast<std::int64_t>(direction));
          emitInput(linkOp, cloneCell);
          attachedClones.push_back(cloneCell);
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
        CellRef condCell = compileBooleanExpr(pred, c);
        CellRef branchOp = emitOp(OpcodeKind::Branch, "#FILTER_BRANCH");
        emitInput(branchOp, condCell);
      }
    }

    // Range Clamp [start, end]
    if (step.rangeClamp.has_value() && !nextStream.empty()) {
      const auto &clamp  = *step.rangeClamp;
      std::int64_t total = static_cast<std::int64_t>(nextStream.size());
      std::int64_t s     = clamp.start;
      std::int64_t e     = clamp.end.value_or(s);

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
        CellRef opCell = noCell;
        if (op.path) {
          opCell = compilePathExpression(*op.path, core_.home());
        } else if (op.bareCreate) {
          CellRef newOp = emitOp(OpcodeKind::New, "#CLONE_TAIL_NEW");
          emitInput(newOp, master);
          emitInputConstant(newOp,
                            static_cast<std::int64_t>(core_.dims().clone));
          emitInputConstant(newOp, static_cast<std::int64_t>(1));
          opCell = core_.arena().makeCell();
          emitOutput(newOp, opCell);
          if (op.bareCreate->kind == CreateValue::Kind::Literal) {
            CellRef valOp = emitOp(OpcodeKind::Value, "#CLONE_TAIL_INIT");
            emitInput(valOp, opCell);
            emitInputConstant(valOp, static_cast<std::int64_t>(0));
            emitInputConstant(valOp, static_cast<std::int64_t>(-1));
            emitInputConstant(valOp, op.bareCreate->literal);
          } else if (op.bareCreate->kind == CreateValue::Kind::Expression &&
                     op.bareCreate->expr) {
            CellRef valOp = emitOp(OpcodeKind::Value, "#CLONE_TAIL_INIT");
            emitInput(valOp, opCell);
            emitInputConstant(valOp, static_cast<std::int64_t>(0));
            emitInputConstant(valOp, static_cast<std::int64_t>(-1));
            CellRef valData =
                compileValueExpr(*op.bareCreate->expr, core_.home());
            emitInput(valOp, valData);
          }
        }

        if (opCell != noCell) {
          CellRef linkOp = emitOp(OpcodeKind::Link, "#CLONE_JOIN");
          emitInput(linkOp, master);
          emitInputConstant(linkOp,
                            static_cast<std::int64_t>(core_.dims().clone));
          emitInputConstant(linkOp, static_cast<std::int64_t>(1));
          emitInput(linkOp, opCell);
        }
      }
    }
  }

  lastResultCells_ = stream;
  return stream.empty() ? noCell : stream.front();
}

CellRef VQLCompiler::compileAnchor(const AnchorNode &anchor) {
  CellRef res = noCell;
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
    res = core_.home();
    break;
  case AnchorKind::Variable: {
    auto it = varBindings_.find(anchor.name);
    if (it != varBindings_.end() && it->second != noCell) {
      res = it->second;
    } else {
      CellRef resOp = emitOp(OpcodeKind::Resolve, "#RESOLVE " + anchor.name);
      emitInputConstant(resOp, anchor.name);
      res = core_.arena().makeCell();
      emitOutput(resOp, res);
    }
    break;
  }
  case AnchorKind::LiteralCellId:
    res = static_cast<CellRef>(anchor.cellId);
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
        CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
        emitInput(valOp, res);
        emitInputConstant(valOp, static_cast<std::int64_t>(0));
        emitInputConstant(valOp, static_cast<std::int64_t>(-1));
        CellRef valData = compileValueExpr(*cv.expr, core_.home());
        emitInput(valOp, valData);
      }
    }
    break;
  }
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

CellRef VQLCompiler::compilePathStep(const PathStep &step,
                                     CellRef inStreamCell) {
  PathExpression dummy{.steps = {step}};
  return compilePathExpression(dummy, inStreamCell);
}

CellRef VQLCompiler::compileBooleanExpr(const BooleanExpr &expr,
                                        CellRef ctxCell) {
  if (expr.terms.empty()) {
    return emitConstant(true);
  }

  CellRef resultCell = noCell;
  for (const auto &term : expr.terms) {
    CellRef termCell = noCell;
    for (const auto &factor : term.factors) {
      CellRef factCell = noCell;
      if (std::holds_alternative<ComparisonExpr>(factor.test)) {
        factCell =
            compileComparison(std::get<ComparisonExpr>(factor.test), ctxCell);
      } else if (std::holds_alternative<PredicateTest>(factor.test)) {
        const auto &pt = std::get<PredicateTest>(factor.test);
        factCell       = std::visit(
            [this, ctxCell](const auto &t) -> CellRef {
              using T = std::decay_t<decltype(t)>;
              if constexpr (std::is_same_v<T,
                                           std::shared_ptr<PathExpression>>) {
                if (t) return compilePathExpression(*t, ctxCell);
                return emitConstant(false);
              } else if constexpr (std::is_same_v<T, bool>) {
                return ctxCell;
              } else if constexpr (std::is_same_v<T,
                                                  std::shared_ptr<ValueExpr>>) {
                if (t) return compileValueExpr(*t, ctxCell);
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

      if (factor.negated) {
        CellRef notOp = emitOp(OpcodeKind::Not, "#NOT");
        emitInput(notOp, factCell);
        CellRef notOut = core_.arena().makeCell();
        emitOutput(notOp, notOut);
        factCell = notOut;
      }

      if (termCell == noCell) {
        termCell = factCell;
      } else {
        CellRef andOp = emitOp(OpcodeKind::And, "#AND");
        emitInput(andOp, termCell);
        emitInput(andOp, factCell);
        CellRef andOut = core_.arena().makeCell();
        emitOutput(andOp, andOut);
        termCell = andOut;
      }
    }

    if (resultCell == noCell) {
      resultCell = termCell;
    } else {
      CellRef orOp = emitOp(OpcodeKind::Or, "#OR");
      emitInput(orOp, resultCell);
      emitInput(orOp, termCell);
      CellRef orOut = core_.arena().makeCell();
      emitOutput(orOp, orOut);
      resultCell = orOut;
    }
  }

  return resultCell;
}

CellRef VQLCompiler::compileComparison(const ComparisonExpr &comp,
                                       CellRef ctxCell) {
  CellRef lhs =
      comp.left ? compileValueExpr(*comp.left, ctxCell) : core_.home();
  CellRef rhs =
      comp.right ? compileValueExpr(*comp.right, ctxCell) : core_.home();

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
  emitInput(op, lhs);
  emitInput(op, rhs);

  CellRef outCell = core_.arena().makeCell();
  emitOutput(op, outCell);
  return outCell;
}

CellRef VQLCompiler::compileValueExpr(const ValueExpr &expr, CellRef ctxCell) {
  return std::visit(
      [this, ctxCell](const auto &val) -> CellRef {
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
          if (it != varBindings_.end() && it->second != noCell) {
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
        } else {
          return core_.home();
        }
      },
      expr.kind);
}

void VQLCompiler::linkAsExecutable(CellRef entryOp) {
  if (entryOp == noCell) return;
  // Link posward off home along +d.spin
  core_.arena().link(core_.home(), core_.dims().spin, false, entryOp);
  // Spawn main execution cursor
  vm_.spawnCursor(entryOp, "main");
}

void VQLCompiler::linkAsLibrary(CellRef entryOp, std::string_view moduleName,
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

std::string VQLCompiler::disassemble(CellRef entryOp,
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

xanadu::MicroversionId
VQLCompiler::exportToStore(xanadu::Store &store,
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
      CellRef target = arena.linked(c, dimRef, false /*posward*/);
      if (target != zigzag::noCell && cellMap.count(target)) {
        CellRef to = cellMap.at(target);
        ver        = store.setLink(ver, from, mappedDim, false, to, &manifold);
        static_cast<void>(manifold.advance(store, ver));
      }
    }
  }

  return ver;
}

} // namespace xanadu::vql
