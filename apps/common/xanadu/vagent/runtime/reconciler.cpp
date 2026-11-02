/**
 * @file reconciler.cpp
 * @brief State reconciler for live agent upgrades between program versions
 *        (design/vagent-shared-program-and-live-upgrade.md §4.3).
 */
#include "common/xanadu/vagent/runtime/reconciler.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::runtime {

namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorError;

constexpr std::string_view kFailCycles     = "cycles";
constexpr std::string_view kFailFuel       = "fuel";
constexpr std::string_view kFailStatement  = "fail";
constexpr std::string_view kFailUnfinished = "unfinished";
constexpr std::string_view kVmMaxCycles    = "Exceeded max cycles";

template <class Pred>
bool removeFromRank(zigzag::ArenaManifold &arena, CellRef anchor,
                    zigzag::DimRef dim, Pred chosen) {
  CellRef before = anchor;
  for (const auto cell : zigzag::rankAfter(arena, anchor, dim)) {
    if (chosen(cell)) {
      const auto after = arena.linked(cell, dim);
      zigzag::expectWritten(arena.unlink(cell, dim, DimVector::POS));
      zigzag::expectWritten(arena.link(before, dim, DimVector::POS, after));
      return true;
    }
    before = cell;
  }
  return false;
}

void copyCellData(const zigzag::ArenaManifold &fromArena, CellRef from,
                  zigzag::ArenaManifold &toArena, CellRef to) {
  const std::string text = fromArena.textOf(from);
  if (!text.empty()) {
    const auto span = toArena.intern(text);
    zigzag::expectWritten(toArena.setContent(
        to, std::span<const xanadu::PrimediaSpan>{&span, 1}));
  }
  switch (fromArena.valueKindOf(from)) {
  case xanadu::ValueKind::Int64:
    zigzag::expectWritten(toArena.setValueBits(
        to, xanadu::ValueKind::Int64,
        std::bit_cast<std::uint64_t>(fromArena.asInt64(from).value_or(0))));
    break;
  case xanadu::ValueKind::Double:
    zigzag::expectWritten(toArena.setValueBits(
        to, xanadu::ValueKind::Double,
        xanadu::canonicalDoubleBits(fromArena.asDouble(from).value_or(0.0))));
    break;
  case xanadu::ValueKind::Bool:
    zigzag::expectWritten(toArena.setValueBits(
        to, xanadu::ValueKind::Bool, fromArena.asBool(from).value_or(false)));
    break;
  default:
    break;
  }
}

bool hasLinks(const zigzag::ArenaManifold &arena, CellRef cell,
              std::span<const DimRef> dims) {
  return std::ranges::any_of(dims, [&](DimRef d) {
    return d != zigzag::noCell && arena.linked(cell, d).has_value();
  });
}

CellRef cloneGraph(const zigzag::ArenaManifold &fromArena,
                   zigzag::ArenaManifold &toArena, CellRef from,
                   std::span<const DimRef> dimsFrom,
                   std::span<const DimRef> dimsInto) {
  if (from == zigzag::noCell) return zigzag::noCell;
  std::vector<CellRef> order{from};
  std::unordered_map<CellRef, CellRef> copy;
  const auto admit = [&](CellRef c) {
    const auto made = toArena.makeCell();
    copyCellData(fromArena, c, toArena, made);
    copy.emplace(c, made);
    order.push_back(c);
  };
  copy.emplace(from, toArena.makeCell());
  copyCellData(fromArena, from, toArena, copy.at(from));
  for (std::size_t i = 0; i < order.size(); ++i) {
    for (const auto d : dimsFrom) {
      if (d == zigzag::noCell) continue;
      if (const auto next = fromArena.linked(order[i], d);
          next && !copy.contains(*next)) {
        admit(*next);
      }
    }
  }
  for (const auto c : order) {
    for (std::size_t k = 0; k < dimsFrom.size(); ++k) {
      if (dimsFrom[k] == zigzag::noCell || dimsInto[k] == zigzag::noCell)
        continue;
      if (const auto next = fromArena.linked(c, dimsFrom[k])) {
        auto itNext = copy.find(*next);
        if (itNext != copy.end()) {
          zigzag::expectWritten(toArena.link(copy.at(c), dimsInto[k],
                                             DimVector::POS, itNext->second));
        }
      }
    }
  }
  return copy.at(from);
}

// -- AST equivalence helpers for plan body & where clause ---------------------

bool equalExpr(const ExprPtr &a, const ExprPtr &b);

bool equalLiteral(const ELiteral &a, const ELiteral &b) {
  return a.kind == b.kind && a.intValue == b.intValue &&
         a.floatValue == b.floatValue && a.boolValue == b.boolValue &&
         a.durationMs == b.durationMs && a.text == b.text;
}

bool equalTermArg(const TermArg &a, const TermArg &b) {
  if (a.logicVar != b.logicVar) return false;
  return equalExpr(a.expr, b.expr);
}

bool equalTerm(const Term &a, const Term &b) {
  if (a.name != b.name || a.args.size() != b.args.size()) return false;
  for (std::size_t i = 0; i < a.args.size(); ++i) {
    if (!equalTermArg(a.args[i], b.args[i])) return false;
  }
  return true;
}

bool equalGoalCall(const GoalCall &a, const GoalCall &b) {
  if (a.name != b.name || a.args.size() != b.args.size()) return false;
  for (std::size_t i = 0; i < a.args.size(); ++i) {
    if (!equalExpr(a.args[i], b.args[i])) return false;
  }
  return true;
}

bool equalTypeExpr(const TypeExpr &a, const TypeExpr &b) {
  if (a.kind != b.kind || a.name != b.name || a.args.size() != b.args.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.args.size(); ++i) {
    if (!equalTypeExpr(a.args[i], b.args[i])) return false;
  }
  return true;
}

bool equalStmt(const Stmt &a, const Stmt &b) {
  if (a.isLet != b.isLet || a.name != b.name) return false;
  if (a.type.has_value() != b.type.has_value()) return false;
  if (a.type.has_value() && !equalTypeExpr(*a.type, *b.type)) return false;
  return equalExpr(a.value, b.value);
}

bool equalExpr(const ExprPtr &a, const ExprPtr &b) {
  if (!a && !b) return true;
  if (!a || !b) return false;
  if (a->node.index() != b->node.index()) return false;
  return std::visit(
      [&](const auto &an) -> bool {
        using T        = std::decay_t<decltype(an)>;
        const auto &bn = std::get<T>(b->node);
        if constexpr (std::is_same_v<T, ELiteral>) {
          return equalLiteral(an, bn);
        } else if constexpr (std::is_same_v<T, EName>) {
          return an.name == bn.name;
        } else if constexpr (std::is_same_v<T, ESpecial>) {
          return an.kind == bn.kind;
        } else if constexpr (std::is_same_v<T, EUnary>) {
          return an.op == bn.op && equalExpr(an.operand, bn.operand);
        } else if constexpr (std::is_same_v<T, EBinary>) {
          return an.op == bn.op && equalExpr(an.left, bn.left) &&
                 equalExpr(an.right, bn.right);
        } else if constexpr (std::is_same_v<T, ERange>) {
          return equalExpr(an.lo, bn.lo) && equalExpr(an.hi, bn.hi) &&
                 equalExpr(an.by, bn.by);
        } else if constexpr (std::is_same_v<T, ECall>) {
          if (!equalExpr(an.callee, bn.callee) ||
              an.args.size() != bn.args.size()) {
            return false;
          }
          for (std::size_t i = 0; i < an.args.size(); ++i) {
            if (!equalExpr(an.args[i], bn.args[i])) return false;
          }
          return true;
        } else if constexpr (std::is_same_v<T, EIndex>) {
          return equalExpr(an.base, bn.base) && equalExpr(an.index, bn.index);
        } else if constexpr (std::is_same_v<T, EField>) {
          return an.name == bn.name && equalExpr(an.base, bn.base);
        } else if constexpr (std::is_same_v<T, EList>) {
          if (an.elems.size() != bn.elems.size()) return false;
          for (std::size_t i = 0; i < an.elems.size(); ++i) {
            if (!equalExpr(an.elems[i], bn.elems[i])) return false;
          }
          return true;
        } else if constexpr (std::is_same_v<T, ERecord>) {
          if (an.type != bn.type || an.fields.size() != bn.fields.size()) {
            return false;
          }
          for (std::size_t i = 0; i < an.fields.size(); ++i) {
            if (an.fields[i].name != bn.fields[i].name ||
                !equalExpr(an.fields[i].value, bn.fields[i].value)) {
              return false;
            }
          }
          return true;
        } else if constexpr (std::is_same_v<T, EBlock>) {
          if (an.stmts.size() != bn.stmts.size()) return false;
          for (std::size_t i = 0; i < an.stmts.size(); ++i) {
            if (!equalStmt(an.stmts[i], bn.stmts[i])) return false;
          }
          return true;
        } else if constexpr (std::is_same_v<T, EIf>) {
          return equalExpr(an.cond, bn.cond) &&
                 equalExpr(an.thenBranch, bn.thenBranch) &&
                 equalExpr(an.elseBranch, bn.elseBranch);
        } else if constexpr (std::is_same_v<T, EWhile>) {
          return equalExpr(an.cond, bn.cond) && equalExpr(an.body, bn.body);
        } else if constexpr (std::is_same_v<T, EFor>) {
          if (an.binder != bn.binder || !equalExpr(an.iterable, bn.iterable) ||
              an.believesTerm.has_value() != bn.believesTerm.has_value()) {
            return false;
          }
          if (an.believesTerm.has_value() &&
              !equalTerm(*an.believesTerm, *bn.believesTerm)) {
            return false;
          }
          return equalExpr(an.body, bn.body);
        } else if constexpr (std::is_same_v<T, EBelieves>) {
          return equalTerm(an.term, bn.term) && equalExpr(an.from, bn.from);
        } else if constexpr (std::is_same_v<T, EFindall>) {
          return equalTermArg(an.templ, bn.templ) &&
                 equalTerm(an.goal, bn.goal);
        } else if constexpr (std::is_same_v<T, EEmit>) {
          return equalExpr(an.event, bn.event) &&
                 equalExpr(an.space, bn.space) &&
                 equalExpr(an.recipients, bn.recipients);
        } else if constexpr (std::is_same_v<T, ETell>) {
          return an.untell == bn.untell && equalExpr(an.to, bn.to) &&
                 equalTerm(an.fact, bn.fact);
        } else if constexpr (std::is_same_v<T, EBeliefChange>) {
          return an.kind == bn.kind && equalTerm(an.fact, bn.fact);
        } else if constexpr (std::is_same_v<T, ESpawn>) {
          if (an.agent != bn.agent || !equalExpr(an.space, bn.space) ||
              an.args.size() != bn.args.size()) {
            return false;
          }
          for (std::size_t i = 0; i < an.args.size(); ++i) {
            if (!equalExpr(an.args[i], bn.args[i])) return false;
          }
          return true;
        } else if constexpr (std::is_same_v<T, EAdopt>) {
          return equalGoalCall(an.goal, bn.goal);
        } else if constexpr (std::is_same_v<T, EAchieve>) {
          return equalGoalCall(an.goal, bn.goal) && equalExpr(an.by, bn.by) &&
                 equalExpr(an.within, bn.within) &&
                 equalExpr(an.orElse, bn.orElse);
        } else if constexpr (std::is_same_v<T, EAsk>) {
          return equalExpr(an.question, bn.question) &&
                 equalExpr(an.within, bn.within) &&
                 equalExpr(an.orElse, bn.orElse);
        } else if constexpr (std::is_same_v<T, EAwait>) {
          if (an.eventType != bn.eventType || an.binder != bn.binder ||
              an.term.has_value() != bn.term.has_value()) {
            return false;
          }
          if (an.term.has_value() && !equalTerm(*an.term, *bn.term)) {
            return false;
          }
          return equalExpr(an.guard, bn.guard) &&
                 equalExpr(an.within, bn.within) &&
                 equalExpr(an.orElse, bn.orElse);
        } else if constexpr (std::is_same_v<T, EDrop>) {
          return equalExpr(an.target, bn.target);
        } else if constexpr (std::is_same_v<T, EStop>) {
          return an.keepChildren == bn.keepChildren &&
                 equalExpr(an.target, bn.target);
        } else if constexpr (std::is_same_v<T, ETerminate>) {
          return an.keepChildren == bn.keepChildren;
        } else if constexpr (std::is_same_v<T, EInstall>) {
          if (an.kind != bn.kind || an.name != bn.name ||
              an.args.size() != bn.args.size()) {
            return false;
          }
          for (std::size_t i = 0; i < an.args.size(); ++i) {
            if (!equalExpr(an.args[i], bn.args[i])) return false;
          }
          return true;
        } else if constexpr (std::is_same_v<T, EReturn>) {
          return equalExpr(an.value, bn.value);
        } else if constexpr (std::is_same_v<T, EYield> ||
                             std::is_same_v<T, EConsume> ||
                             std::is_same_v<T, EFail>) {
          return true;
        } else {
          return false;
        }
      },
      a->node);
}

bool planEqual(const PlanDecl &p1, const PlanDecl &p2) {
  if (p1.name != p2.name || p1.goal != p2.goal || p1.params != p2.params ||
      p1.hasParams != p2.hasParams) {
    return false;
  }
  if (!equalExpr(p1.where, p2.where)) return false;
  if (!equalExpr(p1.body, p2.body)) return false;
  return true;
}

const PlanDecl *findPlan(const Unit &unit, std::string_view agentType,
                         std::string_view goalName, std::string_view planName) {
  for (const auto &decl : unit.decls) {
    if (const auto *p = std::get_if<PlanDecl>(&decl)) {
      if (p->goal == goalName && p->name == planName) return p;
    } else if (const auto *a = std::get_if<AgentDecl>(&decl)) {
      if (a->name == agentType) {
        for (const auto &m : a->members) {
          if (const auto *p = std::get_if<PlanDecl>(&m)) {
            if (p->goal == goalName && p->name == planName) return p;
          }
        }
      }
    }
  }
  return nullptr;
}

void stageCancel(zigzag::ArenaManifold &arena, const ValueDims &vd,
                 zigzag::DimRef outboxDim, zigzag::CellRef toAgent,
                 std::string_view toPeer, std::string_view correlation) {
  if (toPeer.empty()) return;
  const zigzag::CellRef cancelEntry = arena.makeCell(kKindCancel);
  setField(arena, vd, cancelEntry, kOutboxReqTo,
           writeValue(arena, vd, WireValue::text(std::string(toPeer))));
  setField(arena, vd, cancelEntry, kOutboxCorrelation,
           writeValue(arena, vd, WireValue::text(std::string(correlation))));
  appendToRank(arena, toAgent, outboxDim, cancelEntry);
}

// -- Core reconciliation logic ------------------------------------------------

std::expected<CellRef, ActorError>
executeReconcile(const zigzag::ArenaManifold &fromArena,
                 std::optional<CellRef> oldStateRoot, WorkerContext &toContext,
                 const CompiledProgram &fromProgram,
                 const CompiledProgram &toProgram,
                 const ReconcileOptions &options, ReconcileOutcome *outcome) {
  ReconcileOutcome localOutcome;
  auto &out = outcome != nullptr ? *outcome : localOutcome;

  const auto &agentType = options.agentType;
  if (!toProgram.declares(agentType)) {
    return std::unexpected(ActorError::UnknownProcess);
  }

  // 1. Ensure toAgent is instantiated in toContext
  std::optional<CompiledAgent> compiledAgent;
  std::optional<CellRef> existingToAgent;
  for (std::uint32_t i = 0; i < toContext.arena.cellCount(); ++i) {
    const CellRef c = toContext.arena.refOf(i);
    if (zigzag::isEphemeral(c) && toContext.arena.textOf(c) == agentType) {
      existingToAgent = c;
      break;
    }
  }
  CellRef toAgent = zigzag::noCell;
  if (existingToAgent) {
    toAgent = *existingToAgent;
    if (outcome && outcome->compiledAgent) {
      compiledAgent = outcome->compiledAgent;
    }
  } else {
    auto compiled = const_cast<CompiledProgram &>(toProgram).instantiate(
        toContext, agentType);
    if (!compiled) {
      return std::unexpected(ActorError::ExecutionFailed);
    }
    toAgent       = compiled->agentCell;
    compiledAgent = *compiled;
    if (outcome != nullptr) {
      outcome->compiledAgent = *compiled;
    }
  }

  const AgentDims toDims = registerDimensions(toContext.arena);
  const ValueDims toVd   = valueDims(toContext.core);

  // 2. Find fromAgent in fromArena
  CellRef fromAgent = (oldStateRoot && *oldStateRoot != zigzag::noCell &&
                       fromArena.contains(*oldStateRoot))
                          ? *oldStateRoot
                          : zigzag::noCell;
  if (fromAgent == zigzag::noCell) {
    for (std::uint32_t i = 0; i < fromArena.cellCount(); ++i) {
      const CellRef c = fromArena.refOf(i);
      if (fromArena.textOf(c) == agentType) {
        fromAgent = c;
        break;
      }
    }
  }
  if (fromAgent == zigzag::noCell) {
    const auto itAg = fromProgram.agents().find(std::string(agentType));
    if (itAg != fromProgram.agents().end() &&
        fromArena.contains(itAg->second.agentCell)) {
      fromAgent = itAg->second.agentCell;
    }
  }
  if (fromAgent == zigzag::noCell) {
    return std::unexpected(ActorError::UnknownProcess);
  }

  const auto fromDim = [&](std::string_view name) -> DimRef {
    if (const auto d = fromArena.dimensionNamed(name)) return *d;
    const auto dimDims = fromArena.dimensionNamed("d.dims");
    const auto home    = fromArena.homeCell();
    if (dimDims && home != zigzag::noCell) {
      for (const auto dim : zigzag::rankAfter(fromArena, home, *dimDims)) {
        if (fromArena.textOf(dim) == name) return dim;
      }
    }
    for (std::uint32_t i = 0; i < fromArena.cellCount(); ++i) {
      const CellRef c = fromArena.refOf(i);
      if (fromArena.textOf(c) == name) return c;
    }
    return zigzag::noCell;
  };

  const ValueDims fromVd{
      .vars   = fromDim("d.vars"),
      .values = fromDim("d.values"),
      .grab   = fromDim("d.grab"),
      .step   = fromDim("d.step"),
  };
  const AgentDims fromDims{
      .behavior  = fromDim("d.behavior"),
      .skill     = fromDim("d.skill"),
      .capacity  = fromDim("d.capacity"),
      .space     = fromDim("d.space"),
      .member    = fromDim("d.member"),
      .holon     = fromDim("d.holon"),
      .intention = fromDim("d.intention"),
      .subgoal   = fromDim("d.subgoal"),
      .outbox    = fromDim("d.outbox"),
      .belief    = fromDim("d.belief"),
      .pending   = fromDim("d.pending"),
      .source    = fromDim("d.source"),
      .extends   = fromDim("d.extends"),
      .scan      = fromDim("d.scan"),
  };

  const std::array valueFrom{fromVd.vars, fromVd.values, fromVd.grab,
                             fromVd.step};
  const std::array valueInto{toVd.vars, toVd.values, toVd.grab, toVd.step};
  const std::array termFrom{fromVd.grab, fromVd.step, fromDims.source};
  const std::array termInto{toVd.grab, toVd.step, toDims.source};
  const std::array intentionFrom{fromVd.vars, fromVd.values, fromVd.grab,
                                 fromVd.step, fromDims.subgoal};
  const std::array intentionInto{toVd.vars, toVd.values, toVd.grab, toVd.step,
                                 toDims.subgoal};

  // 3. Fields reconciliation (§4.3.1)
  std::unordered_map<std::string, Type> toFieldTypes;
  if (auto it = toProgram.checked().agents.find(std::string(agentType));
      it != toProgram.checked().agents.end()) {
    for (const auto &p : it->second.params) {
      toFieldTypes[p.name] = p.type;
    }
    for (const auto &f : it->second.fields) {
      toFieldTypes[f.name] = f.type;
    }
  }

  std::unordered_map<std::string, Type> fromFieldTypes;
  if (auto it = fromProgram.checked().agents.find(std::string(agentType));
      it != fromProgram.checked().agents.end()) {
    for (const auto &p : it->second.params) {
      fromFieldTypes[p.name] = p.type;
    }
    for (const auto &f : it->second.fields) {
      fromFieldTypes[f.name] = f.type;
    }
  }

  const CellRef droppedRec = toContext.arena.makeCell("");
  const CellRef marker = toContext.arena.makeCell(std::string(kRecordMarker));
  zigzag::expectWritten(
      toContext.arena.link(droppedRec, toVd.vars, DimVector::POS, marker));

  std::unordered_set<std::string> seenFromFields;
  if (fromVd.vars != zigzag::noCell && fromVd.values != zigzag::noCell) {
    for (const auto &[nameCell, valueCell] :
         fieldsOf(fromArena, fromVd, fromAgent)) {
      if (!valueCell) continue;
      const auto name = fromArena.textOf(nameCell);
      if (name.empty() || name == kFieldTurn || name == kFieldDroppedFields) {
        continue;
      }
      seenFromFields.insert(name);

      // Reserved metadata fields starting with '$'
      if (name.starts_with('$')) {
        const auto target = fieldValue(toContext.arena, toVd, toAgent, name);
        const bool structured =
            hasLinks(fromArena, *valueCell, valueFrom) ||
            (target && hasLinks(toContext.arena, *target, valueInto));
        if (target && !structured) {
          copyCellData(fromArena, *valueCell, toContext.arena, *target);
        } else {
          setField(toContext.arena, toVd, toAgent, name,
                   cloneGraph(fromArena, toContext.arena, *valueCell, valueFrom,
                              valueInto));
        }
        out.preservedFields++;
        continue;
      }

      // User declared fields
      auto itTo = toFieldTypes.find(name);
      if (itTo != toFieldTypes.end()) {
        const auto &toType = itTo->second;
        auto itFrom        = fromFieldTypes.find(name);
        if (itFrom != fromFieldTypes.end() && itFrom->second != toType) {
          out.warnings.push_back(
              std::format("Field '{}' changed type from {} to {}", name,
                          typeName(itFrom->second), typeName(toType)));
          return std::unexpected(ActorError::NotAuthorized);
        }
        const auto target = fieldValue(toContext.arena, toVd, toAgent, name);
        const bool structured =
            hasLinks(fromArena, *valueCell, valueFrom) ||
            (target && hasLinks(toContext.arena, *target, valueInto));
        if (target && !structured) {
          copyCellData(fromArena, *valueCell, toContext.arena, *target);
        } else {
          setField(toContext.arena, toVd, toAgent, name,
                   cloneGraph(fromArena, toContext.arena, *valueCell, valueFrom,
                              valueInto));
        }
        out.preservedFields++;
      } else {
        // Field was removed in V2
        out.droppedFields++;
        std::string strVal;
        switch (fromArena.valueKindOf(*valueCell)) {
        case xanadu::ValueKind::Int64:
          strVal = std::to_string(fromArena.asInt64(*valueCell).value_or(0));
          break;
        case xanadu::ValueKind::Double:
          strVal = std::to_string(fromArena.asDouble(*valueCell).value_or(0.0));
          break;
        case xanadu::ValueKind::Bool:
          strVal =
              fromArena.asBool(*valueCell).value_or(false) ? "true" : "false";
          break;
        default:
          strVal = fromArena.textOf(*valueCell);
          break;
        }
        const CellRef valCell = toContext.arena.makeCell(strVal);
        setField(toContext.arena, toVd, droppedRec, name, valCell);
      }
    }
  }

  // Count added fields: declared in V2 but not present in V1
  for (const auto &[name, type] : toFieldTypes) {
    if (!seenFromFields.contains(name)) {
      out.addedFields++;
    }
  }

  setField(toContext.arena, toVd, toAgent, kFieldDroppedFields, droppedRec);

  // 4. Belief base reconciliation (§4.3.2)
  const auto rankedFrom = [&](CellRef owner, DimRef dim) {
    return dim == zigzag::noCell ? std::vector<CellRef>{}
                                 : zigzag::rankAfter(fromArena, owner, dim) |
                                       std::ranges::to<std::vector<CellRef>>();
  };
  const auto rankedTo = [&](CellRef owner, DimRef dim) {
    return dim == zigzag::noCell
               ? std::vector<CellRef>{}
               : zigzag::rankAfter(toContext.arena, owner, dim) |
                     std::ranges::to<std::vector<CellRef>>();
  };

  const auto freshPredicates = rankedTo(toAgent, toDims.belief);
  const auto oldClause       = fromDim("d.clause");
  const auto newClause       = toContext.core.dims().clause;

  for (const auto predicate : rankedFrom(fromAgent, fromDims.belief)) {
    const auto name    = fromArena.textOf(predicate);
    const auto clauses = rankedFrom(predicate, oldClause);

    auto itToPred   = toProgram.checked().predicates.find(name);
    auto itFromPred = fromProgram.checked().predicates.find(name);
    bool sigMatch   = false;
    if (itToPred != toProgram.checked().predicates.end()) {
      if (itFromPred != fromProgram.checked().predicates.end()) {
        if (itFromPred->second.params.size() ==
            itToPred->second.params.size()) {
          sigMatch = true;
          for (std::size_t i = 0; i < itFromPred->second.params.size(); ++i) {
            if (itFromPred->second.params[i].type !=
                itToPred->second.params[i].type) {
              sigMatch = false;
              break;
            }
          }
        }
      } else {
        sigMatch = true;
      }
    }

    if (!sigMatch) {
      out.droppedBeliefs += clauses.size();
      continue;
    }

    auto match = std::ranges::find_if(freshPredicates, [&](CellRef candidate) {
      return toContext.arena.textOf(candidate) == name;
    });
    CellRef targetPred = zigzag::noCell;
    if (match != freshPredicates.end()) {
      targetPred = *match;
    } else {
      targetPred = toContext.arena.makeCell(name);
      appendToRank(toContext.arena, toAgent, toDims.belief, targetPred);
    }

    for (const auto clause : clauses) {
      appendToRank(
          toContext.arena, targetPred, newClause,
          cloneGraph(fromArena, toContext.arena, clause, termFrom, termInto));
      out.preservedBeliefs++;
    }
  }

  // 5. Hosted spaces and children
  auto toSpaces = rankedTo(toAgent, toDims.space);
  for (const auto space : rankedFrom(fromAgent, fromDims.space)) {
    const auto name  = fromArena.textOf(space);
    const auto match = std::ranges::find_if(
        toSpaces, [&](CellRef c) { return toContext.arena.textOf(c) == name; });
    CellRef fresh;
    if (match == toSpaces.end()) {
      fresh = toContext.arena.makeCell(std::string_view(name));
      appendToRank(toContext.arena, toAgent, toDims.space, fresh);
      toSpaces.push_back(fresh);
    } else {
      fresh = *match;
    }
    for (const auto member : rankedFrom(space, fromDims.member)) {
      appendToRank(
          toContext.arena, fresh, toDims.member,
          toContext.arena.makeCell(std::string_view(fromArena.textOf(member))));
    }
  }

  for (const auto child : rankedFrom(fromAgent, fromDims.holon)) {
    appendToRank(
        toContext.arena, toAgent, toDims.holon,
        toContext.arena.makeCell(std::string_view(fromArena.textOf(child))));
  }

  // 6. Mounted skills and behaviors (§4.3.3)
  std::unordered_set<std::string> remountedBehaviors;
  for (const auto b : rankedFrom(fromAgent, fromDims.behavior)) {
    const auto name = fromArena.textOf(b);
    if (!remountedBehaviors.insert(name).second) continue;
    if (compiledAgent && compiledAgent->behaviors.contains(name)) {
      const auto &bc = compiledAgent->behaviors.at(name);
      if (bc.cell != zigzag::noCell) {
        appendToRank(toContext.arena, toAgent, toDims.behavior, bc.cell);
      }
    } else if (toProgram.checked().behaviors.contains(name)) {
      const CellRef bCell = toContext.arena.makeCell(name);
      appendToRank(toContext.arena, toAgent, toDims.behavior, bCell);
    }
  }

  std::unordered_set<std::string> remountedSkills;
  const DimRef fromClone = fromDim("d.clone");
  for (const auto s : rankedFrom(fromAgent, fromDims.skill)) {
    const auto name = fromArena.textOf(s);
    if (!remountedSkills.insert(name).second) continue;
    if (compiledAgent && compiledAgent->skills.contains(name)) {
      const auto &sc = compiledAgent->skills.at(name);
      if (sc.instances.empty()) {
        if (sc.cell != zigzag::noCell) {
          appendToRank(toContext.arena, toAgent, toDims.skill, sc.cell);
        }
      } else {
        for (const auto inst : sc.instances) {
          appendToRank(toContext.arena, toAgent, toDims.skill, inst);
        }
      }
      CellRef fromSkillCell = s;
      if (fromClone != zigzag::noCell) {
        CellRef cur = s;
        while (cur != zigzag::noCell) {
          fromSkillCell   = cur;
          const auto prev = fromArena.linked(cur, fromClone, DimVector::NEG);
          cur             = prev.value_or(zigzag::noCell);
        }
      }
      if (fromSkillCell != zigzag::noCell && sc.cell != zigzag::noCell) {
        for (const auto &[nameCell, valueCell] :
             fieldsOf(fromArena, fromVd, fromSkillCell)) {
          if (!valueCell) continue;
          const auto fname  = fromArena.textOf(nameCell);
          const auto target = fieldValue(toContext.arena, toVd, sc.cell, fname);
          const bool structured =
              hasLinks(fromArena, *valueCell, valueFrom) ||
              (target && hasLinks(toContext.arena, *target, valueInto));
          if (target && !structured) {
            copyCellData(fromArena, *valueCell, toContext.arena, *target);
          } else {
            setField(toContext.arena, toVd, sc.cell, fname,
                     cloneGraph(fromArena, toContext.arena, *valueCell,
                                valueFrom, valueInto));
          }
        }
      }
    } else if (toProgram.checked().skills.contains(name)) {
      const CellRef sCell = toContext.arena.makeCell(name);
      appendToRank(toContext.arena, toAgent, toDims.skill, sCell);
      for (const auto &[nameCell, valueCell] : fieldsOf(fromArena, fromVd, s)) {
        if (!valueCell) continue;
        const auto fname = fromArena.textOf(nameCell);
        setField(toContext.arena, toVd, sCell, fname,
                 cloneGraph(fromArena, toContext.arena, *valueCell, valueFrom,
                            valueInto));
      }
    }
  }

  // 7. Intentions, plans, and activation frames (§4.3.4)
  auto cancelIntentionDelegates = [&](auto &self, CellRef intentCell) -> void {
    if (const auto delCell =
            fieldValue(fromArena, fromVd, intentCell, kIntentionDelegator)) {
      std::string peer;
      std::string corr;
      if (const auto pCell =
              fieldValue(fromArena, fromVd, *delCell, kDelegatorPeer)) {
        if (auto val = readValue(fromArena, fromVd, *pCell)) {
          if (auto *s = std::get_if<std::string>(&val->data)) peer = *s;
        }
        if (peer.empty()) peer = fromArena.textOf(*pCell);
      }
      if (const auto cCell =
              fieldValue(fromArena, fromVd, *delCell, kDelegatorCorr)) {
        if (auto val = readValue(fromArena, fromVd, *cCell)) {
          if (auto *s = std::get_if<std::string>(&val->data))
            corr = *s;
          else if (auto *i = std::get_if<std::int64_t>(&val->data))
            corr = std::to_string(*i);
        }
        if (corr.empty()) corr = fromArena.textOf(*cCell);
      }
      if (!peer.empty()) {
        stageCancel(toContext.arena, toVd, toDims.outbox, toAgent, peer, corr);
      }
    }

    if (const auto waitCell =
            fieldValue(fromArena, fromVd, intentCell, kIntentionWaiting)) {
      std::string toPeer;
      std::string corr;
      if (const auto toCell =
              fieldValue(fromArena, fromVd, *waitCell, kWaitingTo)) {
        if (auto val = readValue(fromArena, fromVd, *toCell)) {
          if (auto *s = std::get_if<std::string>(&val->data)) toPeer = *s;
        }
        if (toPeer.empty()) toPeer = fromArena.textOf(*toCell);
      }
      if (const auto corrCell =
              fieldValue(fromArena, fromVd, *waitCell, kWaitingCorrelation)) {
        if (auto val = readValue(fromArena, fromVd, *corrCell)) {
          if (auto *s = std::get_if<std::string>(&val->data))
            corr = *s;
          else if (auto *i = std::get_if<std::int64_t>(&val->data))
            corr = std::to_string(*i);
        }
        if (corr.empty()) corr = fromArena.textOf(*corrCell);
      }
      if (corr.empty()) {
        if (const auto keyCell =
                fieldValue(fromArena, fromVd, *waitCell, kWaitingKey)) {
          if (auto k = fromArena.asInt64(*keyCell)) {
            corr = std::to_string(*k);
          }
        }
      }
      if (!toPeer.empty()) {
        stageCancel(toContext.arena, toVd, toDims.outbox, toAgent, toPeer,
                    corr);
      }
    }

    if (fromDims.subgoal != zigzag::noCell) {
      for (const auto sub : rankedFrom(intentCell, fromDims.subgoal)) {
        self(self, sub);
      }
    }
  };

  for (const auto top : rankedFrom(fromAgent, fromDims.intention)) {
    std::string goalName;
    if (const auto gCell = fieldValue(fromArena, fromVd, top, kIntentionGoal)) {
      goalName = fromArena.textOf(*gCell);
    }
    if (goalName.empty()) {
      goalName = fromArena.textOf(top);
    }

    bool goalExists = toProgram.checked().goals.contains(goalName);
    if (!goalExists) {
      if (auto it = toProgram.checked().agents.find(std::string(agentType));
          it != toProgram.checked().agents.end()) {
        goalExists =
            std::ranges::any_of(it->second.goals, [&](const GoalInfo &gi) {
              return gi.name == goalName;
            });
      }
    }

    if (!goalExists) {
      out.abortedIntentions++;
      cancelIntentionDelegates(cancelIntentionDelegates, top);
      continue;
    }

    std::string planName;
    if (const auto pCell = fieldValue(fromArena, fromVd, top, kIntentionPlan)) {
      planName = fromArena.textOf(*pCell);
    }

    bool planIdentical = true;
    if (!planName.empty()) {
      const PlanDecl *p1 =
          findPlan(fromProgram.unit(), agentType, goalName, planName);
      const PlanDecl *p2 =
          findPlan(toProgram.unit(), agentType, goalName, planName);
      if (!p1 || !p2 || !planEqual(*p1, *p2)) {
        planIdentical = false;
      }
    }

    if (planIdentical) {
      const CellRef clonedIntention = cloneGraph(
          fromArena, toContext.arena, top, intentionFrom, intentionInto);
      appendToRank(toContext.arena, toAgent, toDims.intention, clonedIntention);
      const bool isWaiting =
          fieldValue(toContext.arena, toVd, clonedIntention, kIntentionWaiting)
              .has_value();
      if (!isWaiting) {
        setField(toContext.arena, toVd, clonedIntention, kIntentionReady,
                 toContext.arena.makeScalarCell(std::int64_t{0}));
      }
      const auto kindCell =
          fieldValue(toContext.arena, toVd, clonedIntention, kIntentionKind);
      if (kindCell && toContext.arena.textOf(*kindCell) == kIntentionMaintain) {
        setField(toContext.arena, toVd, clonedIntention, kIntentionBlockedUntil,
                 toContext.arena.makeScalarCell(std::int64_t{0}));
      }
      out.preservedIntentions++;
    } else {
      out.abortedIntentions++;
      cancelIntentionDelegates(cancelIntentionDelegates, top);

      const CellRef clonedIntention = cloneGraph(
          fromArena, toContext.arena, top, intentionFrom, intentionInto);

      // Clear tried list
      const CellRef emptyTried =
          toContext.arena.makeCell(std::string(kListContent));
      setField(toContext.arena, toVd, clonedIntention, kIntentionTried,
               emptyTried);

      // Clear plan name
      setField(toContext.arena, toVd, clonedIntention, kIntentionPlan,
               std::nullopt);

      // Cancel subgoals on d.subgoal
      if (toDims.subgoal != zigzag::noCell) {
        zigzag::expectWritten(toContext.arena.unlink(
            clonedIntention, toDims.subgoal, DimVector::POS));
      }

      // Clear waiting and frame
      setField(toContext.arena, toVd, clonedIntention, kIntentionWaiting,
               std::nullopt);
      setField(toContext.arena, toVd, clonedIntention, kIntentionFrame,
               std::nullopt);

      // Fail current step with cause "upgraded", set ready for re-deliberation
      setField(toContext.arena, toVd, clonedIntention, kIntentionStep,
               toContext.arena.makeScalarCell(std::int64_t{0}));
      setField(toContext.arena, toVd, clonedIntention, kIntentionFaulted,
               toContext.arena.makeCell("upgraded"));
      setField(toContext.arena, toVd, clonedIntention, kIntentionReady,
               toContext.arena.makeScalarCell(std::int64_t{0}));

      appendToRank(toContext.arena, toAgent, toDims.intention, clonedIntention);
    }
  }

  // 8. Event dispatch (§4.3.5)
  const std::string fromVer = !options.fromVersion.empty()
                                  ? options.fromVersion
                                  : std::string(fromProgram.programRef());
  const CellRef evRec       = toContext.arena.makeCell(kEventUpgrade);
  const CellRef evMarker = toContext.arena.makeCell(std::string(kRecordMarker));
  zigzag::expectWritten(
      toContext.arena.link(evRec, toVd.vars, DimVector::POS, evMarker));
  setField(toContext.arena, toVd, evRec, "from_version",
           toContext.arena.makeCell(fromVer));

  const CellRef outboxEntry = toContext.arena.makeCell(kKindEvent);
  setField(toContext.arena, toVd, outboxEntry, kOutboxEvent, evRec);
  setField(toContext.arena, toVd, outboxEntry, kOutboxSpace,
           toContext.arena.makeCell(kSpaceInner));
  appendToRank(toContext.arena, toAgent, toDims.outbox, outboxEntry);

  if (compiledAgent && compiledAgent->dispatchEntry != zigzag::noCell) {
    const CellRef turnCell = toContext.arena.makeCell();
    const CellRef turnMarker =
        toContext.arena.makeCell(std::string(kRecordMarker));
    zigzag::expectWritten(
        toContext.arena.link(turnCell, toVd.vars, DimVector::POS, turnMarker));

    const CellRef turnEvRec = toContext.arena.makeCell(kEventUpgrade);
    const CellRef turnEvMarker =
        toContext.arena.makeCell(std::string(kRecordMarker));
    zigzag::expectWritten(toContext.arena.link(turnEvRec, toVd.vars,
                                               DimVector::POS, turnEvMarker));
    setField(toContext.arena, toVd, turnEvRec, "from_version",
             toContext.arena.makeCell(fromVer));

    setField(toContext.arena, toVd, turnCell, kTurnEvent, turnEvRec);
    setField(toContext.arena, toVd, toAgent, kFieldTurn, turnCell);

    const CellRef cursor =
        toContext.vm.spawnCursor(compiledAgent->dispatchEntry);
    toContext.vm.pushChoicePoint(cursor, compiledAgent->failOp);

    const std::uint64_t work = options.maxInstructions;
    const auto startCycles   = toContext.vm.totalCycles();
    std::optional<std::string> failure;

    try {
      for (;;) {
        const auto used = toContext.vm.totalCycles() - startCycles;
        if (used >= work) {
          failure = std::string(kFailCycles);
          break;
        }
        const auto result = toContext.vm.run(cursor, work - used);
        if (result.success) break;
        if (result.contractViolation !=
            zigzag::vortex::ContractViolationKind::None) {
          if (toContext.vm.backtrack(cursor)) continue;
        }
        failure = result.errorMessage == kVmMaxCycles
                      ? std::string(kFailCycles)
                      : std::string(kFailStatement);
        break;
      }
      if (!failure) {
        const auto at = toContext.vm.getCursorOpcode(cursor);
        if (at == compiledAgent->failOp) {
          failure = std::string(kFailStatement);
        } else if (at != compiledAgent->haltOp) {
          failure = std::string(kFailUnfinished);
        }
      }
    } catch (...) {
      failure = std::string(kFailFuel);
    }

    toContext.vm.clearChoicePoints();
    zigzag::expectWritten(toContext.arena.unlink(
        cursor, toContext.core.dims().vars, DimVector::POS));
    zigzag::expectWritten(toContext.arena.unlink(
        cursor, toContext.core.dims().stack, DimVector::POS));
    removeFromRank(toContext.arena, toContext.core.home(),
                   toContext.core.dims().cursors,
                   [&](CellRef c) { return c == cursor; });
    setField(toContext.arena, toVd, toAgent, kFieldTurn, std::nullopt);

    if (failure) {
      if (*failure == kFailCycles || *failure == kFailFuel) {
        return std::unexpected(ActorError::ResourceLimit);
      }
      return std::unexpected(ActorError::ExecutionFailed);
    }
  }

  return toAgent;
}

} // namespace

StateReconciler makeStateReconciler(const CompiledProgram &fromProgram,
                                    const CompiledProgram &toProgram,
                                    std::string agentType,
                                    ReconcileOutcome *outcome) {
  ReconcileOptions options{
      .fromVersion = std::string(fromProgram.programRef()),
      .toVersion   = std::string(toProgram.programRef()),
      .agentType   = std::move(agentType),
  };
  return makeStateReconciler(fromProgram, toProgram, std::move(options),
                             outcome);
}

StateReconciler makeStateReconciler(const CompiledProgram &fromProgram,
                                    const CompiledProgram &toProgram,
                                    ReconcileOptions options,
                                    ReconcileOutcome *outcome) {
  return [fromProgram = &fromProgram, toProgram = &toProgram,
          opts = std::move(options), outcome](
             const identity::ActorPid &, const zigzag::ArenaManifold &oldArena,
             std::optional<CellRef> oldStateRoot,
             WorkerContext &newContext) -> std::expected<CellRef, ActorError> {
    return executeReconcile(oldArena, oldStateRoot, newContext, *fromProgram,
                            *toProgram, opts, outcome);
  };
}

std::expected<ReconcileOutcome, ActorError>
reconcileState(const WorkerContext &fromContext, WorkerContext &toContext,
               const CompiledProgram &fromProgram,
               const CompiledProgram &toProgram, std::string_view agentType) {
  ReconcileOptions options{
      .fromVersion = std::string(fromProgram.programRef()),
      .toVersion   = std::string(toProgram.programRef()),
      .agentType   = std::string(agentType),
  };
  return reconcileState(fromContext, toContext, fromProgram, toProgram,
                        options);
}

std::expected<ReconcileOutcome, ActorError>
reconcileState(const WorkerContext &fromContext, WorkerContext &toContext,
               const CompiledProgram &fromProgram,
               const CompiledProgram &toProgram,
               const ReconcileOptions &options) {
  ReconcileOutcome outcome;
  auto result =
      executeReconcile(fromContext.arena, /*oldStateRoot=*/std::nullopt,
                       toContext, fromProgram, toProgram, options, &outcome);
  if (!result) {
    return std::unexpected(result.error());
  }
  return outcome;
}

} // namespace xanadu::vagent::runtime
