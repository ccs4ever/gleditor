#ifndef COMMON_XANADU_VAGENT_CELL_PATH_HPP
#define COMMON_XANADU_VAGENT_CELL_PATH_HPP

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/token.hpp"

namespace xanadu::vagent {

// The embedded-VQL subset accepted inside a cells"…" literal. A cells path
// is an anchor followed by signed dimension steps; each step either
// traverses a rank or appends % creations, and a traversal step may carry
// one link-presence shape filter ([{+d.f}]). A variable anchor names a
// Cell-typed binding whose ref-box the traversal follows at run time.
struct CellsPath {
  struct Anchor {
    enum class Kind : std::uint8_t { Home, LiteralCell, Variable };
    Kind kind{Kind::Home};
    std::string variable;    // Kind::Variable: binding name without '$'
    std::uint64_t cellId{0}; // Kind::LiteralCell
  };
  struct Step {
    std::string dim;    // dimension name, e.g. "d.items"
    bool neg{false};    // "-d.x" direction
    bool create{false}; // has % creations
    // Literal creation values; an empty string is an unnamed mint.
    std::vector<std::string> literals;
    bool hasFilter{false}; // [{+d.f}] link-presence shape predicate
    std::string filterDim;
    bool filterNeg{false};
  };
  Anchor anchor;
  std::vector<Step> steps;
  bool mutating{false};   // any create step
  bool traversing{false}; // any non-create step
};

// Parses and validates the embedded text of a cells"…" literal. The subset
// is the shared one documented in design/vagent-agent-language.md: anchors
// ##, a literal cell id, or $name; signed dimension steps; at most one
// traversal step, which must be last; creations posward and literal only.
// Errors are reported as syntax diagnostics at loc.
[[nodiscard]] std::expected<CellsPath, Diagnostic>
parseCellsPath(std::string_view source, SourceLocation loc);

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_CELL_PATH_HPP
