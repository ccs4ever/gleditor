#include <gtest/gtest.h> // for Test, TestInfo (ptr only), EXPECT_EQ

#include <gleditor/shader_introspect.hpp> // for ShaderDecl, parseShaderDecls

#include <gmock/gmock.h> // for ElementsAre, IsEmpty
#include <string>        // for string
#include <vector>        // for vector

using testing::ElementsAre;
using testing::IsEmpty;

namespace {
constexpr bool kVertexStage    = true;
constexpr bool kNonVertexStage = false;
} // namespace

// Regression: scalar types leave the component-count group unmatched, which
// used to be fed to std::stoi and aborted the program during startup.
TEST(ShaderIntrospect, scalarUniformHasNoComponentCount) {
  EXPECT_THAT(parseShaderDecls("uniform float cubeDepth;\n", kVertexStage),
              ElementsAre(ShaderDecl{"uniform", "float", "cubeDepth", 0}));
}

TEST(ShaderIntrospect, scalarAttributeIsOneComponent) {
  EXPECT_THAT(parseShaderDecls("in uint fgcolor;\n", kVertexStage),
              ElementsAre(ShaderDecl{"in", "uint", "fgcolor", 1}));
}

TEST(ShaderIntrospect, vectorAttributeUsesTrailingCount) {
  EXPECT_THAT(parseShaderDecls("in vec3 position;\n", kVertexStage),
              ElementsAre(ShaderDecl{"in", "vec", "position", 3}));
}

TEST(ShaderIntrospect, unsignedVectorAttribute) {
  EXPECT_THAT(parseShaderDecls("in uvec2 tag;\n", kVertexStage),
              ElementsAre(ShaderDecl{"in", "uvec", "tag", 2}));
}

TEST(ShaderIntrospect, matrixUniformReportsNoSize) {
  EXPECT_THAT(parseShaderDecls("uniform mat4 model;\n", kVertexStage),
              ElementsAre(ShaderDecl{"uniform", "mat", "model", 0}));
}

TEST(ShaderIntrospect, samplerUniformReportsNoSize) {
  EXPECT_THAT(
      parseShaderDecls("uniform sampler2DArray texGlyphCache;\n", kVertexStage),
      ElementsAre(ShaderDecl{"uniform", "sampler", "texGlyphCache", 0}));
}

TEST(ShaderIntrospect, inputsOutsideVertexStageAreSkipped) {
  EXPECT_THAT(parseShaderDecls("in vec3 position;\n", kNonVertexStage),
              IsEmpty());
}

TEST(ShaderIntrospect, uniformsAreKeptOutsideVertexStage) {
  EXPECT_THAT(parseShaderDecls("uniform mat4 model;\n", kNonVertexStage),
              ElementsAre(ShaderDecl{"uniform", "mat", "model", 0}));
}

TEST(ShaderIntrospect, leadingWhitespaceAndLayoutLinesAreTolerated) {
  EXPECT_THAT(parseShaderDecls("layout(location = 0)\n  in vec3 position;\n",
                               kVertexStage),
              ElementsAre(ShaderDecl{"in", "vec", "position", 3}));
}

TEST(ShaderIntrospect, declarationsAreReturnedInSourceOrder) {
  const std::string src = "in vec3 position;\n"
                          "in uint fgcolor;\n"
                          "in vec2 texcoord;\n"
                          "uniform mat4 projection;\n";
  EXPECT_THAT(parseShaderDecls(src, kVertexStage),
              ElementsAre(ShaderDecl{"in", "vec", "position", 3},
                          ShaderDecl{"in", "uint", "fgcolor", 1},
                          ShaderDecl{"in", "vec", "texcoord", 2},
                          ShaderDecl{"uniform", "mat", "projection", 0}));
}

TEST(ShaderIntrospect, emptySourceYieldsNoDeclarations) {
  EXPECT_THAT(parseShaderDecls("", kVertexStage), IsEmpty());
}

// vi: set sw=2 sts=2 ts=2 et:
