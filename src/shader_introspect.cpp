/**
 * @file shader_introspect.cpp
 * @brief Implementation of the GLSL declaration scanner.
 */
#include <gleditor/shader_introspect.hpp> // IWYU pragma: associated

#include <regex>       // for regex, sregex_iterator, regex_constants
#include <string>      // for string, stoi
#include <string_view> // for string_view
#include <vector>      // for vector

namespace {

// Matches a declaration such as "uniform mat4 model;" or "in uvec2 tag;".
// Group 1: storage qualifier, group 2: alphabetic part of the type, group 3:
// the optional trailing component count, group 4: the variable name.
const std::regex &declRegex() {
  static const std::regex reg(
      R"(^\s*(uniform|in)\s+([a-zA-Z]+)(\d+)?\S*?\s+(\w+)\s*;)",
      std::regex_constants::multiline);
  return reg;
}

} // namespace

std::vector<ShaderDecl> parseShaderDecls(const std::string_view source,
                                         const bool isVertexStage) {
  std::vector<ShaderDecl> decls;

  const std::cregex_iterator end;
  for (std::cregex_iterator it(source.data(), source.data() + source.size(),
                               declRegex());
       it != end; ++it) {
    const auto kind = it->str(1);
    // Outside the vertex stage an `in` names an inter-stage varying, not a
    // vertex attribute, so it contributes nothing to the buffer layout.
    if ("in" == kind && !isVertexStage) {
      continue;
    }
    // The component count is optional: scalar types such as `float` and `uint`
    // leave the group unmatched and are a single component. Reading it with
    // std::stoi unconditionally throws std::invalid_argument on those.
    const auto &countGroup = (*it)[3];
    const int components =
        countGroup.matched ? std::stoi(countGroup.str()) : 1;
    decls.push_back(ShaderDecl{kind, it->str(2), it->str(4),
                               "in" == kind ? components : 0});
  }

  return decls;
}
// vi: set sw=2 sts=2 ts=2 et:
