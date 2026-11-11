#include "common/xanadu/vagent/inlang_checker.hpp"

#include <format>
#include <set>
#include <string>
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

bool isReservedParamName(std::string_view name) {
  return name == "result" || name == "produced" || name == "pure" ||
         name == "closed" || name == "arity";
}

bool isUnimportedMetaReserved(std::string_view name) {
  static const std::set<std::string_view> kMetaReserved = {
      "reify",        "validate",    "register",        "register_all",
      "registration", "dim_named",   "as_body",         "head_of",
      "var_of",       "make_cell",   "join_batch",      "build_fragment",
      "boot_writer",  "boot_linker", "compile_fragment"};
  return kMetaReserved.contains(name);
}

bool isNumericType(std::string_view t) { return t == "Int" || t == "Float"; }

bool isComparisonOp(std::string_view op) {
  return op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" ||
         op == "!=";
}

bool isArithmeticOp(std::string_view op) {
  return op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
}

} // namespace

InlangCheckResult
runInlangChecker(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                 zigzag::vortex::VortexVM & /*vm*/, MetaRuntime & /*meta*/,
                 zigzag::CellRef r1Root, std::optional<zigzag::CellRef> params,
                 bool importStdMeta) {
  InlangCheckResult result;
  if (!arena.contains(r1Root)) {
    result.error = "invalid R1 AST root cell";
    return result;
  }

  const auto &dims = core.dims();
  std::map<std::string, std::string> env;
  std::size_t arity = 0;

  // Pass 1: Parameter validation & environment setup
  if (params.has_value() && *params != noCell && arena.contains(*params)) {
    const auto pVal = *params;
    std::vector<CellRef> paramCells;
    if (const auto grabChild = arena.linked(pVal, dims.grab, Posward)) {
      paramCells.push_back(*grabChild);
      for (const auto c : zigzag::rankAfter(arena, *grabChild, dims.step)) {
        paramCells.push_back(c);
      }
    } else {
      paramCells.push_back(pVal);
      for (const auto c : zigzag::rankAfter(arena, pVal, dims.step)) {
        paramCells.push_back(c);
      }
    }

    arity = paramCells.size();
    for (const auto p : paramCells) {
      const auto pName = arena.textOf(p);
      if (isReservedParamName(pName)) {
        result.error = std::format(
            "parameter name '{}' is reserved descriptor key", pName);
        return result;
      }
      if (!importStdMeta && isUnimportedMetaReserved(pName)) {
        result.error =
            std::format("unimported std:meta identifier '{}' reserved", pName);
        return result;
      }

      std::string pType = "Cell";
      if (const auto valCell = arena.linked(p, dims.values, Posward)) {
        pType = arena.textOf(*valCell);
      }
      env[pName] = pType;
    }
  }

  bool isPure              = true;
  bool isClosed            = true;
  std::size_t captureCount = 0;

  // Pass 2 & 3: Recursive type inference, condition validation, purity &
  // closure
  const auto inferNode = [&](auto &self, CellRef node,
                             bool inCond) -> std::string {
    if (!arena.contains(node)) return "";
    const auto kind = arena.textOf(node);
    const auto tag  = arena.linked(node, dims.name, Posward)
                          .transform([&](CellRef c) { return arena.textOf(c); })
                          .value_or("");

    const auto setType = [&](std::string_view t) {
      zigzag::expectWritten(
          arena.link(node, dims.values, Posward, arena.makeCell(t)));
    };

    if (kind == "lit.int") {
      if (inCond) return "";
      setType("Int");
      return "Int";
    }
    if (kind == "lit.float") {
      if (inCond) return "";
      setType("Float");
      return "Float";
    }
    if (kind == "lit.bool") {
      // In condition position, bare bool literal is barred per
      // condition-position rules
      if (inCond) return "";
      setType("Bool");
      return "Bool";
    }
    if (kind == "lit.str") {
      if (inCond) return "";
      setType("Str");
      return "Str";
    }
    if (kind == "lit.unit") {
      if (inCond) return "";
      setType("()");
      return "()";
    }
    if (kind == "ident") {
      if (!importStdMeta && isUnimportedMetaReserved(tag)) {
        return "";
      }
      auto it = env.find(tag);
      if (it != env.end()) {
        if (inCond && it->second == "Bool") {
          // bare boolean variable in condition position
          return "";
        }
        setType(it->second);
        return it->second;
      }
      // Unresolved variable -> capture
      isClosed = false;
      captureCount++;
      const auto capName = std::format("cap${}", captureCount);
      const auto rec     = varsRecord(arena, core, node, "capture", true);
      if (rec) {
        zigzag::expectWritten(
            arena.link(*rec, dims.values, Posward, arena.makeCell(capName)));
      }
      setType("Cell");
      return "Cell";
    }
    if (kind == "unary") {
      const auto operand = arena.linked(node, dims.grab, Posward);
      if (!operand) return "";
      const auto tArg = self(self, *operand, false);
      if (tArg.empty()) return "";
      if (tag == "-") {
        if (!isNumericType(tArg)) return "";
        setType(tArg);
        return tArg;
      }
      if (tag == "not" || tag == "!") {
        if (tArg != "Bool") return "";
        setType("Bool");
        return "Bool";
      }
      return "";
    }
    if (kind == "binary") {
      const auto left = arena.linked(node, dims.grab, Posward);
      if (!left) return "";
      const auto right = arena.linked(*left, dims.step, Posward);
      if (!right) return "";

      const auto tLeft  = self(self, *left, false);
      const auto tRight = self(self, *right, false);
      if (tLeft.empty() || tRight.empty()) return "";

      if (isArithmeticOp(tag)) {
        if ((!isNumericType(tLeft) && tLeft != "Cell") ||
            (!isNumericType(tRight) && tRight != "Cell"))
          return "";
        const auto resType = (tLeft != "Cell") ? tLeft : tRight;
        setType(resType);
        return resType;
      }
      if (isComparisonOp(tag)) {
        if (tLeft != tRight && tLeft != "Cell" && tRight != "Cell") return "";
        setType("Bool");
        return "Bool";
      }
      if (tag == "&" || tag == "|" || tag == "&&" || tag == "||") {
        if ((tLeft != "Bool" && tLeft != "Cell") ||
            (tRight != "Bool" && tRight != "Cell"))
          return "";
        setType("Bool");
        return "Bool";
      }
      return "";
    }
    if (kind == "if") {
      const auto cond = arena.linked(node, dims.grab, Posward);
      if (!cond) return "";
      const auto thenBranch = arena.linked(*cond, dims.step, Posward);
      if (!thenBranch) return "";
      const auto elseBranch = arena.linked(*thenBranch, dims.step, Posward);
      if (!elseBranch) return "";

      const auto tCond = self(self, *cond, true);
      if (tCond != "Bool") return "";
      const auto tThen = self(self, *thenBranch, false);
      const auto tElse = self(self, *elseBranch, false);
      if (tThen.empty() ||
          (tThen != tElse && tThen != "Cell" && tElse != "Cell"))
        return "";

      const auto resType = (tThen != "Cell") ? tThen : tElse;
      setType(resType);
      return resType;
    }
    if (kind == "let") {
      const auto val = arena.linked(node, dims.grab, Posward);
      if (!val) return "";
      const auto tVal = self(self, *val, false);
      if (tVal.empty()) return "";
      if (!tag.empty()) {
        env[tag] = tVal;
      }
      setType("()");
      return "()";
    }
    if (kind == "call") {
      std::string calleeName = tag;
      const auto calleeCell  = arena.linked(node, dims.grab, Posward);
      if (calleeCell) {
        if (const auto n = arena.linked(*calleeCell, dims.name, Posward)) {
          calleeName = arena.textOf(*n);
        }
      }
      if (!importStdMeta && isUnimportedMetaReserved(calleeName)) {
        return "";
      }
      // Calls may mutate or be impure
      if (calleeName == "make_cell" || calleeName == "boot_writer" ||
          calleeName == "boot_linker" || calleeName == "join_batch" ||
          calleeName == "build_fragment" || calleeName == "compile_fragment") {
        if (!importStdMeta) return "";
        if (calleeName == "make_cell" || calleeName == "join_batch")
          isPure = false;
        if (calleeName == "make_cell") {
          setType("Cell");
          return "Cell";
        }
        if (calleeName == "boot_writer" || calleeName == "boot_linker") {
          setType("Code<Cell>");
          return "Code<Cell>";
        }
        if (calleeName == "join_batch") {
          setType("Bool");
          return "Bool";
        }
        if (calleeName == "build_fragment") {
          setType("Opt<Cell>");
          return "Opt<Cell>";
        }
      }
      if (calleeName == "str_len") {
        setType("Int");
        return "Int";
      }
      if (calleeName == "int_to_str") {
        setType("Str");
        return "Str";
      }
      if (calleeName == "str_slice") {
        setType("Str");
        return "Str";
      }
      if (calleeName == "byte_at") {
        setType("Opt<Int>");
        return "Opt<Int>";
      }
      if (calleeName == "str_to_int") {
        setType("Opt<Int>");
        return "Opt<Int>";
      }
      return "";
    }

    return "";
  };

  const auto rootType = inferNode(inferNode, r1Root, false);
  if (rootType.empty()) {
    result.error = "type checking or condition-position validation failed";
    return result;
  }

  // Annotate descriptor fields on R1 root
  const auto addVar = [&](std::string_view key, CellRef val) {
    const auto rec = varsRecord(arena, core, r1Root, key, true);
    if (rec) {
      zigzag::expectWritten(arena.link(*rec, dims.values, Posward, val));
    }
  };

  addVar("result_type", arena.makeCell(rootType));
  addVar("pure",
         arena.makeScalarCell(isPure ? std::int64_t{1} : std::int64_t{0}));
  addVar("closed",
         arena.makeScalarCell(isClosed ? std::int64_t{1} : std::int64_t{0}));
  addVar("arity", arena.makeScalarCell(static_cast<std::int64_t>(arity)));

  result.ok         = true;
  result.resultType = rootType;
  result.pure       = isPure;
  result.closed     = isClosed;
  result.arity      = arity;
  return result;
}

} // namespace xanadu::vagent
