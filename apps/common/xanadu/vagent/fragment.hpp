// The §6.1 fragment-object half of phase 3
// (design/vagent-metaprogramming-implementation.md:224-321): the
// pipeline-side tables the scalar Emitter lowers surviving code"…"
// literals and `run` callsites through, and `compileFragment` — the
// routine that wraps a checked fragment body in its arena-resident
// object: a head cell carrying `+d.entry → entry op`,
// `+d.values → descriptor`, the `+d.refs` marking inventory and the
// `+d.recipe` replay template meta.cpp's `std:meta` routines consume.
#ifndef COMMON_XANADU_VAGENT_FRAGMENT_HPP
#define COMMON_XANADU_VAGENT_FRAGMENT_HPP

#include <expected>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vagent/expand.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"

namespace xanadu::vagent {

// The apparatus dimensions the fragment object hangs its metadata on.
// Names match meta.cpp's mintMetaBindSet table exactly — both sides
// file through findOrMintDimension, so whichever runs first files and
// the other resolves the same DimRef. `entry`/`refs`/`ref`/`recipe`/
// `refFor`/`meshRelocation` are the phase-3 set fragment objects use;
// the remaining apparatus dims are phase-4 surfaces.
struct FragmentDims {
  zigzag::DimRef entry{};          ///< d.entry — head → entry op
  zigzag::DimRef refs{};           ///< d.refs — marking inventory rank
  zigzag::DimRef ref{};            ///< d.ref — record → marked cell
  zigzag::DimRef recipe{};         ///< d.recipe — replay template rank
  zigzag::DimRef refFor{};         ///< d.refFor — ref-capture → slot
  zigzag::DimRef meshRelocation{}; ///< d.mesh.relocation — "cell" mark
};

// The arena-resident fragment object compileFragment hangs on one
// code"…" literal: `head` is the Code<T> value's referent (what a
// head-valued ref-box's int64 names); `entry`, `result`, `produced`
// are the body-graph cells the descriptor and the `run` convention
// reach through it.
struct FragmentObject {
  zigzag::CellRef head{};
  zigzag::CellRef entry{};
  zigzag::CellRef result{};
  zigzag::CellRef produced{};
};

// A member `def`'s callsite target: the module `+d.vars` symbol the
// call resolves through — its `+d.values → head` edge lands when the
// method's own walk finishes, so a caller lowered before its callee
// still resolves at run — and the declared result type the callsite
// reports.
struct MethodTarget {
  zigzag::CellRef sym{};
  Type result{};
};

// A closed `let` carried into a deferred `static` eval: the name bound
// at check time to a literal or a `static` initializer. `manifest` is
// what the eval binds the name to — a `CellValue` for a literal or
// unit init, or the initializer's `Static` node for a staged one,
// whose own pending record the drain resolves first. The eval binds a
// fresh literal cell, never the runtime slot (whose write only exists
// at run).
struct ClosedLocal {
  std::string name;
  Type type{};
  std::variant<zigzag::vortex::CellValue, const Expr *> manifest{};
};

// A `static` operand whose evaluation is deferred to the end of the
// member walk — when every member `def` head's `sym +d.values` edge
// exists, so calls resolve order-independently and `var` initializers
// fold member results. The eval restates `target`'s content (an op
// operand literal, or a var holder); the placeholder's opaque literal
// record was already appended at emit, since a body's refs/literals
// records seal into +d.refs before the drain runs. `closed` carries
// the enclosing `let`s bound to manifest cells — the only free names
// a pure+closed operand may legally read. `expr` is the `Static` node
// itself: the operand hangs off it and diagnostics locate there.
struct PendingStatic {
  const Expr *expr{};
  zigzag::CellRef target{};
  Type type{};
  const std::map<std::string, MethodTarget, std::less<>> *methods{};
  std::vector<ClosedLocal> closed;
};

// The pipeline tables a compile carries beside the bindings: expand's
// §5.6 fragment table and per-literal metadata, the checker's
// per-fragment info (via check()'s infoOut), the capture records for
// provenance reads, and the binding table the body's cap$N aliases and
// outside references resolve through. `emitted` memoizes one head per
// literal — the same literal referenced by `let` and `run` emits once.
struct FragmentContext {
  const FragmentTable *fragments{};
  const std::map<FragmentKey, FragmentMeta> *meta{};
  const std::map<std::string, CaptureRecord, std::less<>> *captures{};
  const FragmentInfoMap *info{};
  const CompilerBindings *bindings{};
  // The member-method callsite table the `def` call arm resolves
  // against — flat member names (an agent's `def`s); capacity or
  // qualified calls are not in it and refuse as before.
  const std::map<std::string, MethodTarget, std::less<>> *methods{};
  // §5.5's deferred-eval sink — set by the program lowerer so `static`
  // operands evaluate once after the member walk, when every `def`
  // head's `sym +d.values` edge exists, rather than at their emission
  // site. Absent, `static` evaluates eagerly at emit — the statement-
  // level path's behavior.
  std::vector<PendingStatic> *pendingStatics{};
  // The compiling unit's module name — the `src:<module>:<line>:<col>`
  // head label's first segment (§6.7); absent for a module-less unit.
  std::optional<std::string> module;
  mutable std::map<FragmentKey, FragmentObject> emitted;
};

// The exact schema the sibling's meta.cpp consumes — kept in one
// place so a schema change diffs against their readers:
//
//   head
//     +d.entry   → entry op (the fragment.entry op cell)
//     +d.name    → label cell  — code"…#label" text verbatim, else
//                  the §6.7 synthesized
//                  "src:<module>:<line>:<col>[+o<ordinal>]"
//     +d.values  → descriptor cell (core d.values, never a meta dim)
//     +d.refs    → first refs record; +d.step chains the rest
//     +d.recipe  → first recipe member; +d.step chains the rest
//
//   descriptor
//     +d.vars    → rank of field records; each record's *content* is
//                  the key verbatim, its +d.values edge the value:
//                  "result" → the body's published result cell
//                    (+d.name on the same record → result type name)
//                  "produced" → the body's published produced cell
//                  "pure" → bool cell (checker FragmentInfo.pure)
//                  "closed" → bool cell (FragmentInfo.closed)
//                  "arity" → int64 cell (declared parameter count)
//                  one per declared parameter: [param name] → typeName
//     +d.grab    → first parameter cell, +d.step chains declaration
//                  order; each parameter cell +d.name→name and
//                  +d.values→typeName cell
//
//   The descriptor apparatus is itself recipe-covered: literal members
//   twin head/descriptor/param/record cells and link tuples replay
//   their edges (head +d.entry included — the copy twins the entry and
//   keeps the source head's graph intact), so a reified head is a
//   runnable fragment, not a bare wrapper. Only head +d.name, +d.refs
//   and +d.recipe stay out — reify owns the copy's label and
//   regenerates both chains.
//
//   +d.refs records
//     +d.ref     → the marked source cell (slot, ref cell or recipe
//                  member cell — never +d.values, which evicts links)
//     +d.name    → name cell — NAME-record (label/call/run/dim);
//                  absent → REF-record
//     +d.grab    → role cell ("call" | "run") — name-records only
//     marked non-name cells carry +d.mesh.relocation → "cell"
//
//   +d.recipe members (int64 positional content, +d.step chained)
//     literal member      int64 = source cell position (content copy)
//     link tuple          four members: from pos, dim anchor
//                         (+d.name→dim name, content Int64(DimRef)),
//                         dir (sign), to pos
//     ref-capture member  int64 = slot pos; +d.refFor→slot; +d.name→
//                         dim name on dim referents; +d.values→
//                         binding on cap$N capture slots
//
// `run` ABI (§6.2 + the sibling's runFragment): the Code value's
// referent is the *head*; the caller binds each argument by parameter
// name on the cursor's +d.vars rank (the VM's one vars channel — a
// pushed frame holds only the return op, and no graph op can reach a
// frame's +d.vars), then #CALLs the *entry op*. The callee's prologue
// resolves parameters by name off that same cursor rank — Bind's
// prepend/first-match shape, so innermost activation binds win and
// recursion is safe. Return lands on the Call's +d.spin target, where
// the callsite gates on the descriptor-published produced cell and
// copies the published result cell out. A kernel-side runFragment
// binds args the same way — prepended on the *cursor's* +d.vars,
// variable cells carrying the parameter name as content and +d.values
// the value — never on the frame, which Resolve does not walk.
// Parameter signature is descriptor data — never an argument channel.

// The fragment-table key for a surviving literal — loc + fragment
// ordinal, the same shape the checker builds.
[[nodiscard]] inline FragmentKey fragmentKeyOf(const Expr &literal) {
  return {literal.loc, literal.stamp ? literal.stamp->ordinal : std::nullopt};
}

// The head/callsite label for one literal: the code"#label …" text
// verbatim when declared, else the §6.7 `src:<module>:<line>:<col>`
// position key (`+o<ordinal>` appended for a nested literal — the
// fragment ordinal is the program-wide arm of the +o:c descent). The
// one string both the head's +d.name and the callsite name-records
// carry.
[[nodiscard]] inline std::string
fragmentLabelOf(const FragmentKey &key,
                const std::optional<std::string> &declared,
                const std::optional<std::string> &module) {
  if (declared && !declared->empty()) return *declared;
  std::string label = "src:" + (module ? *module : "") + ":" +
                      std::to_string(key.loc.line) + ":" +
                      std::to_string(key.loc.column);
  if (key.ordinal) label += "+o" + std::to_string(*key.ordinal);
  return label;
}

// Mint or reuse the fragment object for a surviving code"…" literal.
// Emits the body through compiler.compileFragmentBody, hangs head,
// descriptor, +d.refs inventory and +d.recipe template, memoizes on
// context.emitted, and refuses — as a Diagnostic — splice-only
// fragments, missing records, and missing checker info. `frameLocals`
// is the enclosing emitter's in-scope local-name→cell overlay: a
// cap$N whose captured name is a `let` local (a frame index, absent
// from context.bindings) resolves to the caller's local cell here,
// and an inner `let` shadows a decl-level `var` of the same name.
// `memberFields` is the enclosing member's owner-field set: a cap$N
// whose captured name is a skill/behavior `var` resolves
// owner-relative — the literal declares the trailing `$owner` Cell
// parameter of §5.8's member convention, and the field reads as a
// #FIND hop on the bound owner cell.
[[nodiscard]] std::expected<FragmentObject, Diagnostic>
compileFragment(const Expr &literal, const FragmentContext &context,
                Compiler &compiler,
                const CompilerBindings *frameLocals = nullptr,
                const OwnerFields *memberFields = nullptr);

// File the phase-3 apparatus dims the emission needs (idempotent —
// findOrMintDimension's find arm resolves a sibling-filed name).
[[nodiscard]] FragmentDims mintFragmentDims(zigzag::vortex::VortexCore &core);

// The program-image record §6.6's programRef bind record carries: a
// cell whose +d.vars records are "floor" → int64(imageFloor) and
// "ops" → a +d.step-chained rank of int64 op positions in mint order
// (the post-fold enumeration genesis' setOpcodeKind pass replays).
// Returns absent when nothing was emitted.
[[nodiscard]] std::optional<zigzag::CellRef>
finalizeProgramImage(const Compiler &compiler);

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_FRAGMENT_HPP
