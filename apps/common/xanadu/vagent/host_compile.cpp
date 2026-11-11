#include "common/xanadu/vagent/host_compile.hpp"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/ast_clone.hpp"
#include "common/xanadu/vagent/cell_path.hpp"
#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vagent/fragment.hpp"
#include "common/xanadu/vagent/frontend.hpp"
#include "common/xanadu/vagent/records.hpp"
#include "common/xanadu/vagent/token.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::vagent {
namespace {

std::string_view trimWhitespace(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                        s.front() == '\r' || s.front() == '\n')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                        s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
}

bool isPureExpr(const Expr &expr) {
  switch (expr.kind) {
  case ExprKind::Emit:
  case ExprKind::Tell:
  case ExprKind::Untell:
  case ExprKind::Believe:
  case ExprKind::Forget:
  case ExprKind::Revise:
  case ExprKind::Spawn:
  case ExprKind::Adopt:
  case ExprKind::Achieve:
  case ExprKind::Ask:
  case ExprKind::AwaitTerm:
  case ExprKind::AwaitEvent:
  case ExprKind::Drop:
  case ExprKind::Install:
  case ExprKind::Uninstall:
  case ExprKind::Mount:
  case ExprKind::Unmount:
  case ExprKind::Stop:
  case ExprKind::Terminate:
  case ExprKind::Yield:
  case ExprKind::Consume:
  case ExprKind::Splice:
    return false;
  case ExprKind::Cells: {
    const auto path = parseCellsPath(expr.text, expr.loc);
    return !(path && path->mutating);
  }
  case ExprKind::Code:
    return true;
  case ExprKind::Static:
    return !expr.children.empty() && expr.children.front() &&
           isPureExpr(*expr.children.front());
  case ExprKind::Binary:
    if (expr.text == ":=" || expr.text == "<-") return false;
    break;
  case ExprKind::Call: {
    if (expr.children.empty() || !expr.children.front()) return false;
    const auto &callee = *expr.children.front();
    if (callee.kind == ExprKind::Identifier) {
      if (callee.text == "abs" || callee.text == "min" ||
          callee.text == "max" || callee.text == "sqrt" ||
          callee.text == "int" || callee.text == "float" ||
          callee.text == "str" || callee.text == "len") {
        break;
      }
      return false;
    }
    return false;
  }
  default:
    break;
  }
  for (const auto &child : expr.children) {
    if (child && !isPureExpr(*child)) return false;
  }
  const Expr *extra[] = {expr.scope.get(), expr.recipients.get(),
                         expr.deadline.get(), expr.guard.get(),
                         expr.recovery.get()};
  for (const auto *child : extra) {
    if (child && !isPureExpr(*child)) return false;
  }
  return true;
}

} // namespace

std::optional<zigzag::CellRef>
hostCompileFragment(MetaRuntime &meta, zigzag::vortex::VortexCore &core,
                    zigzag::vortex::VortexVM &vm, std::string_view label,
                    zigzag::CellRef paramsCell, std::string_view resultType,
                    zigzag::CellRef srcCell) {
  const auto recordFault =
      [&](std::string_view why) -> std::optional<zigzag::CellRef> {
    if (meta.agentCell() != zigzag::noCell &&
        core.arena().contains(meta.agentCell()) && !label.empty()) {
      if (const auto rec = varsRecord(core.arena(), core, meta.agentCell(),
                                      std::format("fault:{}", label), true)) {
        if (const auto old = core.arena().linked(*rec, core.dims().values,
                                                 zigzag::DimVector::POS)) {
          zigzag::expectWritten(core.arena().unlink(*rec, core.dims().values,
                                                    zigzag::DimVector::POS));
        }
        zigzag::expectWritten(core.arena().link(*rec, core.dims().values,
                                                zigzag::DimVector::POS,
                                                core.arena().makeCell(why)));
      }
    }
    return std::nullopt;
  };

  if (label.empty()) {
    return std::nullopt;
  }
  if (label.starts_with("decl:") || label.starts_with("patch:") ||
      label.starts_with("boot:")) {
    return recordFault("reserved label prefix");
  }
  if (srcCell == zigzag::noCell || !core.arena().contains(srcCell)) {
    return recordFault("invalid source cell");
  }
  if (resultType.empty()) {
    return recordFault("missing result type");
  }

  const std::string srcText{core.arena().textOf(srcCell)};
  for (std::size_t i = 0; i < srcText.size(); ++i) {
    core.arena().consumeWork();
  }

  struct ParamDef {
    std::string name;
    std::string type;
  };
  std::vector<ParamDef> params;

  if (paramsCell != zigzag::noCell && core.arena().contains(paramsCell)) {
    if (const auto first = core.arena().linked(paramsCell, core.dims().grab,
                                               zigzag::DimVector::POS)) {
      for (auto p = first; p && core.arena().contains(*p);
           p      = core.arena().linked(*p, core.dims().step,
                                        zigzag::DimVector::POS)) {
        std::string name{core.arena().textOf(*p)};
        if (name.empty()) {
          name = meta.labelOf(*p).value_or("");
        }
        std::string type = "Cell";
        if (const auto tCell = core.arena().linked(*p, core.dims().values,
                                                   zigzag::DimVector::POS)) {
          std::string tTxt{core.arena().textOf(*tCell)};
          if (!tTxt.empty()) type = std::move(tTxt);
        }
        name = std::string{trimWhitespace(name)};
        type = std::string{trimWhitespace(type)};
        if (!name.empty()) {
          params.push_back({std::move(name), std::move(type)});
        }
      }
    } else {
      const std::string headTxt{core.arena().textOf(paramsCell)};
      const bool hasListSelf = !headTxt.empty() && headTxt != "List";

      const auto processElem = [&](zigzag::CellRef elem) {
        auto target = elem;
        if (const auto ref = core.arena().asInt64(elem);
            ref && core.arena().contains(static_cast<zigzag::CellRef>(*ref))) {
          target = static_cast<zigzag::CellRef>(*ref);
        }
        std::string txt{core.arena().textOf(target)};
        if (txt.empty() || txt == "List") return;

        std::string paramName;
        std::string paramType = "Cell";
        if (const auto colon = txt.find(':'); colon != std::string::npos) {
          paramName = txt.substr(0, colon);
          paramType = txt.substr(colon + 1);
        } else {
          paramName = txt;
          if (const auto tCell = core.arena().linked(target, core.dims().values,
                                                     zigzag::DimVector::POS)) {
            std::string tTxt{core.arena().textOf(*tCell)};
            if (!tTxt.empty()) paramType = std::move(tTxt);
          }
        }
        paramName = std::string{trimWhitespace(paramName)};
        paramType = std::string{trimWhitespace(paramType)};
        if (!paramName.empty()) {
          params.push_back({std::move(paramName), std::move(paramType)});
        }
      };

      if (hasListSelf) {
        for (const auto elem :
             zigzag::rank(core.arena(), paramsCell, core.dims().step)) {
          processElem(elem);
        }
      } else {
        for (const auto elem :
             zigzag::rankAfter(core.arena(), paramsCell, core.dims().step)) {
          processElem(elem);
        }
      }
    }
  }

  std::string paramsStr;
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i > 0) paramsStr += ", ";
    paramsStr += params[i].name + ": " + params[i].type;
  }

  std::string retSuffix;
  if (!resultType.empty()) {
    retSuffix = std::format(": {}", resultType);
  }

  std::string bodyWrapper;
  const std::string_view trimmed = trimWhitespace(srcText);
  if (trimmed.starts_with('{') && trimmed.ends_with('}')) {
    bodyWrapper = std::string{trimmed};
  } else {
    bodyWrapper = "{\n" + srcText + "\n}";
  }

  const std::string syntheticSource =
      std::format("module host\n"
                  "import std:meta\n"
                  "agent Host {{\n"
                  "  def fragment({}){} {}\n"
                  "}}\n",
                  paramsStr, retSuffix, bodyWrapper);

  auto artifacts = compileArtifacts(syntheticSource);
  if (!artifacts) {
    const std::string msg = artifacts.error().empty()
                                ? "compilation failed"
                                : artifacts.error().front().detail;
    return recordFault(msg);
  }

  const Decl *foundMethod = nullptr;
  for (const auto &decl : artifacts->program.declarations) {
    if (decl.kind == DeclKind::Agent && decl.name == "Host") {
      for (const auto &m : decl.members) {
        if (m.kind == DeclKind::Method && m.name == "fragment") {
          foundMethod = &m;
          break;
        }
      }
      break;
    }
  }

  if (!foundMethod || !foundMethod->body) {
    return recordFault("internal compiler error: synthetic method not found");
  }

  const bool isPure       = isPureExpr(*foundMethod->body);
  const Type parsedResult = foundMethod->result.value_or(Type{"()", {}});

  Expr literal;
  literal.kind  = ExprKind::Unit;
  literal.loc   = foundMethod->loc;
  literal.stamp = ScopeStamp{0, 0};

  const FragmentKey key{foundMethod->loc, 0};
  FragmentTable fragments;
  fragments.emplace(
      key, FragmentRecord{.body   = CodeBody{std::string{label},
                                             foundMethod->parameters,
                                             cloneExpr(*foundMethod->body)},
                          .closed = true});

  FragmentInfoMap infos;
  infos.emplace(key, FragmentInfo{.result = parsedResult,
                                  .pure   = isPure,
                                  .closed = true,
                                  .arity  = foundMethod->parameters.size()});

  FragmentContext context;
  context.fragments = &fragments;
  context.info      = &infos;
  context.module    = std::nullopt;
  context.bindings  = nullptr;

  Compiler compiler{core, vm};
  auto object = compileFragment(literal, context, compiler);
  if (!object) {
    return recordFault(object.error().detail);
  }

  const auto recipeDim = core.findOrMintDimension("d.recipe");
  for (auto cell =
           core.arena().linked(object->head, recipeDim, zigzag::DimVector::POS);
       cell && core.arena().contains(*cell);
       cell = core.arena().linked(*cell, core.dims().step,
                                  zigzag::DimVector::POS)) {
    core.arena().consumeWork();
  }

  auto reg = meta.registerCode(object->head);
  if (!reg) {
    return recordFault(reg.error());
  }

  return object->head;
}

} // namespace xanadu::vagent
