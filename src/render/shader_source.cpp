/**
 * @file shader_source.cpp
 * @brief Implementation of the portable-GLSL preamble assembly.
 */
#include <gleditor/render/shader_source.hpp> // IWYU pragma: associated

#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace render {

namespace {

/**
 * @brief Declarations of the uniforms the shader bodies reference.
 *
 * OpenGL and OpenGL ES take plain uniforms for the matrices and the sampler;
 * Vulkan has no default uniform block, so every one of them has to live in a
 * descriptor-backed block or a push constant.
 */
std::string uniformBlock(const Backend backend, const ShaderStage stage) {
  const bool vulkan = Backend::Vulkan == backend;

  std::string out;
  if (ShaderStage::Vertex == stage) {
    if (vulkan) {
      // The camera changes once per frame and the model matrix once per draw,
      // so the model matrix goes in a push constant to avoid a descriptor
      // update -- and a buffer -- for every glyph run.
      out += "layout(set = 0, binding = 0, std140) uniform Camera {\n"
             "    mat4 uProjection;\n"
             "    mat4 uView;\n"
             "};\n"
             "layout(push_constant) uniform Push {\n"
             "    mat4 uModel;\n"
             "} uPush;\n"
             "#define uModel uPush.uModel\n";
    } else {
      out += "uniform mat4 uProjection;\n"
             "uniform mat4 uView;\n"
             "uniform mat4 uModel;\n";
    }

    out += "struct HighlightRange {\n"
           "    uint start;\n"
           "    uint end;\n"
           "    uint colour;\n"
           "    uint reserved;\n"
           "};\n";
    out += vulkan ? "layout(set = 0, binding = 1, std140) uniform Highlights {\n"
                  : "layout(std140) uniform Highlights {\n";
    out += std::format("    HighlightRange uRanges[{}];\n", maxHighlightRanges);
    out += "};\n";
  } else {
    out += vulkan
               ? "layout(set = 0, binding = 2) uniform sampler2DArray uGlyphAtlas;\n"
               : "uniform sampler2DArray uGlyphAtlas;\n";
  }
  return out;
}

/// Version directive and, for OpenGL ES, the precision defaults that desktop
/// GLSL supplies implicitly but ES requires to be stated.
std::string versionAndPrecision(const Backend backend,
                                const ShaderStage stage) {
  switch (backend) {
  case Backend::OpenGL:
    return "#version 330 core\n";
  case Backend::Vulkan:
    return "#version 450 core\n";
  case Backend::OpenGLES:
    return std::format("#version 300 es\n"
                       "precision highp float;\n"
                       "precision highp int;\n"
                       "{}",
                       ShaderStage::Fragment == stage
                           ? "precision highp sampler2DArray;\n"
                           : "");
  }
  throw std::invalid_argument("assembleShaderSource: unknown backend");
}

/**
 * @brief Interface-variable macros.
 *
 * Vulkan requires an explicit location on every interface variable including
 * varyings. Desktop GLSL 3.30 and GLSL ES 3.00 only accept them on vertex
 * attributes and fragment outputs, and match varyings by name instead.
 */
std::string interfaceMacros(const Backend backend, const ShaderStage stage) {
  const bool located = Backend::Vulkan == backend;
  const auto varying = [located](const char *direction) {
    return located ? std::format("layout(location = loc) {}", direction)
                   : std::string{direction};
  };
  const auto flatVarying = [located](const char *direction) {
    return located ? std::format("layout(location = loc) flat {}", direction)
                   : std::format("flat {}", direction);
  };

  std::string out;
  if (ShaderStage::Vertex == stage) {
    // Vertex attributes take explicit locations on every backend.
    out += "#define GLEDITOR_IN(loc) layout(location = loc) in\n";
    out += std::format("#define GLEDITOR_OUT(loc) {}\n", varying("out"));
    out +=
        std::format("#define GLEDITOR_OUT_FLAT(loc) {}\n", flatVarying("out"));
    out += Backend::Vulkan == backend
               ? "#define GLEDITOR_VERTEX_INDEX gl_VertexIndex\n"
               : "#define GLEDITOR_VERTEX_INDEX gl_VertexID\n";
  } else {
    out += std::format("#define GLEDITOR_IN(loc) {}\n", varying("in"));
    out += std::format("#define GLEDITOR_IN_FLAT(loc) {}\n", flatVarying("in"));
    // Fragment outputs are located on every backend.
    out += "#define GLEDITOR_FRAG_OUT(loc) layout(location = loc) out\n";
  }
  return out;
}

} // namespace

std::string assembleShaderSource(const Backend backend, const ShaderStage stage,
                                 const std::string_view body) {
  std::string out = versionAndPrecision(backend, stage);
  out += std::format("#define GLEDITOR_MAX_HIGHLIGHTS {}\n", maxHighlightRanges);
  out += interfaceMacros(backend, stage);
  out += uniformBlock(backend, stage);
  // Reset the line counter so compiler diagnostics point at lines of the
  // portable body rather than at the generated preamble. The ES dialect
  // rejects the two-argument form, so only the line number is set.
  out += Backend::OpenGLES == backend ? "#line 1\n" : "#line 1 0\n";
  out += body;
  return out;
}

std::string readShaderBody(const std::string &path) {
  std::ifstream stream(path);
  if (!stream.is_open()) {
    throw std::runtime_error("Failed to open shader source: " + path);
  }
  std::ostringstream buf;
  buf << stream.rdbuf();
  return buf.str();
}

} // namespace render
// vi: set sw=2 sts=2 ts=2 et:
