#include <gtest/gtest.h>

#include <limits>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/provenance.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"

namespace zigzag::vortex::mesh {
namespace {
constexpr EvaluationBudget budget{1'000'000, 100'000, 100, 1'000'000};
using Wire = xanadu::bencode::Value;

VortexProgram addition() {
  return {.cells        = {{},
                           {},
                           {CellValue{std::int64_t{20}}, {}},
                           {CellValue{std::int64_t{22}}, {}},
                           {}},
          .instructions = {{0, OpcodeKind::Add, {2, 3}, {4}, 1},
                           {1, OpcodeKind::Halt, {}, {}, {}}},
          .entry        = 0,
          .outputs      = {{4}}};
}
CellRef along(const ArenaManifold &arena, CellRef root,
              std::string_view dimension) {
  for (const auto &edge : arena.dimensionsOf(root))
    if (arena.textOf(edge.dim) == dimension) return edge.pos;
  return noCell;
}
CellRef firstValue(const EvaluatedValue &value) {
  auto &arena = value.context->arena;
  return along(arena, along(arena, value.root, "d.mesh.items"), "d.mesh.value");
}
std::string replaceBody(const std::string &bytes, const Wire &body) {
  auto envelope      = xanadu::bencode::decode(bytes);
  auto fields        = envelope.asDict();
  const auto encoded = body.encode();
  fields["body"]     = Wire::string(encoded);
  fields["sha256"]   = Wire::string(xanadu::sha256Hex(encoded));
  return Wire::dict(std::move(fields)).encode();
}

TEST(MeshVortexArtifactTest, SealedInstructionsExecuteInIndependentWorkers) {
  auto bytes = sealVortexProgram(addition());
  ASSERT_TRUE(bytes) << bytes.error();
  EXPECT_EQ(*bytes, *sealVortexProgram(addition()));
  auto first = evaluate(Frontend::Vortex, *bytes, budget);
  ASSERT_TRUE(first) << first.error();
  auto second = evaluate(Frontend::Vortex, *bytes, budget);
  ASSERT_TRUE(second) << second.error();
  EXPECT_NE(first->context.get(), second->context.get());
  EXPECT_EQ(first->context->arena.asInt64(firstValue(*first)), 42);
  EXPECT_EQ(second->context->arena.asInt64(firstValue(*second)), 42);
}

TEST(MeshVortexArtifactTest, ControlTargetsRelocateAndReturnTypedHandles) {
  auto program = addition();
  program.cells.push_back({VortexReference{0}, {}});
  program.cells.push_back({});
  program.instructions.push_back({6, OpcodeKind::Jump, {5}, {}, {}});
  program.entry = 6;
  program.outputs.push_back({5});
  auto bytes = sealVortexProgram(program);
  ASSERT_TRUE(bytes) << bytes.error();
  auto result = evaluate(Frontend::Vortex, *bytes, budget);
  ASSERT_TRUE(result) << result.error();
  auto &arena = result->context->arena;
  EXPECT_EQ(arena.asInt64(firstValue(*result)), 42);
  auto record = along(arena, result->root, "d.mesh.items");
  record      = along(arena, record, "d.mesh.items");
  auto handle = along(arena, record, "d.mesh.value");
  ASSERT_EQ(arena.valueKindOf(handle), xanadu::ValueKind::OpHandle);
  ASSERT_TRUE(arena.handleTarget(handle));
  EXPECT_NE(*arena.handleTarget(handle), 0);
  EXPECT_EQ(result->context->vm.getOpcodeKind(*arena.handleTarget(handle)),
            OpcodeKind::Add);
  auto marker = along(arena, handle, "d.mesh.relocation");
  ASSERT_NE(marker, noCell);
  EXPECT_EQ(arena.textOf(marker), "cell");
}

TEST(MeshVortexArtifactTest, ResultGraphPreservesLiteralTypesAndLinks) {
  auto program = addition();
  program.cells.push_back(
      {CellValue{std::string("answer")}, {{"d.answer", 4}}});
  program.outputs = {{5}};
  auto bytes      = sealVortexProgram(program);
  ASSERT_TRUE(bytes) << bytes.error();
  auto result = evaluate(Frontend::Vortex, *bytes, budget);
  ASSERT_TRUE(result) << result.error();
  auto &arena = result->context->arena;
  auto root   = firstValue(*result);
  EXPECT_EQ(arena.textOf(root), "answer");
  EXPECT_EQ(arena.asInt64(along(arena, root, "d.answer")), 42);
}

TEST(MeshVortexArtifactTest, MalformedHashVersionAndUnsealedSourceRefuse) {
  auto bytes = sealVortexProgram(addition());
  ASSERT_TRUE(bytes) << bytes.error();
  EXPECT_FALSE(evaluate(Frontend::Vortex, "1 + 2", budget));
  EXPECT_FALSE(
      evaluate(Frontend::Vortex, bytes->substr(0, bytes->size() - 1), budget));
  auto fields      = xanadu::bencode::decode(*bytes).asDict();
  fields["sha256"] = Wire::string(std::string(64, '0'));
  EXPECT_FALSE(evaluate(Frontend::Vortex, Wire::dict(fields).encode(), budget));
  fields            = xanadu::bencode::decode(*bytes).asDict();
  fields["version"] = Wire::integer(999);
  EXPECT_FALSE(evaluate(Frontend::Vortex, Wire::dict(fields).encode(), budget));
}

TEST(MeshVortexArtifactTest, InvalidReferencesOpcodesAndNativeCallbacksRefuse) {
  auto program  = addition();
  program.entry = 99;
  EXPECT_FALSE(sealVortexProgram(program));
  program                        = addition();
  program.instructions[0].inputs = {999};
  EXPECT_FALSE(sealVortexProgram(program));
  program                        = addition();
  program.instructions[0].opcode = static_cast<OpcodeKind>(255);
  EXPECT_FALSE(sealVortexProgram(program));
  program                      = addition();
  program.instructions[0].next = 4;
  EXPECT_FALSE(sealVortexProgram(program));
  auto bytes = sealVortexProgram(addition());
  ASSERT_TRUE(bytes) << bytes.error();
  const auto body = xanadu::bencode::decode(
      xanadu::bencode::decode(*bytes).find("body")->asString());
  auto fields = body.asDict();
  auto cells  = fields["cells"].asList();
  auto cell   = cells[2].asDict();
  cell["value"] =
      Wire::list({Wire::string("native"), Wire::string("putStrLn")});
  cells[2]        = Wire::dict(std::move(cell));
  fields["cells"] = Wire::list(std::move(cells));
  EXPECT_FALSE(evaluate(Frontend::Vortex,
                        replaceBody(*bytes, Wire::dict(std::move(fields))),
                        budget));
}

TEST(MeshVortexArtifactTest, VagentDepthOpcodeCannotEnterPureArtifacts) {
  auto program                        = addition();
  program.instructions.front().opcode = OpcodeKind::ChoiceDepth;
  program.instructions.front().inputs.clear();
  EXPECT_FALSE(sealVortexProgram(program));

  auto sealed = sealVortexProgram(addition());
  ASSERT_TRUE(sealed) << sealed.error();
  auto body = xanadu::bencode::decode(
                  xanadu::bencode::decode(*sealed).find("body")->asString())
                  .asDict();
  auto instructions = body.at("instructions").asList();
  auto first        = instructions.front().asDict();
  first["opcode"] =
      Wire::integer(static_cast<std::int64_t>(OpcodeKind::ChoiceDepth));
  instructions.front() = Wire::dict(std::move(first));
  body["instructions"] = Wire::list(std::move(instructions));
  EXPECT_FALSE(evaluate(Frontend::Vortex,
                        replaceBody(*sealed, Wire::dict(std::move(body))),
                        budget));
}

TEST(MeshVortexArtifactTest, CyclicProgramsConsumeExplicitCycleBudget) {
  VortexProgram program{.cells        = {{}, {VortexReference{0}, {}}},
                        .instructions = {{0, OpcodeKind::Jump, {1}, {}, {}}},
                        .entry        = 0,
                        .outputs      = {}};
  auto bytes = sealVortexProgram(program);
  ASSERT_TRUE(bytes) << bytes.error();
  auto limited      = budget;
  limited.maxCycles = 50;
  auto result       = evaluate(Frontend::Vortex, *bytes, limited);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("budget"), std::string::npos);
}

TEST(MeshVortexArtifactTest, DecodeNestingCellsAndBytesAreBounded) {
  auto bytes = sealVortexProgram(addition());
  ASSERT_TRUE(bytes) << bytes.error();
  EXPECT_FALSE(evaluate(Frontend::Vortex, *bytes, budget, nullptr, nullptr,
                        SyntaxLimits{1024 * 1024, 2, 64}));
  EXPECT_FALSE(evaluate(Frontend::Vortex, *bytes, budget, nullptr, nullptr,
                        SyntaxLimits{10, 512, 64}));
  const auto nested = std::string(10000, 'l') + std::string(10000, 'e');
  EXPECT_FALSE(evaluate(Frontend::Vortex, nested, budget));
  auto fields      = xanadu::bencode::decode(*bytes).asDict();
  fields["body"]   = Wire::string(nested);
  fields["sha256"] = Wire::string(xanadu::sha256Hex(nested));
  EXPECT_FALSE(evaluate(Frontend::Vortex,
                        Wire::dict(std::move(fields)).encode(), budget));
}

TEST(MeshVortexArtifactTest, ReservedExecutionEdgesAndIntegerOverflowRefuse) {
  auto program = addition();
  program.cells[2].links.push_back({"d.spin", 0});
  EXPECT_FALSE(sealVortexProgram(program));
  program                = addition();
  program.cells[2].value = CellValue{std::numeric_limits<std::int64_t>::max()};
  auto bytes             = sealVortexProgram(program);
  ASSERT_TRUE(bytes) << bytes.error();
  auto result = evaluate(Frontend::Vortex, *bytes, budget);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("overflow"), std::string::npos);
}
} // namespace
} // namespace zigzag::vortex::mesh
