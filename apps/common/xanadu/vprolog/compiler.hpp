/**
 * @file compiler.hpp
 * @brief Translates Prolog AST into Vortex / Vlog hyperstructural entities.
 */
#ifndef COMMON_XANADU_VPROLOG_COMPILER_HPP
#define COMMON_XANADU_VPROLOG_COMPILER_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vprolog/ast.hpp"
#include "common/xanadu/zigzag/vlog.hpp"

namespace xanadu::vprolog {

struct CompiledQuery {
  std::vector<zigzag::CellRef> goals;
  std::vector<std::pair<std::string, zigzag::CellRef>> variables;
  zigzag::CellRef primaryGoal{zigzag::noCell};
};

class Compiler {
public:
  explicit Compiler(zigzag::vortex::VortexCore &core);

  /// Compiles all clauses in a Prolog program into the Vortex logic database.
  void compileProgram(const Program &program);

  /// Compiles a single fact or rule clause and adds it to the predicate
  /// database.
  zigzag::CellRef compileClause(const Clause &clause);

  /// Compiles a query clause and returns goal cells and variable bindings.
  CompiledQuery compileQuery(const Clause &queryClause);

  /// Compiles a query string (e.g. "?- append(X, Y, [a, b])." or "append(X, Y,
  /// [a, b]).").
  CompiledQuery compileQuery(std::string_view queryString);

  /// Lowers an AST Term into a Vortex cell, mapping variables within varMap.
  zigzag::CellRef
  lowerTerm(const Term &term,
            std::unordered_map<std::string, zigzag::CellRef> &varMap);

  /// Returns the predicate cell for @p functor (and optional arity), creating
  /// it if necessary.
  zigzag::CellRef getOrCreatePredicate(std::string_view functor,
                                       std::size_t arity = 0);

  /// Returns all custom predicate cells defined in this compiler session.
  [[nodiscard]] std::vector<zigzag::CellRef> customPredicates() const;

  /// Runs resolution for a compiled query, returning solutions.
  std::vector<zigzag::vortex::LogicSolution>
  solve(const CompiledQuery &query, std::size_t maxSolutions = 100);

  /// Solves once, returning true if at least one solution exists.
  bool solveOnce(const CompiledQuery &query);

  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return core_; }
  [[nodiscard]] zigzag::Vlog &vlog() noexcept { return vlog_; }
  [[nodiscard]] zigzag::vortex::VortexStdLib &stdlib() noexcept {
    return stdlib_;
  }

private:
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM vm_;
  zigzag::Vlog vlog_;
  zigzag::vortex::VortexStdLib stdlib_;
  std::unordered_map<std::string, zigzag::CellRef> predicates_;
  std::vector<zigzag::CellRef> predicateList_;
};

} // namespace xanadu::vprolog

#endif // COMMON_XANADU_VPROLOG_COMPILER_HPP
