/**
 * @file shader_assemble.cpp
 * @brief Host tool that writes the backend-specific form of a portable shader.
 *
 * The Vulkan backend consumes SPIR-V, which has to be produced at build time.
 * Compiling it from a hand-maintained copy of the shader would let the offline
 * form drift away from what assembleShaderSource() generates at runtime for the
 * other backends, so this tool calls that same function: the preamble has
 * exactly one definition.
 *
 * Usage: shader_assemble <backend> <vert|frag> <input.glsl> <output.glsl>
 */
#include <fstream>
#include <iostream>
#include <string>

#include <gleditor/render/shader_source.hpp>
#include <gleditor/render/types.hpp>

int main(const int argc, const char *const *const argv) {
  if (5 != argc) {
    std::cerr << "usage: shader_assemble <backend> <vert|frag> <in> <out>\n";
    return 2;
  }

  try {
    const auto backend = render::backendFromName(argv[1]);
    const std::string stageName = argv[2];
    if ("vert" != stageName && "frag" != stageName) {
      std::cerr << "stage must be vert or frag\n";
      return 2;
    }
    const auto stage = "vert" == stageName ? render::ShaderStage::Vertex
                                           : render::ShaderStage::Fragment;

    const auto body = render::readShaderBody(argv[3]);

    std::ofstream out(argv[4]);
    if (!out.is_open()) {
      std::cerr << "cannot write " << argv[4] << "\n";
      return 1;
    }
    out << render::assembleShaderSource(backend, stage, body);
    return 0;
  } catch (const std::exception &err) {
    std::cerr << "shader_assemble: " << err.what() << "\n";
    return 1;
  }
}
// vi: set sw=2 sts=2 ts=2 et:
