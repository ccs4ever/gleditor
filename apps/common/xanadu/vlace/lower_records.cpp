/**
 * @file lower_records.cpp
 * @brief Card L4: record construction and update, field accessors, record
 *        patterns.
 */
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/vlace/lower_cards.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vlace::lower {

namespace {

using zigzag::CellRef;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

constexpr std::int64_t linkPosward = 1;

Pattern clonePat(const PatternNode &p) {
  auto out  = std::make_unique<PatternNode>();
  out->loc  = p.loc;
  out->node = std::visit(
      [](const auto &n) -> decltype(PatternNode::node) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, PConstructor>) {
          PConstructor c{.ctor = n.ctor, .args = {}};
          for (const auto &a : n.args) c.args.push_back(clonePat(*a));
          return c;
        } else if constexpr (std::is_same_v<T, PTuple>) {
          PTuple t;
          for (const auto &a : n.elems) t.elems.push_back(clonePat(*a));
          return t;
        } else if constexpr (std::is_same_v<T, PRecord>) {
          PRecord r{.ctor = n.ctor, .fields = {}};
          for (const auto &[name, a] : n.fields)
            r.fields.emplace_back(name, clonePat(*a));
          return r;
        } else {
          return n;
        }
      },
      p.node);
  return out;
}

CellRef emit(LowerContext &ctx, OpcodeKind kind, std::string_view label,
             std::initializer_list<CellRef> inputs,
             std::initializer_list<CellRef> outputs = {}) {
  const auto op = ctx.emitOp(kind, label);
  for (const auto cell : inputs) ctx.emitInput(op, cell);
  for (const auto cell : outputs) ctx.emitOutput(op, cell);
  return op;
}

CellRef intConstant(LowerContext &ctx, std::int64_t value) {
  return ctx.emitConstant(CellValue{value}).cell;
}

Operand emitLinkRead(LowerContext &ctx, CellRef subject, zigzag::DimRef dim) {
  const auto out = ctx.newRegister();
  emit(ctx, OpcodeKind::Link, "#vlace_record_link",
       {subject, intConstant(ctx, static_cast<std::int64_t>(dim)),
        intConstant(ctx, linkPosward)},
       {out.cell});
  return out;
}

} // namespace

Lowered<Operand> lowerRecordUpdate(LowerContext &ctx, const ERecordUpdate &e,
                                   SourceLocation loc) {
  const bool bareCtor = std::holds_alternative<ECtor>(e.record->node);
  if (bareCtor) {
    const auto &tag = std::get<ECtor>(e.record->node).name;
    const auto info = ctx.state().ctors().find(tag);
    if (!info) {
      return fail(CompileError::UnboundIdentifier,
                  "unknown constructor '" + tag + "'", loc);
    }
    if (info->fields.empty() && info->arity > 0) {
      return fail(CompileError::TypeMismatch,
                  "'" + tag + "' is not a record constructor", loc);
    }

    std::vector<const ExprNode *> slots(info->arity, nullptr);
    for (const auto &[name, expr] : e.fields) {
      const auto idx = ctx.state().ctors().fieldIndex(tag, name);
      if (!idx) {
        return fail(CompileError::UnboundIdentifier,
                    "record '" + tag + "' has no field '" + name + "'", loc);
      }
      slots[*idx] = expr.get();
    }

    for (std::size_t k = 0; k < info->arity; ++k) {
      if (slots[k] == nullptr) {
        return fail(CompileError::NonExhaustiveMatch,
                    "record construction of '" + tag + "' leaves field '" +
                        info->fields[k] + "' unset",
                    loc);
      }
    }

    std::vector<Operand> fieldOps;
    fieldOps.reserve(info->arity);
    for (std::size_t k = 0; k < info->arity; ++k) {
      auto field = lowerLazy(ctx, *slots[k]);
      if (!field) return field;
      fieldOps.push_back(*field);
    }
    return makeTerm(ctx, tag, fieldOps, loc);
  }

  // Functional update: r { f = e, ... }
  if (e.fields.empty()) {
    return lowerExpr(ctx, *e.record);
  }

  std::string ctorName;
  for (const auto &[name, _] : e.fields) {
    if (const auto owner = ctx.state().ctors().fieldOwner(name)) {
      ctorName = *owner;
      break;
    }
  }
  if (ctorName.empty()) {
    return fail(CompileError::UnboundIdentifier,
                "unknown field '" + e.fields.front().first + "'", loc);
  }

  const auto info = ctx.state().ctors().find(ctorName);
  if (!info) {
    return fail(CompileError::UnboundIdentifier,
                "unknown record constructor '" + ctorName + "'", loc);
  }

  std::vector<const ExprNode *> updatedExprs(info->arity, nullptr);
  for (const auto &[name, expr] : e.fields) {
    const auto idx = ctx.state().ctors().fieldIndex(ctorName, name);
    if (!idx) {
      return fail(CompileError::UnboundIdentifier,
                  "record '" + ctorName + "' has no field '" + name + "'", loc);
    }
    updatedExprs[*idx] = expr.get();
  }

  auto recExpr = lowerExpr(ctx, *e.record);
  if (!recExpr) return recExpr;
  auto rec = forceOperand(ctx, *recExpr, e.record->loc);
  if (!rec) return rec;

  const auto grabDim = ctx.core().dims().grab;
  const auto stepDim = ctx.core().dims().step;
  const auto header  = emitLinkRead(ctx, rec->cell, grabDim);

  std::vector<Operand> existingSlots;
  existingSlots.reserve(info->arity);
  Operand prev = header;
  for (std::size_t i = 0; i < info->arity; ++i) {
    auto slot = emitLinkRead(ctx, prev.cell, stepDim);
    prev      = slot;
    existingSlots.push_back(slot);
  }

  std::vector<Operand> fieldOps;
  fieldOps.reserve(info->arity);
  for (std::size_t k = 0; k < info->arity; ++k) {
    if (updatedExprs[k] != nullptr) {
      auto field = lowerLazy(ctx, *updatedExprs[k]);
      if (!field) return field;
      fieldOps.push_back(*field);
    } else {
      fieldOps.push_back(existingSlots[k]);
    }
  }

  return makeTerm(ctx, ctorName, fieldOps, loc);
}

std::optional<Lowered<Operand>> lowerFieldAccessor(LowerContext &ctx,
                                                   std::string_view name,
                                                   SourceLocation loc) {
  const auto owner = ctx.state().ctors().fieldOwner(name);
  if (!owner) return std::nullopt;

  ELambda lambda;
  auto pat = std::make_unique<PatternNode>();
  pat->loc = loc;
  PRecord prec;
  prec.ctor    = *owner;
  auto valPat  = std::make_unique<PatternNode>();
  valPat->loc  = loc;
  valPat->node = PVar{.name = "#field_val"};
  prec.fields.emplace_back(std::string(name), std::move(valPat));
  pat->node = std::move(prec);
  lambda.params.push_back(std::move(pat));

  auto body   = std::make_unique<ExprNode>();
  body->loc   = loc;
  body->node  = EVar{.name = "#field_val"};
  lambda.body = std::move(body);

  ExprNode root;
  root.loc  = loc;
  root.node = std::move(lambda);
  return lowerExpr(ctx, root);
}

Lowered<void> lowerPatRecord(LowerContext &ctx, const PRecord &p,
                             SourceLocation loc, PatternSite &site) {
  const auto info = ctx.state().ctors().find(p.ctor);
  if (!info) {
    return fail(CompileError::UnboundIdentifier,
                "unknown constructor '" + p.ctor + "' in pattern", loc);
  }
  if (info->fields.empty() && info->arity > 0) {
    return fail(CompileError::TypeMismatch,
                "'" + p.ctor + "' is not a record constructor", loc);
  }

  std::unordered_set<std::string_view> seen;
  for (const auto &[name, _] : p.fields) {
    if (!seen.insert(name).second) {
      return fail(CompileError::NonLinearPattern,
                  "field '" + name + "' bound twice in record pattern", loc);
    }
    const auto idx = ctx.state().ctors().fieldIndex(p.ctor, name);
    if (!idx) {
      return fail(CompileError::UnboundIdentifier,
                  "record '" + p.ctor + "' has no field '" + name + "'", loc);
    }
  }

  PConstructor pcon;
  pcon.ctor = p.ctor;
  pcon.args.resize(info->arity);
  for (std::size_t k = 0; k < info->arity; ++k) {
    auto wild    = std::make_unique<PatternNode>();
    wild->loc    = loc;
    wild->node   = PWildcard{};
    pcon.args[k] = std::move(wild);
  }
  for (const auto &[name, pat] : p.fields) {
    const auto idx = *ctx.state().ctors().fieldIndex(p.ctor, name);
    pcon.args[idx] = clonePat(*pat);
  }

  return lowerPatConstructor(ctx, pcon, loc, site);
}

} // namespace xanadu::vlace::lower
