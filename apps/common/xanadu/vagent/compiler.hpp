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
#include <string>
#include <string_view>

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

  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return core_; }
  [[nodiscard]] zigzag::vortex::VortexVM &vm() noexcept { return vm_; }

private:
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM vm_;
  std::size_t beliefEventDepth_{kDefaultBeliefEventDepth};
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_COMPILER_HPP
