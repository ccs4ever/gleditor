/**
 * @file fuzz_vagent_frontend.cpp
 * @brief Exercise vagent tokenization, both parser entry points, and checking.
 */
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <tuple>

#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/lexer.hpp"
#include "common/xanadu/vagent/parser.hpp"

namespace {

// These bound one fuzz iteration's work; they are not source-language limits
// or application settings. The corpus includes ordinary multi-agent units.
constexpr std::size_t maxHarnessInputBytes = 64 * 1024;
constexpr std::size_t maxHarnessTokens     = 16 * 1024;
constexpr std::size_t maxHarnessDepth      = 64;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (size > maxHarnessInputBytes) return 0;
  const std::string_view source =
      size == 0 ? std::string_view{}
                : std::string_view(reinterpret_cast<const char *>(data), size);

  auto tokens = xanadu::vagent::Lexer(source).tokenize();
  if (tokens) {
    if (tokens->empty() ||
        tokens->back().kind != xanadu::vagent::TokenKind::EndOfFile) {
      std::abort();
    }
    for (const auto &token : *tokens) {
      if (token.loc.line == 0 || token.loc.column == 0 ||
          token.loc.offset > size) {
        std::abort();
      }
    }
  }

  const xanadu::vagent::ParseLimits limits{maxHarnessTokens, maxHarnessDepth};
  auto program = xanadu::vagent::Parser(source, limits).parseProgram();
  if (program) std::ignore = xanadu::vagent::check(*program);
  std::ignore = xanadu::vagent::Parser(source, limits).parseExpression();
  return 0;
}
