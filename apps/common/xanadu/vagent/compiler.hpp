/**
 * @file compiler.hpp
 * @brief Lowers one vagent agent type to Vortex opcode graphs and the agent
 *        cell the executor adapter drives (design §5.2, §5.5, §5.7-§5.9, §9).
 *
 * What the existing opcode set can and cannot do shapes every choice here:
 * an operand is a *slot* cell whose value names the cell to read, so an
 * operation reaches a cell minted at run time only if the compiler first
 * writes that cell's ref into the operation's slot (a "poke"), and a
 * neighbour can be read only through a parameter pipeline. The compiler
 * therefore emits a small pointer machine: registers are static cells,
 * records, lists and events are held as refs in registers, and shared
 * routines (field lookup, append, deep copy, outbox push) are called with
 * `#Call` and globals for arguments. Locals live in per-routine registers; a
 * routine in a call cycle saves and restores its whole register set around
 * each activation (name-keyed, by call depth), which is the design's "spill
 * across the call" realised without an addressable frame cell.
 *
 * Refuses by returning std::expected<_, CompileFailure>; user input never
 * throws out of this API.
 */
#ifndef COMMON_XANADU_VAGENT_COMPILER_HPP
#define COMMON_XANADU_VAGENT_COMPILER_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/runtime_abi.hpp"
#include "common/xanadu/vagent/token.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace xanadu::vagent {

enum class CompileError : std::uint8_t {
  Unsupported, ///< a construct outside the iteration-2 subset
  UnknownAgent,
  UnknownName,
  UnknownType,
  DuplicateName,
  BadArity,
  TypeMismatch,
};

[[nodiscard]] std::string_view compileErrorName(CompileError code) noexcept;

struct CompileFailure {
  CompileError code{CompileError::Unsupported};
  std::string detail;
  SourceLocation loc{};
};

/// A compiled method's body as a unit a host can run against any frame that
/// has the method's layout, which is how a suspended plan body (iteration 4c)
/// will resume with the locals it left on an intention's frame. Every entry is
/// a stub that ends at the agent's halt op.
struct FrameBody {
  /// The handle the host points at a frame cell. `make` leaves it naming the
  /// frame it built; `enter` runs the body against the frame it names.
  zigzag::CellRef frame{zigzag::noCell};
  /// Builds a frame, copies `args` into its parameters, and points `frame`
  /// at it. The frame is left off the stack.
  zigzag::CellRef make{zigzag::noCell};
  /// Stacks the frame `frame` names, binds the body's handles to it, runs the
  /// body and unstacks it. The result is in `result`.
  zigzag::CellRef enter{zigzag::noCell};
  std::vector<zigzag::CellRef> args;
  zigzag::CellRef result{zigzag::noCell};
};

class Compiler {
public:
  explicit Compiler(zigzag::vortex::VortexCore &core);

  /// Compile @p agentType of @p unit into the core's arena: the agent cell
  /// with its reserved fields, parameters and fields, and the two routines.
  [[nodiscard]] std::expected<CompiledAgent, CompileFailure>
  compileAgent(const Unit &unit, std::string_view agentType);

  /// `vagent.belief_event_depth` (§4.2): settle rounds allowed per turn before
  /// the turn fails hard. Applies to agents compiled afterwards.
  Compiler *setBeliefEventDepth(std::size_t rounds) noexcept;

  /// The frame-run entries of the agent's method @p name, from the last
  /// successful `compileAgent`.
  [[nodiscard]] std::optional<FrameBody> frameBody(std::string_view name) const;

  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return core_; }
  [[nodiscard]] zigzag::vortex::VortexVM &vm() noexcept { return vm_; }

private:
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM vm_;
  std::size_t beliefEventDepth_{kDefaultBeliefEventDepth};
  std::map<std::string, FrameBody, std::less<>> frameBodies_;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_COMPILER_HPP
