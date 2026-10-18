#ifndef COMMON_XANADU_VORTEX_MESH_EVALUATOR_HPP
#define COMMON_XANADU_VORTEX_MESH_EVALUATOR_HPP

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/xanadu/vortex/worker_context.hpp"

namespace zigzag::vortex::mesh {

enum class Frontend { Vortex, Vpl, Vquery, Vprolog, Vlace };

struct EvaluationBudget {
  std::size_t maxCycles;
  std::size_t maxCells;
  std::size_t maxSolutions;
  std::size_t maxOutputBytes;
};

/// Parsing has its own structural ceilings before recursive AST consumers run.
/// Hosts may choose a tighter policy without changing the language parser.
struct SyntaxLimits {
  std::size_t bytes{1024 * 1024};
  std::size_t tokens{512};
  std::size_t nesting{64};
};

/// References are artifact-local ordinals, relocated on each worker. They
/// never carry the sender's CellRef or a native callback/program counter.
struct VortexReference {
  std::size_t cell;
};
struct VortexDimension {
  std::string name;
};
using VortexLiteral =
    std::variant<std::monostate, CellValue, VortexReference, VortexDimension>;
struct VortexLink {
  std::string dimension;
  std::size_t target;
  DimVector direction{DimVector::POS};
};
struct VortexProgramCell {
  VortexLiteral value;
  std::vector<VortexLink> links;
};
struct VortexInstruction {
  std::size_t cell;
  OpcodeKind opcode;
  std::vector<std::size_t> inputs;
  std::vector<std::size_t> outputs;
  std::optional<std::size_t> next;
};
struct VortexOutput {
  std::size_t cell;
  bool dereference{false};
};
struct VortexProgram {
  std::vector<VortexProgramCell> cells;
  std::vector<VortexInstruction> instructions;
  std::size_t entry;
  std::vector<VortexOutput> outputs;
};

/// Validate and seal a closed, pure VM instruction graph into canonical,
/// content-hashed bytes suitable for publishProgram/Frontend::Vortex. Source
/// limits bound decoding before any graph or recursive bencode allocation.
[[nodiscard]] std::expected<std::string, std::string>
sealVortexProgram(const VortexProgram &program, SyntaxLimits limits = {});

struct EvaluatedValue {
  std::unique_ptr<WorkerContext> context;
  CellRef root;
};

/// Evaluate immutable source in a private arena. The snapshot and its reader
/// must outlive the returned context. This entry point never performs IO.
/// Results use d.mesh.items for ordered records and d.mesh.value for each
/// record's value. VPL axes retain dimension, direction and extent metadata.
[[nodiscard]] std::expected<EvaluatedValue, std::string>
evaluate(Frontend frontend, std::string_view source, EvaluationBudget budget,
         const Manifold *snapshot         = nullptr,
         const xanadu::SpanReader *reader = nullptr, SyntaxLimits syntax = {});

} // namespace zigzag::vortex::mesh
#endif
