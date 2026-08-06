/**
 * @file shader_introspect.hpp
 * @brief Minimal GLSL source introspection used to discover attribute and
 *        uniform declarations before linking a program.
 *
 * The renderer derives its vertex attribute layout from the order and component
 * count of the `in` declarations in the vertex shader, so it needs to know the
 * name, base type and component count of every declaration in a shader stage.
 */
#ifndef GLEDITOR_SHADER_INTROSPECT_H
#define GLEDITOR_SHADER_INTROSPECT_H

#include <string>
#include <string_view>
#include <vector>

/**
 * @brief A single `in`/`uniform` declaration recovered from GLSL source.
 */
struct ShaderDecl {
  /// Storage qualifier: either "in" or "uniform".
  std::string kind;
  /// Base type name with any trailing component count removed, e.g. `vec3` is
  /// reported as "vec" and `uint` as "uint". Callers match on this prefix to
  /// decide between the integer and float glVertexAttrib*Pointer entry points.
  std::string varType;
  /// Declared name of the variable.
  std::string name;
  /// Component count for `in` declarations (1 for scalars such as `uint`,
  /// 3 for `vec3`, 2 for `uvec2`); always 0 for uniforms, whose size is not
  /// used to compute the vertex buffer stride.
  int size{};

  bool operator==(const ShaderDecl &oth) const = default;
};

/**
 * @brief Scan GLSL source for top-level `in` and `uniform` declarations.
 *
 * Only the vertex stage contributes `in` declarations; for every other stage
 * they name inter-stage varyings rather than vertex attributes and are skipped.
 *
 * @param source GLSL source text.
 * @param isVertexStage true when @p source is the vertex stage of a program.
 * @return Declarations in source order.
 */
std::vector<ShaderDecl> parseShaderDecls(std::string_view source,
                                         bool isVertexStage);

#endif // GLEDITOR_SHADER_INTROSPECT_H
// vi: set sw=2 sts=2 ts=2 et:
