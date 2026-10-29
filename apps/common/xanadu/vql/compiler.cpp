/**
 * @file compiler.cpp
 * @brief Implementation of VQL Compiler translating AST to Vortex bytecode.
 */
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vortex/assembly_transaction.hpp"
#include "common/xanadu/vql/invocation.hpp"

#include <cstdio>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "common/xanadu/vql/contract_codec.hpp"
#include "common/xanadu/vql/contracts.hpp"
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
  case OpcodeKind::MatchContract:
    return "MATCH_CONTRACT";
  case OpcodeKind::RequireContract:
    return "REQUIRE_CONTRACT";
  case OpcodeKind::CopyCellRefs:
    return "COPY_CELL_REFS";
  case OpcodeKind::ClearCellRefs:
    return "CLEAR_CELL_REFS";
  case OpcodeKind::BranchFalseCell:
    return "BRANCH_FALSE_CELL";
  case OpcodeKind::StreamClear:
    return "STREAM_CLEAR";
  case OpcodeKind::StreamAppend:
    return "STREAM_APPEND";
  case OpcodeKind::StreamBegin:
    return "STREAM_BEGIN";
  case OpcodeKind::StreamReverseBegin:
    return "STREAM_REVERSE_BEGIN";
  case OpcodeKind::RankBegin:
    return "RANK_BEGIN";
  case OpcodeKind::IteratorNext:
    return "ITERATOR_NEXT";
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

VQLCompiler::VQLCompiler(MultiStoreCoordinator &coordinator,
                         zigzag::vortex::VortexVM &vm)
    : core_(coordinator.core()), vm_(vm), coordinator_(coordinator) {
  vm_.setContractCoordinator(coordinator);
}

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
  zigzag::vortex::AssemblyTransaction transaction(vm_);
  entryOp_.reset();
  currentOp_.reset();
  allOps_.clear();
  varBindings_.clear();
  streamBindings_ = options.initialBindings;
  for (const auto &[name, cells] : streamBindings_)
    varBindings_[name] =
        cells.empty() ? std::nullopt : std::optional{cells.front()};
  definitions_.clear();
  activeCalls_.clear();
  contextInputs_  = options.contextCells;
  ambientContext_ = std::nullopt;
  functorBindings_.clear();
  lastResultCells_.clear();
  dynamicStreams_.clear();

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
  try {
    for (const auto &definition : query.defs)
      validateFunctionSignature(definition);
  } catch (const TypeContractError &error) {
    throw std::invalid_argument(error.what());
  }
  for (const auto &definition : query.defs) {
    if (!definitions_.emplace(definition.name, definition).second)
      throw std::invalid_argument("duplicate function definition: " +
                                  definition.name);
  }
  if (!query.mounts.empty() || !query.namespaceDeclarations.empty()) {
    throw std::invalid_argument(
        "VQL mount declarations require Vortex graph lowering");
  }
  if (!query.body.statements.empty()) {
    std::optional<CellRef> result;
    for (const auto &statement : query.body.statements) {
      lastResultCells_.clear();
      if (const auto *path = std::get_if<PathExpression>(&statement)) {
        result = compilePathExpression(*path, core_.home());
      } else if (const auto *block = std::get_if<ExecutionBlock>(&statement)) {
        result = compileExecutionBlock(*block);
      } else if (const auto *weave = std::get_if<WeaveStatement>(&statement)) {
        result = compileExecutionBlock(
            ExecutionBlock{.action = ActionClause{.clause = weave->effect}});
      } else {
        throw std::invalid_argument(
            "watch and perspective statements require Vortex graph lowering");
      }
    }
    return result;
  }
  if (std::holds_alternative<PathExpression>(query.expr)) {
    return compilePathExpression(std::get<PathExpression>(query.expr),
                                 core_.home());
  }
  return compileExecutionBlock(std::get<ExecutionBlock>(query.expr));
}

std::optional<CellRef>
VQLCompiler::compileExecutionBlock(const ExecutionBlock &block) {
  if (block.where)
    throw std::invalid_argument(
        "where clauses require Vortex filtering lowering");
  // 1. Process bindings (for, let)
  for (const auto &binding : block.bindings) {
    if (std::holds_alternative<LetClause>(binding)) {
      const auto &let = std::get<LetClause>(binding);
      std::optional<CellRef> valCell;
      const PathExpression *functorPath =
          std::get_if<PathExpression>(&let.target);
      const ValueExpr *functorValue = std::get_if<ValueExpr>(&let.target);
      const auto functor =
          functorPath != nullptr    ? functorArgument(*functorPath)
          : functorValue != nullptr ? functorArgument(*functorValue)
                                    : std::nullopt;
      if (functor) {
        functorBindings_[let.varName] = *functor;
        varBindings_.erase(let.varName);
        streamBindings_.erase(let.varName);
        continue;
      }
      functorBindings_.erase(let.varName);
      if (functorPath != nullptr &&
          std::holds_alternative<PathExpression>(let.target)) {
        valCell = compilePathExpression(*functorPath, core_.home());
      } else if (functorValue != nullptr) {
        const auto values = compileArgument(*functorValue, ambientContext_);
        streamBindings_[let.varName] = values;
        valCell = values.empty() ? std::nullopt : std::optional{values.front()};
      } else if (std::holds_alternative<ValueExpr>(let.target)) {
        valCell =
            compileValueExpr(std::get<ValueExpr>(let.target), core_.home());
      }
      if (functorPath) streamBindings_[let.varName] = lastResultCells_;
      if (valCell && dynamicStreams_.contains(*valCell)) {
        varBindings_[let.varName] = valCell;
        continue;
      }
      CellRef bindOp = emitOp(OpcodeKind::Bind, "#BIND " + let.varName);
      emitInputConstant(bindOp, let.varName);
      if (valCell) {
        emitInput(bindOp, *valCell);
      }
      varBindings_[let.varName] = valCell;
    } else if (std::holds_alternative<ForClause>(binding)) {
      const auto &forCl = std::get<ForClause>(binding);
      if (forCl.destructurePattern)
        throw std::invalid_argument(
            "for destructuring requires Vortex contract lowering");
      functorBindings_.erase(forCl.varName);
      const auto inStream = compilePathExpression(forCl.inPath, core_.home());
      streamBindings_[forCl.varName] = lastResultCells_;
      if (inStream && dynamicStreams_.contains(*inStream)) {
        varBindings_[forCl.varName] = inStream;
        emitOp(OpcodeKind::Nop, "#FOR_BIND " + forCl.varName);
        continue;
      }
      CellRef bindOp = emitOp(OpcodeKind::Bind, "#FOR_BIND " + forCl.varName);
      emitInputConstant(bindOp, forCl.varName);
      if (inStream) {
        emitInput(bindOp, *inStream);
      }
      varBindings_[forCl.varName] = inStream;
    }
  }

  // 3. Process ActionClause
  return std::visit(
      [this](const auto &act) -> std::optional<CellRef> {
        using T = std::decay_t<decltype(act)>;
        if constexpr (std::is_same_v<T, ReturnClause>) {
          if (act.items.size() > 1)
            throw std::invalid_argument(
                "multiple return items require Vortex result lowering");
          std::optional<CellRef> lastRes;
          for (const auto &retItem : act.items) {
            if (std::holds_alternative<PathExpression>(retItem.item)) {
              lastRes = compilePathExpression(
                  std::get<PathExpression>(retItem.item), core_.home());
            } else if (std::holds_alternative<FieldWeave>(retItem.item)) {
              throw std::invalid_argument(
                  "field weaving requires Vortex graph lowering");
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
                  } else {
                    throw std::invalid_argument(
                        "nested weave loops require Vortex control lowering");
                  }
                },
                effItem.item);
          }
          return core_.home();
        } else if constexpr (std::is_same_v<T, ConditionalClause>) {
          throw std::invalid_argument(
              "conditional action clauses require Vortex graph lowering");
        } else {
          throw std::invalid_argument("unsupported VQL execution clause");
        }
      },
      block.action.clause);
}

std::optional<CellRef>
VQLCompiler::compilePathExpression(const PathExpression &path,
                                   CellRef ctxCell) {
  if (path.anchor.versionPin ||
      std::ranges::any_of(path.steps, [](const PathStep &step) {
        return step.versionPin.has_value();
      }))
    throw std::invalid_argument("version pins require historical VM lowering");
  if (!path.anchorPredicates.empty()) {
    // Refused rather than compiled without them, which would answer every
    // anchor cell whatever the predicate said.
    throw std::invalid_argument(
        "anchor predicates require Vortex graph lowering");
  }
  std::vector<CellRef> stream;
  if (path.anchor.kind == AnchorKind::Variable &&
      streamBindings_.contains(path.anchor.name)) {
    stream = streamBindings_.at(path.anchor.name);
  } else {
    if (path.anchor.kind == AnchorKind::Context) {
      if (ambientContext_)
        stream.push_back(*ambientContext_);
      else if (activeCalls_.empty() && !contextInputs_.empty())
        stream = contextInputs_;
      else if (!path.steps.empty() &&
               std::holds_alternative<FunctionInvocation>(
                   path.steps.front().selector))
        stream.push_back(ctxCell);
    } else if (const auto start = compileAnchor(path.anchor)) {
      stream.push_back(*start);
    }
  }
  if (path.anchor.derefMaster) {
    stream = emitMap(stream, [&](CellRef candidate) {
      return std::vector<CellRef>{emitDereference(candidate)};
    });
  }
  stream =
      compileSteps(path.steps, std::move(stream),
                   path.anchor.kind != AnchorKind::Context ||
                       ambientContext_.has_value() || !contextInputs_.empty());

  if (path.cloneTail && !stream.empty()) {
    stream = emitMap(stream, [&](CellRef master) {
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
      return std::vector<CellRef>{master};
    });
  }

  lastResultCells_ = stream;
  if (stream.empty()) return std::nullopt;
  return stream.front();
}

std::optional<CellRef> VQLCompiler::compileAnchor(const AnchorNode &anchor) {
  CellRef res{};
  switch (anchor.kind) {
  case AnchorKind::Home:
  case AnchorKind::Root:
    if (coordinator_) {
      const auto home = coordinator_->homeAnchor();
      if (!home) return std::nullopt;
      res = *home;
    } else {
      res = core_.home();
    }
    break;
  case AnchorKind::NamedStore: {
    if (!coordinator_)
      throw std::invalid_argument("named-store anchors require a coordinator");
    const auto home = coordinator_->resolveNamedStore(anchor.name);
    if (!home) return std::nullopt;
    res = *home;
    break;
  }
  case AnchorKind::Cursor:
  case AnchorKind::NamedCursor:
  case AnchorKind::AllCursors:
  case AnchorKind::AllNamedCursors:
    throw std::invalid_argument("cursor anchors require VM selector lowering");
  case AnchorKind::Variable: {
    if (functorBindings_.contains(anchor.name)) {
      throw std::invalid_argument(
          "a point-free functor must be applied to a path before it can be "
          "returned as a cell");
    }
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
    throw std::invalid_argument(
        "store generators and historical anchors require VM selector lowering");
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
  const auto result     = core_.arena().makeCell();
  const auto falseValue = emitConstant(false);
  const auto trueValue  = emitConstant(true);
  const auto initialize = emitOp(OpcodeKind::CopyCellRefs, "#BOOL_FALSE");
  emitInput(initialize, falseValue);
  emitOutput(initialize, result);
  const auto end    = vm_.mintOpcode(OpcodeKind::Nop, "#BOOL_END");
  const auto append = [&](CellRef opcode) {
    zigzag::expectWritten(
        core_.arena().link(*currentOp_, core_.dims().spin, false, opcode));
    currentOp_ = opcode;
    allOps_.push_back(opcode);
  };
  // False factors skip the remainder of their conjunction; a true term skips
  // all later alternatives, including effects and return-contract checks.
  for (const auto &term : expr.terms) {
    const auto nextTerm = vm_.mintOpcode(OpcodeKind::Nop, "#BOOL_NEXT_TERM");
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
                  const auto cells =
                      compileArgument(ValueExpr{.kind = t}, ctxCell);
                  return cells.empty() ? emitConstant(false)
                                             : emitCellPresence(cells.front());
                }
                return emitConstant(false);
              } else if constexpr (std::is_same_v<T, bool>) {
                return ctxCell;
              } else if constexpr (std::is_same_v<T,
                                                        std::shared_ptr<ValueExpr>>) {
                if (t) {
                  const auto value = compileValueExpr(*t, ctxCell);
                  return value ? *value : emitConstant(false);
                }
                return emitConstant(false);
              } else if constexpr (std::is_same_v<T, std::shared_ptr<
                                                               FunctionInvocation>>) {
                if (t) {
                  return compileValueExpr(ValueExpr{.kind = t}, ctxCell);
                }
                return emitConstant(false);
              } else {
                throw std::invalid_argument(
                    "VQL shape predicates require Vortex graph lowering");
              }
            },
            pt.kind);
      }

      if (!factCell) factCell = emitConstant(false);
      if (factor.negated) {
        CellRef notOp = emitOp(OpcodeKind::Not, "#NOT");
        emitInput(notOp, *factCell);
        CellRef notOut = core_.arena().makeCell();
        emitOutput(notOp, notOut);
        factCell = notOut;
      }

      const auto branch = emitOp(OpcodeKind::BranchFalseCell, "#BOOL_AND");
      emitInput(branch, *factCell);
      emitInput(branch, nextTerm);
    }
    const auto success = emitOp(OpcodeKind::CopyCellRefs, "#BOOL_TRUE");
    emitInput(success, trueValue);
    emitOutput(success, result);
    const auto done = emitOp(OpcodeKind::BranchFalseCell, "#BOOL_OR_DONE");
    emitInput(done, falseValue);
    emitInput(done, end);
    append(nextTerm);
  }
  append(end);
  return result;
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
            return it->second ? std::optional{emitFirstCell(*it->second)}
                              : std::nullopt;
          }
          CellRef resOp = emitOp(OpcodeKind::Resolve, "#RESOLVE " + val);
          emitInputConstant(resOp, val);
          CellRef outCell = core_.arena().makeCell();
          emitOutput(resOp, outCell);
          return outCell;
        } else if constexpr (std::is_same_v<T,
                                            std::shared_ptr<PathExpression>>) {
          if (val) {
            const auto result = compilePathExpression(*val, ctxCell);
            return result ? std::optional{emitFirstCell(*result)}
                          : std::nullopt;
          }
          throw std::invalid_argument("null compiled path expression");
        } else if constexpr (std::is_same_v<
                                 T, std::shared_ptr<FunctionInvocation>>) {
          if (!val) return std::nullopt;
          const auto definition = definitions_.find(val->name);
          if (definition == definitions_.end())
            throw std::invalid_argument("unknown compiled function: " +
                                        val->name);
          requirePositionalArguments(*val);
          const auto results =
              compileCall(definition->second, val->args, ctxCell, false);
          return results.empty()
                     ? std::nullopt
                     : std::optional{emitFirstCell(results.front())};
        } else if constexpr (std::is_same_v<T, std::shared_ptr<BooleanExpr>>) {
          if (val) {
            return compileBooleanExpr(*val, ctxCell);
          }
          throw std::invalid_argument("null compiled boolean expression");
        } else {
          throw std::invalid_argument(
              "value expression requires Vortex graph lowering");
        }
      },
      expr.kind);
}

CellRef VQLCompiler::linkAsExecutable(CellRef entryOp) {
  // Link posward off home along +d.spin
  zigzag::expectWritten(
      core_.arena().link(core_.home(), core_.dims().spin, false, entryOp));
  // Spawn main execution cursor
  return vm_.spawnCursor(entryOp, "main");
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

} // namespace xanadu::vql
