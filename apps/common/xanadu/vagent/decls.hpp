// M0 declaration lowering (design/vagent-actor-executor.md §8): the
// walk that turns a checked Program's Decl tree into the program
// image. Module cells live on programRoot's +d.stdlib rank;
// agent/event/goal/predicate symbols on the module's +d.vars
// (exportSymbol shape); each plan/on/def/method becomes a clause or
// symbol cell whose heads are compileFragment-shaped with declared
// body-head names (§3.5's resume identity). The agent cell hangs from
// a purpose-minted directory cell: home +d.dirs → dir +d.agents →
// agent — never directly on home, whose promotion frontier would
// otherwise accrete dead cursor twins (review r1, §5.2's sibling).
//
// Every one of the 19 DeclKinds carries an explicit disposition:
// lower (cells emitted), stub (structure reserved, semantics a later
// milestone), or refuse (a Diagnostic — silently dropping a declared
// composition directive would lie about what the image holds).
#ifndef COMMON_XANADU_VAGENT_DECLS_HPP
#define COMMON_XANADU_VAGENT_DECLS_HPP

#include <expected>
#include <string>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent {

// The unqualified spellings records.hpp and the sibling vagent
// headers share — declared here so this header also satisfies the
// include-order contract records.hpp relies on.
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::Negward;
using zigzag::noCell;
using zigzag::Posward;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

enum class DeclDisposition : std::uint8_t { Lower, Stub, Refuse };

// The M0 disposition table — one ruling per DeclKind:
//
//   Agent/Event/Goal/Predicate/Plan/Method/Answer/Handler → Lower
//   Field/Record/Belief                                   → Lower
//   Question/Skill/Capacity/Behavior/Lifecycle            → Stub
//   Uses/Install/BehaviorUse                              → Refuse
//
// Stubs mint the symbol so the module vocabulary is complete — the
// deferred semantics are M2+'s — while the three composition
// directives have no image shape to reserve and refuse instead.
[[nodiscard]] DeclDisposition declDisposition(DeclKind kind) noexcept;

// The roots genesis (c) binds on, plus the emission inventory tests
// inspect. `agentCell`/`directory` stay noCell when the program
// declares no agent; `imageRecord` is finalizeProgramImage's cell —
// noCell when no body emitted an op.
struct LoweredImage {
  CellRef programRoot{noCell};
  CellRef ownModule{noCell};
  CellRef agentCell{noCell};
  CellRef directory{noCell};
  CellRef imageRecord{noCell};
  CellRef imageExtent{noCell};
  std::vector<CellRef> symbols;
  std::vector<CellRef> clauses;
  std::vector<CellRef> heads;
  // Qualified names of decls that took the Stub disposition — the
  // disposition table's audit trail.
  std::vector<std::string> stubbed;
};

// Walk `program.declarations` into `core`'s arena and assemble the
// image roots. Bodies lower through compileFragmentBody — a body the
// scalar emitter refuses (agent statement kinds; M2 owns real
// segmentation) still gets its named head, with a single-segment
// placeholder graph so run/graft machinery sees a uniform shape.
[[nodiscard]] std::expected<LoweredImage, std::vector<Diagnostic>>
lowerProgram(const Program &program, zigzag::vortex::VortexCore &core,
             zigzag::vortex::VortexVM &vm, ScalarCompilerLimits limits = {});

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_DECLS_HPP
