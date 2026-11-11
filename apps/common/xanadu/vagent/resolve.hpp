#ifndef COMMON_XANADU_VAGENT_RESOLVE_HPP
#define COMMON_XANADU_VAGENT_RESOLVE_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"

namespace xanadu::vagent {

struct ResolveLimits {
  // Lookups, scope copies and minted bindings — the same accounting role as
  // CheckLimits::maxResolutionWork, budgeted separately so resolve can be
  // bounded on its own when the pipeline calls it.
  std::size_t maxResolutionWork{1'000'000};
  // Public-AST callers do not necessarily pass through the parser's limits.
  std::size_t maxExpressionDepth{256};
  std::size_t maxDeclarationDepth{256};
};

// The env snapshot at one code"…" literal's position — the bindings its
// body resolves against once expand parses it, plus the owner declaration
// member/goal lookups read. Keyed exactly like the fragment table: the
// literal's SourceLocation plus the fragment ordinal its ScopeStamp
// carries.
struct LiteralSeed {
  ScopeStamp stamp{};
  const Decl *owner{};
  std::map<std::string, ScopeStamp, std::less<>> bindings;
};

// What resolve() leaves for expand's per-literal seeding — one record per
// code"…" literal the env walk visited — and the mint watermarks every
// seeded body resolution continues from, so a body's fresh scope ids and
// fragment ordinals can never collide with the program's or each other's.
struct LiteralSeeds {
  std::map<FragmentKey, LiteralSeed> byLiteral;
  std::uint64_t nextScope{0};
  std::uint64_t nextOrdinal{0};
};

// The §5.6 resolve-only pass (design/vagent-metaprogramming.md): the
// checker's environment construction extracted into a standalone walk so the
// expand stage shares one scope truth instead of duplicating it. The walk
// visits every expression the checker's infer() would — implicit names,
// owner parameters and fields, declaration parameters, plan bindings,
// handler occurrence/as-bindings, block and branch environment copies,
// let/term/await/for bindings and the recovery `cause` — and writes each
// visited Expr's ScopeStamp: `scope` names the scope the node resolved in
// (the enclosing scope for most kinds; the binding's owning scope for an
// Identifier or LogicVariable, so the pair (scope, ordinal) is the binding's
// identity and two bindings in one scope never share a stamp). A name the
// environment does not bind keeps a scope-only stamp — name errors stay the
// checker's to report; resolve is not a second diagnostic authority. A
// code"…" literal's stamp carries a program-wide fragment ordinal instead
// of a binding ordinal, and when `seeds` is given its env is snapshotted
// into `seeds->byLiteral` under the same FragmentKey the fragment table
// uses — the input resolveFragment resumes from.
//
// This is an extraction, not a checker rewrite: check() keeps its own env
// walk for now and the stamps are what expand's cap$N aliasing, and the
// checker's later stamp-honoring rule, consume.
[[nodiscard]] std::expected<void, std::vector<Diagnostic>>
resolve(Program &program, LiteralSeeds *seeds = nullptr,
        ResolveLimits limits = {});

// The seeded subtree entry — expand's per-literal call once a code"…"
// literal's body is parsed. The body walks under a fresh scope minted from
// `shared`'s id space and pre-bound with the seed's env snapshot, so a free
// name resolves to the same binding identity it would have had written in
// place at the literal, and bindings minted inside the body get scope ids
// the program never issued. Nested code"…" literals record their own
// seeds into `shared->byLiteral` for the recursion to pick up.
[[nodiscard]] std::expected<void, std::vector<Diagnostic>>
resolveFragment(Program &program, Expr &body, const LiteralSeed &seed,
                LiteralSeeds &shared, ResolveLimits limits = {});

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_RESOLVE_HPP
