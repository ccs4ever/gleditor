#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent {

inline constexpr std::string_view kR1AstKinds[] = {
    "lit.int",
    "lit.float",
    "lit.bool",
    "lit.str",
    "lit.unit",
    "ident",
    "unary",
    "binary",
    "call",
    "field",
    "let",
    "assign",
    "if",
    "block",
    "list",
    "cells",
    "believes",
    "code",
    "program",
    "import",
    "decl.agent",
    "decl.event",
    "decl.plan",
    "decl.belief",
    "decl.fn",
    "decl.field",
    "param",
    "match",
    "match.arm",
    "safenav",
    "coalesce",
    "map",
    "tuple",
    "pattern.tuple",
    "pattern.list",
    "pattern.record",
    "pattern.wildcard",
    "pattern.ctor",
    "lit.microversion",
    "plan.arm",
};

/// Files the canonical R1 AST kind strings on `programRoot +d.ast.kinds`.
void ensureR1AstVocabulary(zigzag::ArenaManifold &arena,
                           zigzag::vortex::VortexCore &core,
                           zigzag::CellRef programRoot);

/// Marshals a parsed VAgent `Expr` into the 2-degree-safe R1 AST cell schema:
/// - Node cell text = canonical kind tag (`"lit.int"`, `"binary"`, etc.)
/// - `node +d.name` -> token/operator/identifier text cell
/// - `node +d.grab` -> first child, siblings chained on `+d.step`
/// - `node +d.values` -> optional fresh type-annotation cell
/// - `node +d.vars` -> optional `"line"` and `"col"` source coordinates
[[nodiscard]] std::expected<zigzag::CellRef, std::string>
marshalAstToR1(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
               const Expr &expr);

/// Marshals a parsed VAgent `Decl` into the R1 AST cell schema.
[[nodiscard]] std::expected<zigzag::CellRef, std::string>
marshalDeclToR1(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                const Decl &decl);

/// Marshals a full parsed VAgent `Program` into the R1 AST cell schema.
[[nodiscard]] std::expected<zigzag::CellRef, std::string>
marshalProgramToR1(zigzag::ArenaManifold &arena,
                   zigzag::vortex::VortexCore &core, const Program &program);

/// Unmarshals a 2-degree R1 AST cell tree back into an `ExprPtr` for round-trip
/// verification and host interop.
[[nodiscard]] std::expected<ExprPtr, std::string>
unmarshalR1ToAst(const zigzag::ArenaManifold &arena,
                 const zigzag::vortex::VortexCore &core, zigzag::CellRef root);

/// Unmarshals a 2-degree R1 AST declaration cell back into a `Decl`.
[[nodiscard]] std::expected<Decl, std::string>
unmarshalR1ToDecl(const zigzag::ArenaManifold &arena,
                  const zigzag::vortex::VortexCore &core, zigzag::CellRef root);

/// Unmarshals a 2-degree R1 AST program root cell back into a `Program`.
[[nodiscard]] std::expected<Program, std::string>
unmarshalR1ToProgram(const zigzag::ArenaManifold &arena,
                     const zigzag::vortex::VortexCore &core,
                     zigzag::CellRef root);

} // namespace xanadu::vagent
