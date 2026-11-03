#ifndef COMMON_XANADU_VAGENT_CODE_PATH_HPP
#define COMMON_XANADU_VAGENT_CODE_PATH_HPP

#include <expected>
#include <string_view>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/token.hpp"

namespace xanadu::vagent {

// The code"…" counterpart of cell_path's parseCellsPath: parses and
// validates a literal's decoded inner text into §5.1's CodeBody — the
// optional '#' label, the optional 'Ident : Type {,…} |' parameter
// header, and the body expression — then narrows it to the fragment
// subset: `return` is refused (a fragment's produced value is its last
// expression's) and so are the suspending statements of M6 (`await`,
// `achieve`, `ask`, `yield`). Effect calls are not refused here — which
// calls suspend is signature truth, so the checker owns that arm.
//
// `loc` is the enclosing literal's own position; every diagnostic this
// entry reports is qualified with it in the src:m:l:c+o:c convention of
// §5.1, via innerDiagnostic below. The returned CodeBody keeps
// literal-relative inner locs — qualify them the same way when reporting
// at a body node. `splice`/`static` nodes are not refused at this level:
// the expand stage owns their named DiagnosticCode answer.
[[nodiscard]] std::expected<CodeBody, Diagnostic>
parseCodePath(std::string_view source, SourceLocation loc,
              ParseLimits limits = {});

// Maps a diagnostic raised at a literal-inner position into the
// src:m:l:c+o:c convention: the returned diagnostic's loc is the
// enclosing literal's, and the inner position moves into .inner ahead of
// any already-chained arms — so a nested literal's diagnostics chain one
// '+<off-line>:<off-col>' arm per level, outermost first. This is the
// reporter the impl plan keeps out of parseCodeBody so parser.o (and
// VAGENT_SYNTAX_OBJS) never depends on code_path.
[[nodiscard]] Diagnostic innerDiagnostic(Diagnostic inner,
                                         SourceLocation literal);

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_CODE_PATH_HPP
