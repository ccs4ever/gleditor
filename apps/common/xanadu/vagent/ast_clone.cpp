#include "common/xanadu/vagent/ast_clone.hpp"

#include <memory>
#include <utility>

namespace xanadu::vagent {

ExprPtr cloneExpr(const Expr &expr) {
  auto copy        = std::make_unique<Expr>();
  copy->kind       = expr.kind;
  copy->text       = expr.text;
  copy->loc        = expr.loc;
  copy->names      = expr.names;
  copy->annotation = expr.annotation;
  copy->binding    = expr.binding;
  copy->stamp      = expr.stamp;
  copy->children.reserve(expr.children.size());
  for (const auto &child : expr.children)
    copy->children.push_back(cloneExpr(child));
  copy->scope      = cloneExpr(expr.scope);
  copy->recipients = cloneExpr(expr.recipients);
  copy->deadline   = cloneExpr(expr.deadline);
  copy->guard      = cloneExpr(expr.guard);
  copy->recovery   = cloneExpr(expr.recovery);
  return copy;
}

ExprPtr cloneExpr(const ExprPtr &expr) {
  return expr ? cloneExpr(*expr) : nullptr;
}

Decl cloneDecl(const Decl &decl) {
  Decl copy;
  copy.kind         = decl.kind;
  copy.name         = decl.name;
  copy.loc          = decl.loc;
  copy.parameters   = decl.parameters;
  copy.result       = decl.result;
  copy.names        = decl.names;
  copy.target       = decl.target;
  copy.goalKind     = decl.goalKind;
  copy.planBindings = decl.planBindings;
  copy.contracts.reserve(decl.contracts.size());
  for (const auto &contract : decl.contracts) {
    Contract cloned;
    cloned.postcondition = contract.postcondition;
    cloned.condition     = cloneExpr(contract.condition);
    cloned.loc           = contract.loc;
    copy.contracts.push_back(std::move(cloned));
  }
  copy.goalClauses.reserve(decl.goalClauses.size());
  for (const auto &clause : decl.goalClauses) {
    GoalClause cloned;
    cloned.kind      = clause.kind;
    cloned.condition = cloneExpr(clause.condition);
    cloned.loc       = clause.loc;
    copy.goalClauses.push_back(std::move(cloned));
  }
  copy.planArms.reserve(decl.planArms.size());
  for (const auto &arm : decl.planArms) {
    PlanArm cloned;
    cloned.pattern = cloneExpr(arm.pattern);
    cloned.guard   = cloneExpr(arm.guard);
    cloned.body    = cloneExpr(arm.body);
    cloned.loc     = arm.loc;
    copy.planArms.push_back(std::move(cloned));
  }
  copy.members.reserve(decl.members.size());
  for (const auto &member : decl.members)
    copy.members.push_back(cloneDecl(member));
  copy.body        = cloneExpr(decl.body);
  copy.guard       = cloneExpr(decl.guard);
  copy.initializer = cloneExpr(decl.initializer);
  copy.trigger     = cloneExpr(decl.trigger);
  copy.deadline    = cloneExpr(decl.deadline);
  copy.binding     = decl.binding;
  copy.priority    = decl.priority;
  copy.native      = decl.native;
  copy.effect      = decl.effect;
  copy.preempts    = decl.preempts;
  return copy;
}

Program cloneProgram(const Program &program) {
  Program copy;
  copy.module  = program.module;
  copy.imports = program.imports;
  copy.declarations.reserve(program.declarations.size());
  for (const auto &declaration : program.declarations)
    copy.declarations.push_back(cloneDecl(declaration));
  return copy;
}

} // namespace xanadu::vagent
