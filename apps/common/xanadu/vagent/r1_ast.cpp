#include "common/xanadu/vagent/r1_ast.hpp"

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/records.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::vagent {

namespace {

using zigzag::CellRef;
using zigzag::noCell;
using zigzag::Posward;

std::optional<Type> parseTypeString(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  if (s.empty()) return std::nullopt;
  if (s == "()") return Type{"()", {}};
  if (s.front() == '[' && s.back() == ']') {
    auto inner = parseTypeString(s.substr(1, s.size() - 2));
    if (!inner) return std::nullopt;
    return Type{"List", {std::move(*inner)}};
  }
  const auto less = s.find('<');
  if (less != std::string_view::npos && s.back() == '>') {
    std::string base(s.substr(0, less));
    std::string_view argsStr = s.substr(less + 1, s.size() - less - 2);
    std::vector<Type> args;
    std::size_t start = 0;
    int depth         = 0;
    for (std::size_t i = 0; i <= argsStr.size(); ++i) {
      if (i == argsStr.size() || (argsStr[i] == ',' && depth == 0)) {
        std::string_view arg = argsStr.substr(start, i - start);
        auto parsedArg       = parseTypeString(arg);
        if (!parsedArg) return std::nullopt;
        args.push_back(std::move(*parsedArg));
        start = i + 1;
      } else if (argsStr[i] == '<' || argsStr[i] == '[') {
        depth++;
      } else if (argsStr[i] == '>' || argsStr[i] == ']') {
        depth--;
      }
    }
    return Type{std::move(base), std::move(args)};
  }
  return Type{std::string(s), {}};
}

} // namespace

void ensureR1AstVocabulary(zigzag::ArenaManifold &arena,
                           zigzag::vortex::VortexCore &core,
                           zigzag::CellRef programRoot) {
  if (!arena.contains(programRoot)) return;
  const auto astKindsDim = core.findOrMintDimension("d.ast.kinds");
  if (!zigzag::rankAfter(arena, programRoot, astKindsDim).empty()) {
    return;
  }
  CellRef tail = programRoot;
  for (const auto kindStr : kR1AstKinds) {
    const auto cell = arena.makeCell(kindStr);
    zigzag::expectWritten(arena.link(tail, astKindsDim, Posward, cell));
    tail = cell;
  }
}

std::expected<zigzag::CellRef, std::string>
marshalAstToR1(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
               const Expr &expr) {
  std::string_view kindTag;
  switch (expr.kind) {
  case ExprKind::Integer:
    kindTag = "lit.int";
    break;
  case ExprKind::Float:
    kindTag = "lit.float";
    break;
  case ExprKind::Boolean:
    kindTag = "lit.bool";
    break;
  case ExprKind::String:
    kindTag = "lit.str";
    break;
  case ExprKind::Unit:
  case ExprKind::None:
    kindTag = "lit.unit";
    break;
  case ExprKind::Identifier:
  case ExprKind::LogicVariable:
    kindTag = "ident";
    break;
  case ExprKind::Unary:
    kindTag = "unary";
    break;
  case ExprKind::Binary:
    kindTag = (expr.text == ":=") ? "assign" : "binary";
    break;
  case ExprKind::Call:
    kindTag = "call";
    break;
  case ExprKind::Field:
    kindTag = "field";
    break;
  case ExprKind::Let:
    kindTag = "let";
    break;
  case ExprKind::If:
    kindTag = "if";
    break;
  case ExprKind::Block:
    kindTag = "block";
    break;
  case ExprKind::List:
    kindTag = "list";
    break;
  case ExprKind::Cells:
    kindTag = "cells";
    break;
  case ExprKind::Believes:
    kindTag = "believes";
    break;
  case ExprKind::Code:
    kindTag = "code";
    break;
  case ExprKind::Record:
    kindTag = "record";
    break;
  case ExprKind::Term:
    kindTag = "term";
    break;
  case ExprKind::While:
    kindTag = "while";
    break;
  case ExprKind::For:
    kindTag = "for";
    break;
  case ExprKind::Return:
    kindTag = "return";
    break;
  case ExprKind::Fail:
    kindTag = "fail";
    break;
  case ExprKind::Yield:
    kindTag = "yield";
    break;
  case ExprKind::Consume:
    kindTag = "consume";
    break;
  case ExprKind::Emit:
    kindTag = "emit";
    break;
  case ExprKind::Tell:
    kindTag = "tell";
    break;
  case ExprKind::Untell:
    kindTag = "untell";
    break;
  case ExprKind::Believe:
    kindTag = "believe";
    break;
  case ExprKind::Forget:
    kindTag = "forget";
    break;
  case ExprKind::Revise:
    kindTag = "revise";
    break;
  case ExprKind::Spawn:
    kindTag = "spawn";
    break;
  case ExprKind::Adopt:
    kindTag = "adopt";
    break;
  case ExprKind::Achieve:
    kindTag = "achieve";
    break;
  case ExprKind::Ask:
    kindTag = "ask";
    break;
  case ExprKind::AwaitTerm:
    kindTag = "await.term";
    break;
  case ExprKind::AwaitEvent:
    kindTag = "await.event";
    break;
  case ExprKind::Drop:
    kindTag = "drop";
    break;
  case ExprKind::Install:
    kindTag = "install";
    break;
  case ExprKind::Uninstall:
    kindTag = "uninstall";
    break;
  case ExprKind::Mount:
    kindTag = "mount";
    break;
  case ExprKind::Unmount:
    kindTag = "unmount";
    break;
  case ExprKind::Stop:
    kindTag = "stop";
    break;
  case ExprKind::Terminate:
    kindTag = "terminate";
    break;
  case ExprKind::Splice:
    kindTag = "splice";
    break;
  case ExprKind::Static:
    kindTag = "static";
    break;
  case ExprKind::Match:
    kindTag = "match";
    break;
  case ExprKind::MatchArm:
    kindTag = "match.arm";
    break;
  case ExprKind::SafeNav:
    kindTag = "safenav";
    break;
  case ExprKind::Coalesce:
    kindTag = "coalesce";
    break;
  case ExprKind::Map:
    kindTag = "map";
    break;
  case ExprKind::Tuple:
    kindTag = "tuple";
    break;
  case ExprKind::TuplePattern:
    kindTag = "pattern.tuple";
    break;
  case ExprKind::ListPattern:
    kindTag = "pattern.list";
    break;
  case ExprKind::RecordPattern:
    kindTag = "pattern.record";
    break;
  case ExprKind::WildcardPattern:
    kindTag = "pattern.wildcard";
    break;
  case ExprKind::CtorPattern:
    kindTag = "pattern.ctor";
    break;
  case ExprKind::MicroVersionLiteral:
    kindTag = "lit.microversion";
    break;
  default:
    return std::unexpected(
        std::format("marshalAstToR1: unsupported ExprKind {}",
                    static_cast<int>(expr.kind)));
  }

  const auto node = arena.makeCell(kindTag);
  zigzag::expectWritten(
      arena.link(node, core.dims().name, Posward, arena.makeCell(expr.text)));

  if (expr.annotation) {
    zigzag::expectWritten(
        arena.link(node, core.dims().values, Posward,
                   arena.makeCell(typeName(*expr.annotation))));
  }

  if (expr.loc.line > 0) {
    if (const auto r = varsRecord(arena, core, node, "line", true)) {
      zigzag::expectWritten(arena.link(
          *r, core.dims().values, Posward,
          arena.makeScalarCell(static_cast<std::int64_t>(expr.loc.line))));
    }
  }
  if (expr.loc.column > 0) {
    if (const auto r = varsRecord(arena, core, node, "col", true)) {
      zigzag::expectWritten(arena.link(
          *r, core.dims().values, Posward,
          arena.makeScalarCell(static_cast<std::int64_t>(expr.loc.column))));
    }
  }

  if (expr.guard) {
    if (const auto r = varsRecord(arena, core, node, "guard", true)) {
      if (auto gRes = marshalAstToR1(arena, core, *expr.guard)) {
        zigzag::expectWritten(
            arena.link(*r, core.dims().values, Posward, *gRes));
      }
    }
  }
  if (expr.recovery) {
    if (const auto r = varsRecord(arena, core, node, "recovery", true)) {
      if (auto recRes = marshalAstToR1(arena, core, *expr.recovery)) {
        zigzag::expectWritten(
            arena.link(*r, core.dims().values, Posward, *recRes));
      }
    }
  }

  CellRef prevChild = noCell;
  for (std::size_t i = 0; i < expr.children.size(); ++i) {
    if (!expr.children[i]) continue;
    auto childRes = marshalAstToR1(arena, core, *expr.children[i]);
    if (!childRes) return std::unexpected(childRes.error());
    const CellRef child = *childRes;
    if (i == 0) {
      zigzag::expectWritten(arena.link(node, core.dims().grab, Posward, child));
    } else {
      zigzag::expectWritten(
          arena.link(prevChild, core.dims().step, Posward, child));
    }
    prevChild = child;
  }

  return node;
}

std::expected<ExprPtr, std::string>
unmarshalR1ToAst(const zigzag::ArenaManifold &arena,
                 const zigzag::vortex::VortexCore &core, zigzag::CellRef root) {
  if (!arena.contains(root)) {
    return std::unexpected("unmarshalR1ToAst: root is not a valid arena cell");
  }

  const auto kindTag = arena.textOf(root);
  ExprKind kind      = ExprKind::Unit;
  if (kindTag == "lit.int") {
    kind = ExprKind::Integer;
  } else if (kindTag == "lit.float") {
    kind = ExprKind::Float;
  } else if (kindTag == "lit.bool") {
    kind = ExprKind::Boolean;
  } else if (kindTag == "lit.str") {
    kind = ExprKind::String;
  } else if (kindTag == "lit.unit") {
    kind = ExprKind::Unit;
  } else if (kindTag == "ident") {
    kind = ExprKind::Identifier;
  } else if (kindTag == "unary") {
    kind = ExprKind::Unary;
  } else if (kindTag == "binary") {
    kind = ExprKind::Binary;
  } else if (kindTag == "call") {
    kind = ExprKind::Call;
  } else if (kindTag == "field") {
    kind = ExprKind::Field;
  } else if (kindTag == "let") {
    kind = ExprKind::Let;
  } else if (kindTag == "assign") {
    kind = ExprKind::Binary;
  } else if (kindTag == "if") {
    kind = ExprKind::If;
  } else if (kindTag == "block") {
    kind = ExprKind::Block;
  } else if (kindTag == "list") {
    kind = ExprKind::List;
  } else if (kindTag == "cells") {
    kind = ExprKind::Cells;
  } else if (kindTag == "believes") {
    kind = ExprKind::Believes;
  } else if (kindTag == "code") {
    kind = ExprKind::Code;
  } else if (kindTag == "record") {
    kind = ExprKind::Record;
  } else if (kindTag == "term") {
    kind = ExprKind::Term;
  } else if (kindTag == "while") {
    kind = ExprKind::While;
  } else if (kindTag == "for") {
    kind = ExprKind::For;
  } else if (kindTag == "return") {
    kind = ExprKind::Return;
  } else if (kindTag == "fail") {
    kind = ExprKind::Fail;
  } else if (kindTag == "yield") {
    kind = ExprKind::Yield;
  } else if (kindTag == "consume") {
    kind = ExprKind::Consume;
  } else if (kindTag == "emit") {
    kind = ExprKind::Emit;
  } else if (kindTag == "tell") {
    kind = ExprKind::Tell;
  } else if (kindTag == "untell") {
    kind = ExprKind::Untell;
  } else if (kindTag == "believe") {
    kind = ExprKind::Believe;
  } else if (kindTag == "forget") {
    kind = ExprKind::Forget;
  } else if (kindTag == "revise") {
    kind = ExprKind::Revise;
  } else if (kindTag == "spawn") {
    kind = ExprKind::Spawn;
  } else if (kindTag == "adopt") {
    kind = ExprKind::Adopt;
  } else if (kindTag == "achieve") {
    kind = ExprKind::Achieve;
  } else if (kindTag == "ask") {
    kind = ExprKind::Ask;
  } else if (kindTag == "await.term") {
    kind = ExprKind::AwaitTerm;
  } else if (kindTag == "await.event") {
    kind = ExprKind::AwaitEvent;
  } else if (kindTag == "drop") {
    kind = ExprKind::Drop;
  } else if (kindTag == "install") {
    kind = ExprKind::Install;
  } else if (kindTag == "uninstall") {
    kind = ExprKind::Uninstall;
  } else if (kindTag == "mount") {
    kind = ExprKind::Mount;
  } else if (kindTag == "unmount") {
    kind = ExprKind::Unmount;
  } else if (kindTag == "stop") {
    kind = ExprKind::Stop;
  } else if (kindTag == "terminate") {
    kind = ExprKind::Terminate;
  } else if (kindTag == "splice") {
    kind = ExprKind::Splice;
  } else if (kindTag == "static") {
    kind = ExprKind::Static;
  } else if (kindTag == "match") {
    kind = ExprKind::Match;
  } else if (kindTag == "match.arm") {
    kind = ExprKind::MatchArm;
  } else if (kindTag == "safenav") {
    kind = ExprKind::SafeNav;
  } else if (kindTag == "coalesce") {
    kind = ExprKind::Coalesce;
  } else if (kindTag == "map") {
    kind = ExprKind::Map;
  } else if (kindTag == "tuple") {
    kind = ExprKind::Tuple;
  } else if (kindTag == "pattern.tuple") {
    kind = ExprKind::TuplePattern;
  } else if (kindTag == "pattern.list") {
    kind = ExprKind::ListPattern;
  } else if (kindTag == "pattern.record") {
    kind = ExprKind::RecordPattern;
  } else if (kindTag == "pattern.wildcard") {
    kind = ExprKind::WildcardPattern;
  } else if (kindTag == "pattern.ctor") {
    kind = ExprKind::CtorPattern;
  } else if (kindTag == "lit.microversion") {
    kind = ExprKind::MicroVersionLiteral;
  } else {
    return std::unexpected(std::format(
        "unmarshalR1ToAst: unrecognized R1 AST kind tag '{}'", kindTag));
  }

  auto expr  = std::make_unique<Expr>();
  expr->kind = kind;
  if (kindTag == "assign") {
    expr->text = ":=";
  }
  if (const auto nCell = arena.linked(root, core.dims().name, Posward)) {
    const auto t = arena.textOf(*nCell);
    if (!t.empty() || kindTag != "assign") {
      expr->text = t;
    }
  }

  if (const auto vCell = arena.linked(root, core.dims().values, Posward)) {
    const auto tStr = arena.textOf(*vCell);
    if (!tStr.empty()) {
      expr->annotation = parseTypeString(tStr);
    }
  }

  for (const auto rec : zigzag::rankAfter(arena, root, core.dims().vars)) {
    const auto k = arena.textOf(rec);
    if (k == "line") {
      if (const auto v = arena.linked(rec, core.dims().values, Posward)) {
        if (const auto lineVal = arena.asInt64(*v)) {
          expr->loc.line = static_cast<std::uint32_t>(*lineVal);
        }
      }
    } else if (k == "col") {
      if (const auto v = arena.linked(rec, core.dims().values, Posward)) {
        if (const auto colVal = arena.asInt64(*v)) {
          expr->loc.column = static_cast<std::uint32_t>(*colVal);
        }
      }
    } else if (k == "guard") {
      if (const auto v = arena.linked(rec, core.dims().values, Posward)) {
        if (auto g = unmarshalR1ToAst(arena, core, *v)) {
          expr->guard = std::move(*g);
        }
      }
    } else if (k == "recovery") {
      if (const auto v = arena.linked(rec, core.dims().values, Posward)) {
        if (auto recRes = unmarshalR1ToAst(arena, core, *v)) {
          expr->recovery = std::move(*recRes);
        }
      }
    }
  }

  if (const auto firstChild = arena.linked(root, core.dims().grab, Posward)) {
    for (const auto childCell :
         zigzag::rank(arena, *firstChild, core.dims().step)) {
      auto childRes = unmarshalR1ToAst(arena, core, childCell);
      if (!childRes) return std::unexpected(childRes.error());
      expr->children.push_back(std::move(*childRes));
    }
  }

  return expr;
}

std::expected<zigzag::CellRef, std::string>
marshalDeclToR1(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                const Decl &decl) {
  std::string_view kindTag;
  switch (decl.kind) {
  case DeclKind::Agent:
    kindTag = "decl.agent";
    break;
  case DeclKind::Event:
    kindTag = "decl.event";
    break;
  case DeclKind::Plan:
    kindTag = "decl.plan";
    break;
  case DeclKind::Belief:
    kindTag = "decl.belief";
    break;
  case DeclKind::Method:
    kindTag = "decl.fn";
    break;
  case DeclKind::Field:
    kindTag = "decl.field";
    break;
  default:
    kindTag = "decl.event";
    break;
  }

  const auto node = arena.makeCell(kindTag);
  zigzag::expectWritten(
      arena.link(node, core.dims().name, Posward, arena.makeCell(decl.name)));

  if (decl.result) {
    zigzag::expectWritten(arena.link(node, core.dims().values, Posward,
                                     arena.makeCell(typeName(*decl.result))));
  }

  if (decl.loc.line > 0) {
    if (const auto r = varsRecord(arena, core, node, "line", true)) {
      zigzag::expectWritten(arena.link(
          *r, core.dims().values, Posward,
          arena.makeScalarCell(static_cast<std::int64_t>(decl.loc.line))));
    }
  }

  CellRef prevChild      = noCell;
  const auto attachChild = [&](CellRef child) {
    if (prevChild == noCell) {
      zigzag::expectWritten(arena.link(node, core.dims().grab, Posward, child));
    } else {
      zigzag::expectWritten(
          arena.link(prevChild, core.dims().step, Posward, child));
    }
    prevChild = child;
  };

  for (const auto &param : decl.parameters) {
    const auto pCell = arena.makeCell("param");
    zigzag::expectWritten(arena.link(pCell, core.dims().name, Posward,
                                     arena.makeCell(param.name)));
    zigzag::expectWritten(arena.link(pCell, core.dims().values, Posward,
                                     arena.makeCell(typeName(param.type))));
    attachChild(pCell);
  }

  for (const auto &member : decl.members) {
    auto mRes = marshalDeclToR1(arena, core, member);
    if (!mRes) return mRes;
    attachChild(*mRes);
  }

  for (const auto &arm : decl.planArms) {
    const auto armCell = arena.makeCell("plan.arm");
    if (arm.pattern) {
      auto pRes = marshalAstToR1(arena, core, *arm.pattern);
      if (pRes) {
        zigzag::expectWritten(
            arena.link(armCell, core.dims().name, Posward, *pRes));
      }
    }
    if (arm.guard) {
      auto gRes = marshalAstToR1(arena, core, *arm.guard);
      if (gRes) {
        zigzag::expectWritten(
            arena.link(armCell, core.dims().values, Posward, *gRes));
      }
    }
    if (arm.body) {
      auto bRes = marshalAstToR1(arena, core, *arm.body);
      if (bRes) {
        zigzag::expectWritten(
            arena.link(armCell, core.dims().grab, Posward, *bRes));
      }
    }
    attachChild(armCell);
  }

  if (decl.body) {
    auto bRes = marshalAstToR1(arena, core, *decl.body);
    if (!bRes) return bRes;
    attachChild(*bRes);
  }

  return node;
}

std::expected<zigzag::CellRef, std::string>
marshalProgramToR1(zigzag::ArenaManifold &arena,
                   zigzag::vortex::VortexCore &core, const Program &program) {
  const auto root = arena.makeCell("program");
  if (program.module) {
    zigzag::expectWritten(arena.link(root, core.dims().name, Posward,
                                     arena.makeCell(*program.module)));
  }

  CellRef prevChild      = noCell;
  const auto attachChild = [&](CellRef child) {
    if (prevChild == noCell) {
      zigzag::expectWritten(arena.link(root, core.dims().grab, Posward, child));
    } else {
      zigzag::expectWritten(
          arena.link(prevChild, core.dims().step, Posward, child));
    }
    prevChild = child;
  };

  for (const auto &imp : program.imports) {
    const auto impCell = arena.makeCell("import");
    zigzag::expectWritten(arena.link(impCell, core.dims().name, Posward,
                                     arena.makeCell(imp.path)));
    attachChild(impCell);
  }

  for (const auto &decl : program.declarations) {
    auto dRes = marshalDeclToR1(arena, core, decl);
    if (!dRes) return dRes;
    attachChild(*dRes);
  }

  return root;
}

std::expected<Decl, std::string>
unmarshalR1ToDecl(const zigzag::ArenaManifold &arena,
                  const zigzag::vortex::VortexCore &core,
                  zigzag::CellRef root) {
  if (!arena.contains(root)) {
    return std::unexpected("unmarshalR1ToDecl: invalid cell");
  }
  const auto kindTag = arena.textOf(root);
  Decl decl;
  if (kindTag == "decl.agent") {
    decl.kind = DeclKind::Agent;
  } else if (kindTag == "decl.event") {
    decl.kind = DeclKind::Event;
  } else if (kindTag == "decl.plan") {
    decl.kind = DeclKind::Plan;
  } else if (kindTag == "decl.belief") {
    decl.kind = DeclKind::Belief;
  } else if (kindTag == "decl.fn") {
    decl.kind = DeclKind::Method;
  } else if (kindTag == "decl.field") {
    decl.kind = DeclKind::Field;
  }

  if (const auto n = arena.linked(root, core.dims().name, Posward)) {
    decl.name = arena.textOf(*n);
  }
  if (const auto v = arena.linked(root, core.dims().values, Posward)) {
    decl.result = parseTypeString(arena.textOf(*v));
  }

  if (const auto firstChild = arena.linked(root, core.dims().grab, Posward)) {
    for (const auto childCell :
         zigzag::rank(arena, *firstChild, core.dims().step)) {
      const auto tag = arena.textOf(childCell);
      if (tag == "param") {
        Parameter param;
        if (const auto pn =
                arena.linked(childCell, core.dims().name, Posward)) {
          param.name = arena.textOf(*pn);
        }
        if (const auto pv =
                arena.linked(childCell, core.dims().values, Posward)) {
          param.type =
              parseTypeString(arena.textOf(*pv)).value_or(Type{"()", {}});
        }
        decl.parameters.push_back(std::move(param));
      } else if (tag == "plan.arm") {
        PlanArm arm;
        if (const auto pn =
                arena.linked(childCell, core.dims().name, Posward)) {
          if (auto p = unmarshalR1ToAst(arena, core, *pn)) {
            arm.pattern = std::move(*p);
          }
        }
        if (const auto gv =
                arena.linked(childCell, core.dims().values, Posward)) {
          if (auto g = unmarshalR1ToAst(arena, core, *gv)) {
            arm.guard = std::move(*g);
          }
        }
        if (const auto bg =
                arena.linked(childCell, core.dims().grab, Posward)) {
          if (auto b = unmarshalR1ToAst(arena, core, *bg)) {
            arm.body = std::move(*b);
          }
        }
        decl.planArms.push_back(std::move(arm));
      } else if (tag.starts_with("decl.")) {
        auto mRes = unmarshalR1ToDecl(arena, core, childCell);
        if (!mRes) return mRes;
        decl.members.push_back(std::move(*mRes));
      } else {
        auto eRes = unmarshalR1ToAst(arena, core, childCell);
        if (eRes) {
          decl.body = std::move(*eRes);
        }
      }
    }
  }

  return decl;
}

std::expected<Program, std::string>
unmarshalR1ToProgram(const zigzag::ArenaManifold &arena,
                     const zigzag::vortex::VortexCore &core,
                     zigzag::CellRef root) {
  if (!arena.contains(root)) {
    return std::unexpected("unmarshalR1ToProgram: invalid cell");
  }
  Program program;
  if (const auto n = arena.linked(root, core.dims().name, Posward)) {
    program.module = arena.textOf(*n);
  }

  if (const auto firstChild = arena.linked(root, core.dims().grab, Posward)) {
    for (const auto childCell :
         zigzag::rank(arena, *firstChild, core.dims().step)) {
      const auto tag = arena.textOf(childCell);
      if (tag == "import") {
        Import imp;
        if (const auto in =
                arena.linked(childCell, core.dims().name, Posward)) {
          imp.path = arena.textOf(*in);
        }
        program.imports.push_back(std::move(imp));
      } else if (tag.starts_with("decl.")) {
        auto dRes = unmarshalR1ToDecl(arena, core, childCell);
        if (!dRes) return std::unexpected(dRes.error());
        program.declarations.push_back(std::move(*dRes));
      }
    }
  }

  return program;
}

} // namespace xanadu::vagent
