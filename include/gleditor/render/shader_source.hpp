/**
 * @file shader_source.hpp
 * @brief Assembly of portable GLSL bodies into backend-specific shader sources.
 *
 * The glyph shaders in assets/shaders are written in the common subset of
 * GLSL 3.30, GLSL ES 3.00 and Vulkan GLSL. Everything that genuinely differs
 * between those dialects -- the version directive, precision qualifiers,
 * whether varyings carry explicit locations, and how the uniforms are declared
 * -- is expressed through GLEDITOR_* macros that this module defines
 * per backend and stage.
 */
#ifndef GLEDITOR_RENDER_SHADER_SOURCE_H
#define GLEDITOR_RENDER_SHADER_SOURCE_H

#include <string>
#include <string_view>

#include <gleditor/render/types.hpp>

namespace render {

/// Which programmable stage a source belongs to.
enum class ShaderStage : std::uint8_t { Vertex, Fragment };

/**
 * @brief Prepend the backend/stage preamble to a portable shader body.
 * @param body Contents of one of the assets/shaders/*.glsl files.
 */
std::string assembleShaderSource(Backend backend, ShaderStage stage,
                                 std::string_view body);

/// Read a portable shader body from disk, throwing std::runtime_error if it is
/// missing.
std::string readShaderBody(const std::string &path);

} // namespace render

#endif // GLEDITOR_RENDER_SHADER_SOURCE_H
// vi: set sw=2 sts=2 ts=2 et:
