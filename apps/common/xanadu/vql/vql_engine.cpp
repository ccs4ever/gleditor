/**
 * @file vql_engine.cpp
 * @brief Direct query evaluation engine for VQL v13.0.
 */
#include "common/xanadu/vql/vql_engine.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#include "common/xanadu/vql/lexer.hpp"
#include "common/xanadu/vql/parser.hpp"

namespace xanadu::vql {

namespace {

bool stringToDouble(std::string_view sv, double &out) {
  if (sv.empty()) {
    return false;
  }
  std::string s(sv);
  char *end = nullptr;
  out       = std::strtod(s.c_str(), &end);
  return end == s.c_str() + s.size();
}

[[maybe_unused]] bool stringToInt64(std::string_view sv, std::int64_t &out) {
  if (sv.empty()) {
    return false;
  }
  const char *begin = sv.data();
  const char *end   = sv.data() + sv.size();
  auto [ptr, ec]    = std::from_chars(begin, end, out);
  return ec == std::errc{} && ptr == end;
}

double cellValueToDouble(const zigzag::vortex::CellValue &val,
                         bool &convertible) {
  convertible = true;
  if (std::holds_alternative<double>(val)) {
    return std::get<double>(val);
  }
  if (std::holds_alternative<std::int64_t>(val)) {
    return static_cast<double>(std::get<std::int64_t>(val));
  }
  if (std::holds_alternative<bool>(val)) {
    return std::get<bool>(val) ? 1.0 : 0.0;
  }
  if (std::holds_alternative<std::string>(val)) {
    double d = 0.0;
    if (stringToDouble(std::get<std::string>(val), d)) {
      return d;
    }
  }
  convertible = false;
  return 0.0;
}

} // namespace

VQLEngine::VQLEngine(MultiStoreCoordinator &coordinator)
    : coordinator_(coordinator), core_(&coordinator.core()) {}

VQLEngine::VQLEngine(zigzag::vortex::VortexCore &core)
    : ownedCoordinator_(std::make_unique<MultiStoreCoordinator>(core)),
      coordinator_(*ownedCoordinator_), core_(&core) {}

VQLEngine::VQLEngine(zigzag::ArenaManifold &arena)
    : ownedCore_(std::make_unique<zigzag::vortex::VortexCore>(arena)),
      ownedCoordinator_(std::make_unique<MultiStoreCoordinator>(*ownedCore_)),
      coordinator_(*ownedCoordinator_), core_(ownedCore_.get()) {}

zigzag::DimRef VQLEngine::resolveDimension(std::string_view name) {
  return coordinator_.resolveDimension(name);
}

void VQLEngine::setVariable(std::string_view name, zigzag::CellRef value) {
  env_[std::string(name)] = {value};
}

void VQLEngine::setVariable(std::string_view name,
                            std::vector<zigzag::CellRef> values) {
  env_[std::string(name)] = std::move(values);
}

std::vector<zigzag::CellRef>
VQLEngine::getVariable(std::string_view name) const {
  auto it = env_.find(std::string(name));
  if (it != env_.end()) {
    return it->second;
  }
  return {};
}

void VQLEngine::clearVariables() noexcept { env_.clear(); }

std::vector<zigzag::CellRef> VQLEngine::execute(std::string_view queryString) {
  Parser parser(queryString);
  QueryExpression query = parser.parseQuery();
  return execute(query);
}

std::vector<zigzag::CellRef> VQLEngine::execute(const QueryExpression &query) {
  if (std::holds_alternative<PathExpression>(query.expr)) {
    return evaluatePath(std::get<PathExpression>(query.expr));
  }
  return executeBlock(std::get<ExecutionBlock>(query.expr));
}

std::vector<zigzag::CellRef>
VQLEngine::resolveAnchor(const AnchorNode &anchor,
                         const std::vector<zigzag::CellRef> &contextCells) {
  std::vector<zigzag::CellRef> results;

  switch (anchor.kind) {
  case AnchorKind::Home:
    results.push_back(coordinator_.homeAnchor());
    break;

  case AnchorKind::NamedStore: {
    zigzag::CellRef storeHome = coordinator_.resolveNamedStore(anchor.name);
    if (storeHome != zigzag::noCell) {
      results.push_back(storeHome);
    }
    break;
  }

  case AnchorKind::Root:
    results.push_back(coordinator_.homeAnchor());
    break;

  case AnchorKind::Cursor:
    if (core_->dims().cursors != zigzag::noCell) {
      results.push_back(core_->dims().cursors);
    } else {
      results.push_back(coordinator_.homeAnchor());
    }
    break;

  case AnchorKind::NamedCursor:
    results.push_back(coordinator_.homeAnchor());
    break;

  case AnchorKind::Variable: {
    auto it = env_.find(anchor.name);
    if (it != env_.end()) {
      results = it->second;
    }
    break;
  }

  case AnchorKind::LiteralCellId: {
    zigzag::CellRef ref = static_cast<zigzag::CellRef>(anchor.cellId);
    if (core_->arena().contains(ref)) {
      results.push_back(ref);
    }
    break;
  }

  case AnchorKind::Context:
    results = contextCells;
    break;

  case AnchorKind::Create: {
    zigzag::CellRef fresh = core_->arena().makeCell();
    if (anchor.createValue.has_value()) {
      const auto &cv = *anchor.createValue;
      if (cv.kind == CreateValue::Kind::Literal) {
        core_->value(fresh, 0, -1, cv.literal);
      } else if (cv.kind == CreateValue::Kind::Expression && cv.expr) {
        auto val = evaluateValueExpr(*cv.expr, zigzag::noCell);
        core_->value(fresh, 0, -1, val);
      }
    }
    results.push_back(fresh);
    break;
  }
  }

  if (anchor.derefMaster) {
    for (auto &c : results) {
      c = coordinator_.derefCloneMaster(c);
    }
  }

  return results;
}

std::vector<zigzag::CellRef>
VQLEngine::evaluatePath(const PathExpression &path,
                        const std::vector<zigzag::CellRef> &contextCells) {
  std::vector<zigzag::CellRef> current =
      resolveAnchor(path.anchor, contextCells);

  for (const auto &step : path.steps) {
    current = evaluateStep(step, current);
    if (current.empty()) {
      break;
    }
  }

  // Clone joins: leftmost operand is master
  if (path.cloneTail.has_value() && !current.empty()) {
    for (zigzag::CellRef master : current) {
      for (const auto &operand : path.cloneTail->operands) {
        if (operand.bareCreate.has_value()) {
          const auto &bc = *operand.bareCreate;
          zigzag::CellRef cloneCell =
              cellFromValue(bc.kind == CreateValue::Kind::Literal
                                ? zigzag::vortex::CellValue(bc.literal)
                                : zigzag::vortex::CellValue(""));
          // Link cloneCell as clone to master
          zigzag::CellRef tail = master;
          std::unordered_set<zigzag::CellRef> visited;
          while (true) {
            visited.insert(tail);
            zigzag::CellRef next =
                core_->arena().linked(tail, core_->dims().clone, false);
            if (next == zigzag::noCell || visited.contains(next)) {
              break;
            }
            tail = next;
          }
          core_->arena().link(tail, core_->dims().clone, false, cloneCell);
          core_->arena().link(cloneCell, core_->dims().clone, true, tail);
        } else if (operand.path != nullptr) {
          auto clones = evaluatePath(*operand.path, contextCells);
          for (zigzag::CellRef c : clones) {
            zigzag::CellRef tail = master;
            std::unordered_set<zigzag::CellRef> visited;
            while (true) {
              visited.insert(tail);
              zigzag::CellRef next =
                  core_->arena().linked(tail, core_->dims().clone, false);
              if (next == zigzag::noCell || visited.contains(next)) {
                break;
              }
              tail = next;
            }
            core_->arena().link(tail, core_->dims().clone, false, c);
            core_->arena().link(c, core_->dims().clone, true, tail);
          }
        }
      }
    }
  }

  return current;
}

std::vector<zigzag::CellRef>
VQLEngine::traverseDimension(const SignedDimensionStep &dimStep,
                             const std::vector<zigzag::CellRef> &inputs) {
  std::vector<zigzag::CellRef> results;
  zigzag::DimRef dim = resolveDimension(dimStep.dimName);
  bool negward       = (dimStep.direction < 0);

  for (zigzag::CellRef in : inputs) {
    if (!core_->arena().contains(in)) {
      continue;
    }

    switch (dimStep.placement) {
    case Placement::Default:
    case Placement::From: {
      // Walk outward from context in step's direction
      zigzag::CellRef curr = core_->arena().linked(in, dim, negward);
      std::unordered_set<zigzag::CellRef> visited{in};
      while (curr != zigzag::noCell && !visited.contains(curr)) {
        visited.insert(curr);
        results.push_back(curr);
        curr = core_->arena().linked(curr, dim, negward);
      }
      break;
    }

    case Placement::Rank: {
      // Seek to head (extreme negward), then stream to tail (extreme posward)
      zigzag::CellRef head = in;
      std::unordered_set<zigzag::CellRef> visitedNeg{in};
      while (true) {
        zigzag::CellRef prev =
            core_->arena().linked(head, dim, true /*negward*/);
        if (prev == zigzag::noCell || visitedNeg.contains(prev)) {
          break;
        }
        visitedNeg.insert(prev);
        head = prev;
      }

      std::vector<zigzag::CellRef> rankCells;
      zigzag::CellRef curr = head;
      std::unordered_set<zigzag::CellRef> visitedPos;
      while (curr != zigzag::noCell && !visitedPos.contains(curr)) {
        visitedPos.insert(curr);
        rankCells.push_back(curr);
        curr = core_->arena().linked(curr, dim, false /*posward*/);
      }

      if (negward) {
        std::reverse(rankCells.begin(), rankCells.end());
      }
      results.insert(results.end(), rankCells.begin(), rankCells.end());
      break;
    }

    case Placement::Head: {
      // Seek to extreme negward
      zigzag::CellRef head = in;
      std::unordered_set<zigzag::CellRef> visited{in};
      while (true) {
        zigzag::CellRef prev =
            core_->arena().linked(head, dim, true /*negward*/);
        if (prev == zigzag::noCell || visited.contains(prev)) {
          break;
        }
        visited.insert(prev);
        head = prev;
      }
      results.push_back(head);
      break;
    }

    case Placement::Tail: {
      // Seek to extreme posward
      zigzag::CellRef tail = in;
      std::unordered_set<zigzag::CellRef> visited{in};
      while (true) {
        zigzag::CellRef next =
            core_->arena().linked(tail, dim, false /*posward*/);
        if (next == zigzag::noCell || visited.contains(next)) {
          break;
        }
        visited.insert(next);
        tail = next;
      }
      results.push_back(tail);
      break;
    }
    }
  }

  return results;
}

std::vector<zigzag::CellRef>
VQLEngine::performCreates(const SignedDimensionStep &dimStep,
                          const std::vector<zigzag::CellRef> &inputs) {
  if (dimStep.placement == Placement::From ||
      dimStep.placement == Placement::Rank) {
    throw std::runtime_error(
        "::from and ::rank have no meaning on a create (Rule §4.1)");
  }

  std::vector<zigzag::CellRef> results;
  zigzag::DimRef dim = resolveDimension(dimStep.dimName);

  for (zigzag::CellRef in : inputs) {
    if (!core_->arena().contains(in)) {
      continue;
    }

    // Default create seeks to tail; ::head inserts at head
    bool insertAtHead           = (dimStep.placement == Placement::Head);
    zigzag::CellRef attachPoint = in;

    if (insertAtHead) {
      std::unordered_set<zigzag::CellRef> visited{in};
      while (true) {
        zigzag::CellRef prev =
            core_->arena().linked(attachPoint, dim, true /*negward*/);
        if (prev == zigzag::noCell || visited.contains(prev)) {
          break;
        }
        visited.insert(prev);
        attachPoint = prev;
      }
    } else {
      // Seek tail posward
      std::unordered_set<zigzag::CellRef> visited{in};
      while (true) {
        zigzag::CellRef next =
            core_->arena().linked(attachPoint, dim, false /*posward*/);
        if (next == zigzag::noCell || visited.contains(next)) {
          break;
        }
        visited.insert(next);
        attachPoint = next;
      }
    }

    std::vector<zigzag::CellRef> created;
    for (const auto &c : dimStep.creates) {
      zigzag::CellRef newC = zigzag::noCell;
      if (c.kind == CreateValue::Kind::Empty) {
        newC = core_->arena().makeCell();
      } else if (c.kind == CreateValue::Kind::Literal) {
        newC = core_->arena().makeCell(c.literal);
      } else if (c.kind == CreateValue::Kind::Expression && c.expr) {
        auto val = evaluateValueExpr(*c.expr, in);
        newC     = cellFromValue(val);
      }

      if (insertAtHead) {
        core_->arena().link(newC, dim, false /*posward*/, attachPoint);
        core_->arena().link(attachPoint, dim, true /*negward*/, newC);
        attachPoint = newC;
      } else {
        core_->arena().link(attachPoint, dim, false /*posward*/, newC);
        core_->arena().link(newC, dim, true /*negward*/, attachPoint);
        attachPoint = newC;
      }
      created.push_back(newC);
    }

    // Yield handling
    switch (dimStep.placement) {
    default:
      break;
    }

    // YieldMode on create
    // Handled by evaluateStep based on yieldMode
    results.insert(results.end(), created.begin(), created.end());
  }

  return results;
}

std::vector<zigzag::CellRef>
VQLEngine::evaluateStep(const PathStep &step,
                        const std::vector<zigzag::CellRef> &currentCells) {
  if (currentCells.empty()) {
    return {};
  }

  std::vector<zigzag::CellRef> stepOutput;

  if (std::holds_alternative<SignedDimensionStep>(step.selector)) {
    const auto &s = std::get<SignedDimensionStep>(step.selector);
    if (s.creates.empty()) {
      stepOutput = traverseDimension(s, currentCells);
    } else {
      stepOutput = performCreates(s, currentCells);
      // Yield mode mapping on creates:
      if (step.yieldMode == YieldMode::Last) {
        if (!stepOutput.empty()) {
          stepOutput = {stepOutput.back()};
        }
      } else if (step.yieldMode == YieldMode::Both) {
        std::vector<zigzag::CellRef> combined = currentCells;
        combined.insert(combined.end(), stepOutput.begin(), stepOutput.end());
        stepOutput = std::move(combined);
      } else if (step.yieldMode == YieldMode::Keep) {
        stepOutput = currentCells;
      }
    }
  } else if (std::holds_alternative<std::shared_ptr<MacroDimensionGroup>>(
                 step.selector)) {
    const auto &mg =
        *std::get<std::shared_ptr<MacroDimensionGroup>>(step.selector);
    auto runOnce = [&](std::vector<zigzag::CellRef> in) {
      for (const auto &innerStep : mg.steps) {
        in = evaluateStep(innerStep, in);
      }
      return in;
    };

    if (mg.repetition == Repetition::Once) {
      stepOutput = runOnce(currentCells);
    } else if (mg.repetition == Repetition::ZeroOrOne) {
      auto res   = runOnce(currentCells);
      stepOutput = res.empty() ? currentCells : res;
    } else if (mg.repetition == Repetition::ZeroOrMore ||
               mg.repetition == Repetition::OneOrMore) {
      std::vector<zigzag::CellRef> cur = currentCells;
      if (mg.repetition == Repetition::OneOrMore) {
        cur = runOnce(cur);
      }
      int iters = 0;
      while (iters++ < 1000) {
        auto next = runOnce(cur);
        if (next == cur || next.empty()) {
          break;
        }
        cur = std::move(next);
      }
      stepOutput = cur;
    }
  } else if (std::holds_alternative<FunctionInvocation>(step.selector)) {
    const auto &fn = std::get<FunctionInvocation>(step.selector);
    if (fn.name == "value") {
      std::int64_t offset = 0;
      std::int64_t length = -1;
      if (fn.args.size() >= 2) {
        auto oVal = evaluateValueExpr(fn.args[1], currentCells[0]);
        if (std::holds_alternative<std::int64_t>(oVal)) {
          offset = std::get<std::int64_t>(oVal);
        }
      }
      if (fn.args.size() >= 3) {
        auto lVal = evaluateValueExpr(fn.args[2], currentCells[0]);
        if (std::holds_alternative<std::int64_t>(lVal)) {
          length = std::get<std::int64_t>(lVal);
        }
      }
      for (zigzag::CellRef c : currentCells) {
        auto res = core_->value(c, offset, length);
        if (res.has_value()) {
          stepOutput.push_back(*res);
        }
      }
    } else if (fn.name == "count") {
      zigzag::CellRef countCell = core_->arena().makeScalarCell(
          static_cast<std::int64_t>(currentCells.size()));
      stepOutput.push_back(countCell);
    } else if (fn.name == "link" && fn.args.size() >= 2) {
      // Existing-Target Fan-Out (§4.7)
      // Resolve dimension and direction from arg 0
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
        if (!p.steps.empty() &&
            std::holds_alternative<SignedDimensionStep>(p.steps[0].selector)) {
          const auto &sd = std::get<SignedDimensionStep>(p.steps[0].selector);
          dimName        = sd.dimName;
          direction      = sd.direction;
        }
      }

      zigzag::DimRef dim = coordinator_.resolveDimension(dimName);

      // Resolve target cell from arg 1
      zigzag::CellRef targetCell = zigzag::noCell;
      if (std::holds_alternative<std::string>(fn.args[1].kind)) {
        const auto &varName = std::get<std::string>(fn.args[1].kind);
        auto it             = env_.find(varName);
        if (it != env_.end() && !it->second.empty()) {
          targetCell = it->second[0];
        }
      } else if (std::holds_alternative<std::shared_ptr<PathExpression>>(
                     fn.args[1].kind)) {
        auto targets = evaluatePath(
            *std::get<std::shared_ptr<PathExpression>>(fn.args[1].kind), {});
        if (!targets.empty()) {
          targetCell = targets[0];
        }
      } else if (std::holds_alternative<ScalarLiteral>(fn.args[1].kind)) {
        const auto &lit = std::get<ScalarLiteral>(fn.args[1].kind);
        targetCell      = cellFromValue(lit.value);
      }

      if (targetCell != zigzag::noCell) {
        auto gen = core_->cloneGenerator(targetCell);
        std::vector<zigzag::CellRef> attachedClones;
        bool negward = (direction < 0);
        for (zigzag::CellRef c : currentCells) {
          zigzag::CellRef cloneCell = gen();
          core_->link(c, dim, negward, cloneCell);
          attachedClones.push_back(cloneCell);
        }

        if (step.yieldMode == YieldMode::Both) {
          std::vector<zigzag::CellRef> combined = currentCells;
          combined.insert(combined.end(), attachedClones.begin(),
                          attachedClones.end());
          stepOutput = std::move(combined);
        } else if (step.yieldMode == YieldMode::Keep) {
          stepOutput = currentCells;
        } else if (step.yieldMode == YieldMode::Last) {
          if (!attachedClones.empty()) {
            stepOutput = {attachedClones.back()};
          }
        } else {
          stepOutput = attachedClones;
        }
      }
    }
  }

  // Master Dereference >
  if (step.derefMaster) {
    for (auto &c : stepOutput) {
      c = coordinator_.derefCloneMaster(c);
    }
  }

  // Predicates Filtering
  if (!step.predicates.empty()) {
    std::vector<zigzag::CellRef> filtered;
    for (zigzag::CellRef c : stepOutput) {
      bool allPassed = true;
      for (const auto &pred : step.predicates) {
        if (!evaluatePredicate(pred, c)) {
          allPassed = false;
          break;
        }
      }
      if (allPassed) {
        filtered.push_back(c);
      }
    }
    stepOutput = std::move(filtered);
  }

  // Range Clamp [start, end]
  if (step.rangeClamp.has_value() && !stepOutput.empty()) {
    const auto &clamp  = *step.rangeClamp;
    std::int64_t total = static_cast<std::int64_t>(stepOutput.size());
    std::int64_t s     = clamp.start;
    std::int64_t e     = clamp.end.value_or(s);

    std::int64_t from = (s < 0) ? (total + s) : (s - 1);
    std::int64_t to   = (e < 0) ? (total + e) : (e - 1);

    from = std::clamp<std::int64_t>(from, 0, total - 1);
    to   = std::clamp<std::int64_t>(to, 0, total - 1);

    if (from <= to) {
      stepOutput = std::vector<zigzag::CellRef>(stepOutput.begin() + from,
                                                stepOutput.begin() + to + 1);
    } else {
      stepOutput.clear();
    }
  }

  if (step.yieldMode == YieldMode::Keep) {
    return currentCells;
  }

  return stepOutput;
}

zigzag::CellRef VQLEngine::cellFromValue(const zigzag::vortex::CellValue &val) {
  if (std::holds_alternative<double>(val)) {
    return core_->arena().makeScalarCell(std::get<double>(val));
  }
  if (std::holds_alternative<std::int64_t>(val)) {
    return core_->arena().makeScalarCell(std::get<std::int64_t>(val));
  }
  if (std::holds_alternative<bool>(val)) {
    return core_->arena().makeScalarCell(std::get<bool>(val));
  }
  return core_->arena().makeCell(std::get<std::string>(val));
}

zigzag::vortex::CellValue
VQLEngine::evaluateValueExpr(const ValueExpr &expr, zigzag::CellRef context) {
  if (std::holds_alternative<ScalarLiteral>(expr.kind)) {
    const auto &lit = std::get<ScalarLiteral>(expr.kind);
    if (std::holds_alternative<std::string>(lit.value)) {
      return std::get<std::string>(lit.value);
    }
    if (std::holds_alternative<double>(lit.value)) {
      return std::get<double>(lit.value);
    }
    if (std::holds_alternative<std::int64_t>(lit.value)) {
      return std::get<std::int64_t>(lit.value);
    }
    if (std::holds_alternative<bool>(lit.value)) {
      return std::get<bool>(lit.value);
    }
  }

  if (std::holds_alternative<std::string>(expr.kind)) {
    const auto &varName = std::get<std::string>(expr.kind);
    auto it             = env_.find(varName);
    if (it != env_.end() && !it->second.empty()) {
      return core_->render(it->second[0]);
    }
    return "";
  }

  if (std::holds_alternative<std::shared_ptr<PathExpression>>(expr.kind)) {
    const auto &path = *std::get<std::shared_ptr<PathExpression>>(expr.kind);
    auto cells = evaluatePath(path, context != zigzag::noCell
                                        ? std::vector<zigzag::CellRef>{context}
                                        : std::vector<zigzag::CellRef>{});
    if (!cells.empty()) {
      return core_->render(cells[0]);
    }
    return "";
  }

  if (std::holds_alternative<std::shared_ptr<FunctionInvocation>>(expr.kind)) {
    const auto &fn = *std::get<std::shared_ptr<FunctionInvocation>>(expr.kind);
    if (fn.name == "count") {
      if (!fn.args.empty()) {
        if (std::holds_alternative<std::shared_ptr<PathExpression>>(
                fn.args[0].kind)) {
          const auto &path =
              *std::get<std::shared_ptr<PathExpression>>(fn.args[0].kind);
          auto cells =
              evaluatePath(path, context != zigzag::noCell
                                     ? std::vector<zigzag::CellRef>{context}
                                     : std::vector<zigzag::CellRef>{});
          return static_cast<std::int64_t>(cells.size());
        }
      }
    }
  }

  return "";
}

bool VQLEngine::compareValues(const zigzag::vortex::CellValue &left, CompOp op,
                              const zigzag::vortex::CellValue &right) {
  // Numeric comparison check
  bool leftNum  = false;
  bool rightNum = false;
  double lD     = cellValueToDouble(left, leftNum);
  double rD     = cellValueToDouble(right, rightNum);

  if (leftNum && rightNum) {
    switch (op) {
    case CompOp::Equal:
      return std::abs(lD - rD) < 1e-9;
    case CompOp::NotEqual:
      return std::abs(lD - rD) >= 1e-9;
    case CompOp::LessThan:
      return lD < rD;
    case CompOp::GreaterThan:
      return lD > rD;
    case CompOp::LessEqual:
      return lD <= rD;
    case CompOp::GreaterEqual:
      return lD >= rD;
    }
  }

  // String comparison fallback
  std::string lStr = std::holds_alternative<std::string>(left)
                         ? std::get<std::string>(left)
                         : (leftNum ? std::to_string(lD) : "");
  std::string rStr = std::holds_alternative<std::string>(right)
                         ? std::get<std::string>(right)
                         : (rightNum ? std::to_string(rD) : "");

  switch (op) {
  case CompOp::Equal:
    return lStr == rStr;
  case CompOp::NotEqual:
    return lStr != rStr;
  case CompOp::LessThan:
    return lStr < rStr;
  case CompOp::GreaterThan:
    return lStr > rStr;
  case CompOp::LessEqual:
    return lStr <= rStr;
  case CompOp::GreaterEqual:
    return lStr >= rStr;
  }
  return false;
}

bool VQLEngine::evaluateFactor(const BooleanFactor &factor,
                               zigzag::CellRef context) {
  bool result = false;

  if (std::holds_alternative<ComparisonExpr>(factor.test)) {
    const auto &cmp = std::get<ComparisonExpr>(factor.test);
    auto leftVal    = evaluateValueExpr(*cmp.left, context);
    auto rightVal   = evaluateValueExpr(*cmp.right, context);
    result          = compareValues(leftVal, cmp.op, rightVal);
  } else {
    const auto &test = std::get<PredicateTest>(factor.test);
    if (std::holds_alternative<std::shared_ptr<PathExpression>>(test.kind)) {
      const auto &path = *std::get<std::shared_ptr<PathExpression>>(test.kind);
      auto cells =
          evaluatePath(path, context != zigzag::noCell
                                 ? std::vector<zigzag::CellRef>{context}
                                 : std::vector<zigzag::CellRef>{});
      result = !cells.empty();
    } else if (std::holds_alternative<bool>(test.kind)) {
      result = (context != zigzag::noCell) ? core_->isTruthy(context) : false;
    } else if (std::holds_alternative<std::shared_ptr<ValueExpr>>(test.kind)) {
      auto val = evaluateValueExpr(
          *std::get<std::shared_ptr<ValueExpr>>(test.kind), context);
      result = zigzag::vortex::VortexCore::evaluateTruthiness(val);
    }
  }

  return factor.negated ? !result : result;
}

bool VQLEngine::evaluateTerm(const BooleanTerm &term, zigzag::CellRef context) {
  for (const auto &factor : term.factors) {
    if (!evaluateFactor(factor, context)) {
      return false; // Short-circuit AND
    }
  }
  return true;
}

bool VQLEngine::evaluatePredicate(const BooleanExpr &expr,
                                  zigzag::CellRef context) {
  for (const auto &term : expr.terms) {
    if (evaluateTerm(term, context)) {
      return true; // Short-circuit OR
    }
  }
  return false;
}

std::vector<zigzag::CellRef>
VQLEngine::executeBlock(const ExecutionBlock &block) {
  std::vector<zigzag::CellRef> results;
  evaluateBindings(block, 0, results);
  return results;
}

void VQLEngine::evaluateBindings(
    const ExecutionBlock &block, std::size_t bindingIndex,
    std::vector<zigzag::CellRef> &accumulatedResults) {
  if (bindingIndex >= block.bindings.size()) {
    // All bindings completed, test where clause
    if (block.where.has_value()) {
      if (!evaluatePredicate(block.where->condition, zigzag::noCell)) {
        return;
      }
    }

    executeActionClause(block.action, accumulatedResults);
    return;
  }

  const auto &binding = block.bindings[bindingIndex];
  if (std::holds_alternative<ForClause>(binding)) {
    const auto &fc = std::get<ForClause>(binding);
    auto inCells   = evaluatePath(fc.inPath);
    for (zigzag::CellRef c : inCells) {
      env_[fc.varName] = {c};
      evaluateBindings(block, bindingIndex + 1, accumulatedResults);
    }
  } else {
    const auto &lc = std::get<LetClause>(binding);
    if (std::holds_alternative<PathExpression>(lc.target)) {
      env_[lc.varName] = evaluatePath(std::get<PathExpression>(lc.target));
    } else {
      auto val =
          evaluateValueExpr(std::get<ValueExpr>(lc.target), zigzag::noCell);
      env_[lc.varName] = {cellFromValue(val)};
    }
    evaluateBindings(block, bindingIndex + 1, accumulatedResults);
  }
}

void VQLEngine::executeActionClause(
    const ActionClause &action,
    std::vector<zigzag::CellRef> &accumulatedResults) {
  if (std::holds_alternative<ReturnClause>(action.clause)) {
    const auto &ret = std::get<ReturnClause>(action.clause);
    for (const auto &item : ret.items) {
      if (std::holds_alternative<PathExpression>(item.item)) {
        auto res = evaluatePath(std::get<PathExpression>(item.item));
        accumulatedResults.insert(accumulatedResults.end(), res.begin(),
                                  res.end());
      } else if (std::holds_alternative<FieldWeave>(item.item)) {
        const auto &fw             = std::get<FieldWeave>(item.item);
        zigzag::CellRef resultCell = core_->arena().makeCell();
        std::vector<zigzag::CellRef> cur{resultCell};
        for (const auto &step : fw.steps) {
          cur = evaluateStep(step, cur);
        }
        accumulatedResults.push_back(resultCell);
      }
    }
  } else if (std::holds_alternative<EffectClause>(action.clause)) {
    executeEffectClause(std::get<EffectClause>(action.clause));
  } else if (std::holds_alternative<ConditionalClause>(action.clause)) {
    const auto &cond = std::get<ConditionalClause>(action.clause);
    if (evaluatePredicate(cond.condition, zigzag::noCell)) {
      if (cond.thenClause) {
        executeActionClause(*cond.thenClause, accumulatedResults);
      }
    } else {
      if (cond.elseClause) {
        executeActionClause(*cond.elseClause, accumulatedResults);
      }
    }
  }
}

void VQLEngine::executeEffectClause(const EffectClause &eff) {
  for (const auto &item : eff.items) {
    if (std::holds_alternative<PathExpression>(item.item)) {
      evaluatePath(std::get<PathExpression>(item.item));
    } else if (std::holds_alternative<LetClause>(item.item)) {
      const auto &lc = std::get<LetClause>(item.item);
      if (std::holds_alternative<PathExpression>(lc.target)) {
        env_[lc.varName] = evaluatePath(std::get<PathExpression>(lc.target));
      } else {
        auto val =
            evaluateValueExpr(std::get<ValueExpr>(lc.target), zigzag::noCell);
        env_[lc.varName] = {cellFromValue(val)};
      }
    } else {
      const auto &p =
          std::get<std::pair<ForClause, std::shared_ptr<EffectClause>>>(
              item.item);
      auto inCells = evaluatePath(p.first.inPath);
      for (zigzag::CellRef c : inCells) {
        env_[p.first.varName] = {c};
        if (p.second) {
          executeEffectClause(*p.second);
        }
      }
    }
  }
}

} // namespace xanadu::vql
