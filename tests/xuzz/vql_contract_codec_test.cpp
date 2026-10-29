/**
 * @file vql_contract_codec_test.cpp
 * @brief Tests portable VQL contract descriptors.
 */
#include <gtest/gtest.h>

#include "common/xanadu/vql/contract_codec.hpp"
#include "common/xanadu/vql/parser.hpp"

namespace {

using namespace xanadu::vql;

TypeExpr parseType(std::string_view syntax) {
  const std::string source =
      "def contract_value($value: " + std::string(syntax) + ") := $value";
  const auto query = Parser(source).parseQuery();
  return *query.defs.front().params.front().type;
}

TEST(VQLContractCodecTest, CanonicalBasicTypeUsesItsSemanticKind) {
  TypeExpr type{.unionType = UnionType{
                    .members = {PrimaryType{
                        .kind = BasicType{.kind = BasicType::Kind::Int}}}}};
  const auto descriptor = encodeTypeDescriptor(type);
  EXPECT_EQ(descriptor, "VQL_CONTRACT/2\nInt");
  EXPECT_EQ(std::get<BasicType>(
                decodeTypeDescriptor(descriptor).unionType.members.front().kind)
                .kind,
            BasicType::Kind::Int);
}

TEST(VQLContractCodecTest, RoundTripsNestedUnionOptionalAndGenericTypes) {
  const auto source = parseType("Stream<(Cell? | Dim)?> | Stream<Int | Float>");
  const auto encoded = encodeTypeDescriptor(source);
  EXPECT_TRUE(encoded.starts_with("VQL_CONTRACT/2\n"));

  const auto decoded = decodeTypeDescriptor(encoded);
  EXPECT_EQ(encodeTypeDescriptor(decoded), encoded);
  ASSERT_EQ(decoded.unionType.members.size(), 2u);
  ASSERT_TRUE(std::holds_alternative<GenericType>(
      decoded.unionType.members.front().kind));
}

TEST(VQLContractCodecTest, RoundTripsRefinementAndShapeBindingsAndDefaults) {
  const auto source = parseType(
      "Cell { ~-d.link: $target: Stream<Int?> := $fallback, "
      "+d.title: $title: String := \"line\\nquote\\\"\\\\\", .: $self }");
  const auto encoded = encodeTypeDescriptor(source);
  const auto decoded = decodeTypeDescriptor(encoded);
  EXPECT_EQ(encodeTypeDescriptor(decoded), encoded);

  const auto &primary = decoded.unionType.members.front();
  ASSERT_TRUE(primary.refinement.has_value());
  const auto &fields = primary.refinement->pattern.fields;
  ASSERT_EQ(fields.size(), 3u);
  EXPECT_EQ(fields[0].varName, "target");
  ASSERT_TRUE(fields[0].type);
  ASSERT_TRUE(fields[0].defaultValue);
  EXPECT_EQ(std::get<std::string>(fields[0].defaultValue->kind), "fallback");
  EXPECT_EQ(fields[1].varName, "title");
  EXPECT_EQ(fields[2].varName, "self");
  EXPECT_TRUE(fields[2].isContext);
}

TEST(VQLContractCodecTest, RoundTripsSimplePathDefaultsAndFullWidthNames) {
  const auto source  = parseType("{ +d.default: $v := ##/+d.next/-d.prev }");
  const auto encoded = encodeTypeDescriptor(source);
  const auto decoded = decodeTypeDescriptor(encoded);
  EXPECT_EQ(encodeTypeDescriptor(decoded), encoded);
}

TEST(VQLContractCodecTest, RoundTripsTypedContextFields) {
  const auto alias        = parseType("Cell { .: $self: String }");
  const auto encodedAlias = encodeTypeDescriptor(alias);
  EXPECT_EQ(encodedAlias, "VQL_CONTRACT/2\nCell{.: $self: String}");
  const auto decodedAlias = decodeTypeDescriptor(encodedAlias);
  ASSERT_TRUE(decodedAlias.unionType.members.front().refinement);
  const auto &aliasFields =
      decodedAlias.unionType.members.front().refinement->pattern.fields;
  ASSERT_EQ(aliasFields.size(), 1u);
  EXPECT_TRUE(aliasFields.front().isContext);
  EXPECT_EQ(aliasFields.front().varName, "self");
  ASSERT_TRUE(aliasFields.front().type);
  EXPECT_EQ(encodeTypeDescriptor(decodedAlias), encodedAlias);

  const auto unaliased = parseType("Cell { .: Int }");
  EXPECT_EQ(encodeTypeDescriptor(unaliased), "VQL_CONTRACT/2\nCell{.: Int}");
}

TEST(VQLContractCodecTest, RejectsMalformedAndUnknownVersionsByName) {
  EXPECT_THROW((void)decodeTypeDescriptor("OTHER/1\nCell"),
               ContractDescriptorError);
  try {
    (void)decodeTypeDescriptor("VQL_CONTRACT/9\nCell");
    FAIL() << "expected version refusal";
  } catch (const ContractDescriptorError &error) {
    EXPECT_NE(std::string(error.what()).find("VQL_CONTRACT/9"),
              std::string::npos);
  }
  EXPECT_THROW((void)decodeTypeDescriptor("VQL_CONTRACT/9\nCell |"),
               ContractDescriptorError);
  EXPECT_THROW((void)decodeTypeDescriptor("VQL_CONTRACT/1\nCell"),
               ContractDescriptorError);
}

TEST(VQLContractCodecTest, RejectsUnsupportedLazyDefaultExpressionsClearly) {
  const auto source = parseType("{ +d.value: Int := computed(1) }");
  try {
    (void)encodeTypeDescriptor(source);
    FAIL() << "expected unsupported expression refusal";
  } catch (const ContractDescriptorError &error) {
    EXPECT_NE(std::string(error.what()).find("default expression"),
              std::string::npos);
  }
}

TEST(VQLContractCodecTest, RejectsTrailingPayloadAndBoundsImportedSyntax) {
  EXPECT_THROW((void)decodeTypeDescriptor("VQL_CONTRACT/2\nInt) := 1; //"),
               ContractDescriptorError);
  EXPECT_THROW(
      (void)decodeTypeDescriptor("VQL_CONTRACT/2\nInt) := $value; ## //"),
      ContractDescriptorError);
  EXPECT_THROW((void)decodeTypeDescriptor("VQL_CONTRACT/2\nStream<Stream<Int>>",
                                          {.maxBytes = 1024, .maxNesting = 1}),
               ContractDescriptorError);
  EXPECT_THROW(
      (void)decodeTypeDescriptor("VQL_CONTRACT/2\nInt", {.maxBytes = 8}),
      ContractDescriptorError);
}

} // namespace
