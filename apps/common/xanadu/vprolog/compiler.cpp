/**
 * @file compiler.cpp
 * @brief Translates Prolog AST into Vortex / Vlog hyperstructural entities.
 */
#include "common/xanadu/vprolog/compiler.hpp"

#include <algorithm>
#include <cctype>
#include <functional>

#include "common/xanadu/vprolog/parser.hpp"

namespace xanadu::vprolog {

Compiler::Compiler(zigzag::vortex::VortexCore &core)
    : core_(core), vm_(core), vlog_{.m     = core.arena(),
                                    .clone = core.dims().clone,
                                    .grab  = core.dims().grab,
                                    .step  = core.dims().step,
                                    .vars  = core.dims().vars},
      stdlib_(core, vm_) {}

zigzag::CellRef Compiler::getOrCreatePredicate(std::string_view functor,
                                               std::size_t /* arity */) {
  const std::string name(functor);
  auto it = predicates_.find(name);
  if (it != predicates_.end()) {
    return it->second;
  }
  zigzag::CellRef predCell = core_.arena().makeCell(functor);
  predicates_[name]        = predCell;
  predicateList_.push_back(predCell);
  return predCell;
}

std::vector<zigzag::CellRef> Compiler::customPredicates() const {
  return predicateList_;
}

zigzag::CellRef
Compiler::lowerTerm(const Term &term,
                    std::unordered_map<std::string, zigzag::CellRef> &varMap) {
  if (const auto *var = term.asVar()) {
    if (var->isAnonymous) {
      return vlog_.makeVar();
    }
    auto it = varMap.find(var->name);
    if (it != varMap.end()) {
      return it->second;
    }
    zigzag::CellRef freshVar = vlog_.makeVar();
    zigzag::CellRef nameCell = core_.arena().makeCell(var->name);
    core_.arena().link(freshVar, core_.dims().name, false, nameCell);
    varMap[var->name] = freshVar;
    return freshVar;
  }

  if (const auto *atom = term.asAtom()) {
    return core_.arena().makeCell(atom->name);
  }

  if (const auto *num = term.asNumber()) {
    if (num->isFloat()) {
      return core_.arena().makeScalarCell(num->asFloat());
    }
    return core_.arena().makeScalarCell(num->asInt());
  }

  if (const auto *str = term.asString()) {
    return core_.arena().makeCell(str->value);
  }

  if (term.isList()) {
    return lowerTerm(term.canonicalizeList(), varMap);
  }

  if (const auto *comp = term.asCompound()) {
    std::vector<zigzag::CellRef> loweredArgs;
    loweredArgs.reserve(comp->args.size());
    for (const auto &arg : comp->args) {
      loweredArgs.push_back(lowerTerm(arg, varMap));
    }
    return vlog_.makeTerm(comp->functor, loweredArgs);
  }

  return zigzag::noCell;
}

zigzag::CellRef Compiler::compileClause(const Clause &clause) {
  if (clause.isDirective || clause.isQuery) {
    return zigzag::noCell;
  }

  std::string functor;
  std::size_t arity = 0;

  if (const auto *comp = clause.head.asCompound()) {
    functor = comp->functor;
    arity   = comp->args.size();
  } else if (const auto *atom = clause.head.asAtom()) {
    functor = atom->name;
    arity   = 0;
  } else {
    return zigzag::noCell;
  }

  zigzag::CellRef predCell = getOrCreatePredicate(functor, arity);

  std::unordered_map<std::string, zigzag::CellRef> varMap;
  zigzag::CellRef headCell = lowerTerm(clause.head, varMap);

  std::vector<zigzag::CellRef> bodyGoals;
  bodyGoals.reserve(clause.body.size());
  for (const auto &b : clause.body) {
    bodyGoals.push_back(lowerTerm(b, varMap));
  }

  zigzag::CellRef clauseCell = core_.arena().makeCell();
  core_.arena().link(clauseCell, core_.dims().grab, false, headCell);

  zigzag::CellRef prevGoal = zigzag::noCell;
  for (zigzag::CellRef goal : bodyGoals) {
    if (prevGoal == zigzag::noCell) {
      core_.arena().link(clauseCell, core_.dims().spin, false, goal);
    } else {
      core_.arena().link(prevGoal, core_.dims().spin, false, goal);
    }
    prevGoal = goal;
  }

  zigzag::CellRef end = vlog_.endOfRank(predCell, core_.dims().clause, false);
  core_.arena().link(end, core_.dims().clause, false, clauseCell);

  return clauseCell;
}

void Compiler::compileProgram(const Program &program) {
  for (const auto &clause : program.clauses) {
    compileClause(clause);
  }
}

CompiledQuery Compiler::compileQuery(const Clause &queryClause) {
  CompiledQuery result;
  std::unordered_map<std::string, zigzag::CellRef> varMap;
  std::vector<std::string> varNames;

  std::vector<Term> queryTerms;
  if (queryClause.isQuery) {
    queryTerms = queryClause.body;
  } else {
    if (!queryClause.head.isAtom() || queryClause.head.asAtom()->name != "[]") {
      queryTerms.push_back(queryClause.head);
    }
    for (const auto &g : queryClause.body) {
      queryTerms.push_back(g);
    }
  }

  std::function<void(const Term &)> collectNames = [&](const Term &t) {
    if (const auto *v = t.asVar()) {
      if (!v->isAnonymous && std::find(varNames.begin(), varNames.end(),
                                       v->name) == varNames.end()) {
        varNames.push_back(v->name);
      }
    } else if (const auto *c = t.asCompound()) {
      for (const auto &arg : c->args) {
        collectNames(arg);
      }
    } else if (const auto *l = t.asList()) {
      for (const auto &elem : l->elements) {
        collectNames(elem);
      }
      if (l->tail) {
        collectNames(*l->tail);
      }
    }
  };

  for (const auto &term : queryTerms) {
    collectNames(term);
    zigzag::CellRef goalCell = lowerTerm(term, varMap);
    result.goals.push_back(goalCell);
  }

  if (!result.goals.empty()) {
    result.primaryGoal = result.goals[0];
  }

  for (const auto &name : varNames) {
    auto it = varMap.find(name);
    if (it != varMap.end()) {
      result.variables.push_back({name, it->second});
    }
  }

  return result;
}

CompiledQuery Compiler::compileQuery(std::string_view queryString) {
  std::string s(queryString);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                        s.back() == '\r' || s.back() == '\n')) {
    s.pop_back();
  }
  if (!s.empty() && s.back() != '.') {
    s.push_back('.');
  }
  std::size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t' ||
                              s[start] == '\r' || s[start] == '\n')) {
    start++;
  }
  if (start < s.size() &&
      (s.compare(start, 2, "?-") != 0 && s.compare(start, 2, ":-") != 0)) {
    s = "?- " + s.substr(start);
  } else if (start > 0) {
    s = s.substr(start);
  }

  Parser parser(s);
  Clause cl = parser.parseClause();
  return compileQuery(cl);
}

std::vector<zigzag::vortex::LogicSolution>
Compiler::solve(const CompiledQuery &query, std::size_t maxSolutions) {
  if (query.goals.empty()) {
    return {};
  }
  return stdlib_.solveQuery(query.goals, customPredicates(), maxSolutions);
}

bool Compiler::solveOnce(const CompiledQuery &query) {
  if (query.goals.empty()) {
    return false;
  }
  return stdlib_.solveOnce(query.goals, customPredicates());
}

} // namespace xanadu::vprolog
