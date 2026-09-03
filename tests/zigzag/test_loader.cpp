/**
 * @file test_loader.cpp
 * @brief Integration tests for the ZigZag Slice YAML loader using rapidyaml.
 */
#include <gtest/gtest.h>

#include <string>

#include "zigzag/core/zzstructure.hpp"
#include "zigzag/core/zzstructure_loader.hpp"

using namespace zigzag;

TEST(ZzLoaderTest, ParseMinimalValidSlice) {
  const std::string yaml = R"(
zzstructure:
  focus: 1
  cells:
    - id: 1
      text: "Root"
)";

  const auto doc = parseZzStructure(yaml, "test");
  ASSERT_TRUE(doc.has_value());
  EXPECT_EQ(doc->focus, 1U);
  ASSERT_EQ(doc->cells.size(), 1U);
  EXPECT_EQ(doc->cells.at(1).text_data, "Root");
}

TEST(ZzLoaderTest, ParseFullSliceMetadataAndLinks) {
  const std::string yaml = R"(
zzstructure:
  meta:
    name: "Test Slice"
    version: "2.0"
    author: "Ted"
    tags: [xanadu, zigzag]

  focus: 1

  view:
    x_dimension: d.1
    y_dimension: d.2
    z_dimension: d.3

  dimensions:
    d.1:
      label: "Sequence"
      color: "#ff0000"
      spacing: 2.5
    d.2:
      label: "Detail"
      color: "#00ff00"
      spacing: 1.5

  scene:
    background_color: "#111122"
    focus_color: "#ffcc00"
    focus_scale: 1.5
    cell_radius: 0.4

  cells:
    - id: 1
      text: "Chapter 1"
      type: chapter
      dimensions: { d.1: 2, d.2: { pos: 3, neg: 0 } }

    - id: 2
      text: "Chapter 2"
      type: chapter

    - id: 3
      text: "Detail 1.1"
      type: detail
)";

  const auto doc = parseZzStructure(yaml, "test");
  ASSERT_TRUE(doc.has_value());
  EXPECT_EQ(doc->meta.name, "Test Slice");
  EXPECT_EQ(doc->meta.version, "2.0");
  EXPECT_EQ(doc->meta.author, "Ted");
  ASSERT_EQ(doc->meta.tags.size(), 2U);
  EXPECT_EQ(doc->meta.tags[0], "xanadu");

  EXPECT_EQ(doc->view.x_dimension, "d.1");
  EXPECT_EQ(doc->view.y_dimension, "d.2");

  EXPECT_FLOAT_EQ(doc->dimension_meta.at("d.1").spacing, 2.5F);
  EXPECT_FLOAT_EQ(doc->dimension_meta.at("d.1").color.r, 1.0F);

  EXPECT_FLOAT_EQ(doc->scene.focus_scale, 1.5F);
  EXPECT_FLOAT_EQ(doc->scene.cell_radius, 0.4F);

  ASSERT_EQ(doc->cells.size(), 3U);
  // Backlink check: cell 2 should have d.1 neg = 1 auto-derived
  EXPECT_EQ(doc->cells.at(1).dimensions.at("d.1").pos, 2U);
  EXPECT_EQ(doc->cells.at(2).dimensions.at("d.1").neg, 1U);
}

TEST(ZzLoaderTest, RejectMalformedYaml) {
  const std::string badYaml = R"(
zzstructure:
  focus: [unclosed list
)";

  const auto doc = parseZzStructure(badYaml, "test");
  ASSERT_FALSE(doc.has_value());
  EXPECT_EQ(doc.error().kind, LoadError::Kind::MalformedYaml);
}

TEST(ZzLoaderTest, RejectSchemaViolationMissingFocus) {
  const std::string noFocus = R"(
zzstructure:
  cells:
    - id: 1
      text: "Hello"
)";

  const auto doc = parseZzStructure(noFocus, "test");
  ASSERT_FALSE(doc.has_value());
  EXPECT_EQ(doc.error().kind, LoadError::Kind::SchemaViolation);
}

TEST(ZzLoaderTest, RejectDanglingFocus) {
  const std::string danglingFocus = R"(
zzstructure:
  focus: 999
  cells:
    - id: 1
      text: "Hello"
)";

  const auto doc = parseZzStructure(danglingFocus, "test");
  ASSERT_FALSE(doc.has_value());
  EXPECT_EQ(doc.error().kind, LoadError::Kind::DanglingFocus);
}

TEST(ZzLoaderTest, HandleDuplicateCellIds) {
  const std::string dupeYaml = R"(
zzstructure:
  focus: 1
  cells:
    - id: 1
      text: "Original"
    - id: 1
      text: "Replacement"
)";

  const auto doc = parseZzStructure(dupeYaml, "test");
  ASSERT_TRUE(doc.has_value());
  ASSERT_EQ(doc->cells.size(), 1U);
  EXPECT_EQ(doc->cells.at(1).text_data, "Replacement");
}

TEST(ZzLoaderTest, ParsePrefletChainFromYaml) {
  const std::string prefletYaml = R"(
zzstructure:
  focus: 1
  cells:
    - id: 1
      text: "Chapter with link"
      dimensions: { d.preflet: 10 }

    - id: 10
      type: preflet_resource
      text: "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"
      dimensions: { d.preflet: 11 }

    - id: 11
      type: preflet_version
      text: "3.5"
)";

  const auto doc = parseZzStructure(prefletYaml, "test");
  ASSERT_TRUE(doc.has_value());
  ASSERT_TRUE(doc->cells.at(1).preflet.has_value());
  EXPECT_EQ(doc->cells.at(1).preflet->version, "3.5");
}

TEST(ZzLoaderTest, ParseMediaAndImageCells) {
  const std::string mediaYaml = R"(
zzstructure:
  focus: 1
  cells:
    - id: 1
      text: "Text Cell"
      dimensions: { d.1: 2 }

    - id: 2
      text: "Image Cell"
      type: image
      mime_type: "image/png"
      media_path: "assets/textures/diagram.png"
)";

  const auto doc = parseZzStructure(mediaYaml, "test");
  ASSERT_TRUE(doc.has_value());
  ASSERT_EQ(doc->cells.size(), 2U);

  const auto &cell1 = doc->cells.at(1);
  EXPECT_FALSE(cell1.isMedia());
  EXPECT_FALSE(cell1.isImage());

  const auto &cell2 = doc->cells.at(2);
  EXPECT_TRUE(cell2.isMedia());
  EXPECT_TRUE(cell2.isImage());
  EXPECT_EQ(cell2.mime_type, "image/png");
  EXPECT_EQ(cell2.media_path, "assets/textures/diagram.png");
}
