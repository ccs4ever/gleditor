#ifndef COMMON_XANADU_VAGENT_AST_BRIDGE_HPP
#define COMMON_XANADU_VAGENT_AST_BRIDGE_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/runtime/ast.hpp"
#include "common/xanadu/vagent/token.hpp"

namespace zigzag::vortex {
class VortexCore;
}

namespace xanadu::vagent {

namespace runtime {
class Compiler;
struct CompiledAgent;
struct CompileFailure;
} // namespace runtime

// ============================================================================
// Frontend AST -> Runtime AST Lowering
// ============================================================================

/// Lowers a frontend Type representation to a runtime TypeExpr.
[[nodiscard]] runtime::TypeExpr lowerType(const Type &type);

/// Lowers a frontend Term expression (ExprKind::Term / Call / Identifier) to a
/// runtime Term.
[[nodiscard]] std::expected<runtime::Term, Diagnostic>
lowerTerm(const Expr &expr);

/// Lowers a frontend Goal Call (ExprKind::Call / Identifier) to a runtime
/// GoalCall.
[[nodiscard]] std::expected<runtime::GoalCall, Diagnostic>
lowerGoalCall(const Expr &expr);

/// Lowers a frontend expression to a runtime Expr node.
[[nodiscard]] std::expected<runtime::ExprPtr, Diagnostic>
lowerExpr(const Expr &expr);

/// Lowers a frontend member declaration into a runtime Member.
[[nodiscard]] std::expected<runtime::Member, Diagnostic>
lowerMember(const Decl &decl);

/// Lowers a frontend top-level declaration into a runtime TopDecl.
[[nodiscard]] std::expected<runtime::TopDecl, Diagnostic>
lowerTopDecl(const Decl &decl);

/// Lowers an entire frontend Program into a runtime Unit.
[[nodiscard]] std::expected<runtime::Unit, Diagnostic>
lowerProgramToUnit(const Program &program);

// ============================================================================
// Runtime AST -> Frontend AST Lifting
// ============================================================================

/// Lifts a runtime TypeExpr back to a frontend Type representation.
[[nodiscard]] Type liftType(const runtime::TypeExpr &typeExpr);

/// Lifts a runtime Term into a frontend ExprPtr (ExprKind::Term).
[[nodiscard]] ExprPtr liftTerm(const runtime::Term &term);

/// Lifts a runtime GoalCall into a frontend ExprPtr (ExprKind::Call).
[[nodiscard]] ExprPtr liftGoalCall(const runtime::GoalCall &call);

/// Lifts a runtime Expr into a frontend ExprPtr.
[[nodiscard]] std::expected<ExprPtr, Diagnostic>
liftExpr(const runtime::Expr &expr);

/// Lifts a runtime Member into a frontend Decl.
[[nodiscard]] std::expected<Decl, Diagnostic>
liftMember(const runtime::Member &member);

/// Lifts a runtime TopDecl into a frontend Decl.
[[nodiscard]] std::expected<Decl, Diagnostic>
liftTopDecl(const runtime::TopDecl &topDecl);

/// Lifts an entire runtime Unit into a frontend Program.
[[nodiscard]] std::expected<Program, Diagnostic>
liftUnitToProgram(const runtime::Unit &unit);

// ============================================================================
// Structural & Semantic Equivalence Checking
// ============================================================================

[[nodiscard]] bool areEquivalent(const Type &frontend,
                                 const runtime::TypeExpr &runtime);
[[nodiscard]] bool areEquivalent(const Expr &frontend,
                                 const runtime::Expr &runtime);
[[nodiscard]] bool areEquivalent(const Decl &frontend,
                                 const runtime::TopDecl &runtime);
[[nodiscard]] bool areEquivalent(const Decl &frontend,
                                 const runtime::Member &runtime);
[[nodiscard]] bool areEquivalent(const Program &frontend,
                                 const runtime::Unit &runtime);

// ============================================================================
// Compilation Bridge
// ============================================================================

/// Lowers a frontend Program and compiles an agent directly into Vortex opcode
/// graphs.
[[nodiscard]] std::expected<runtime::CompiledAgent, runtime::CompileFailure>
compileProgramAgent(zigzag::vortex::VortexCore &core, const Program &program,
                    std::string_view agentType);

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_AST_BRIDGE_HPP
