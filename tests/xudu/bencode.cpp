/**
 * @file bencode.cpp
 * @brief The encoding a torrent's identity is computed over.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include <xudu/core/bencode.hpp>

namespace {

using xudu::bencode::decode;
using xudu::bencode::Value;

TEST(BencodeTest, integers) {
  EXPECT_EQ(decode("i3e").asInteger(), 3);
  EXPECT_EQ(decode("i-3e").asInteger(), -3);
  EXPECT_EQ(decode("i0e").asInteger(), 0);
}

TEST(BencodeTest, strings) {
  EXPECT_EQ(decode("4:spam").asString(), "spam");
  EXPECT_EQ(decode("0:").asString(), "");
  // Byte strings, not text: a length prefix means the bytes may be anything.
  EXPECT_EQ(decode(std::string("3:a\0b", 5)).asString(), std::string("a\0b", 3));
}

TEST(BencodeTest, lists) {
  const auto value = decode("l4:spam4:eggse");
  ASSERT_EQ(value.asList().size(), 2U);
  EXPECT_EQ(value.asList()[0].asString(), "spam");
  EXPECT_EQ(value.asList()[1].asString(), "eggs");
}

TEST(BencodeTest, dictionaries) {
  const auto value = decode("d3:cow3:moo4:spam4:eggse");
  EXPECT_EQ(value.find("cow")->asString(), "moo");
  EXPECT_EQ(value.find("spam")->asString(), "eggs");
  EXPECT_EQ(value.find("absent"), nullptr);
}

TEST(BencodeTest, nesting) {
  const auto value = decode("d4:listli1ei2eee");
  EXPECT_EQ(value.find("list")->asList().size(), 2U);
}

TEST(BencodeTest, wellFormedInputRoundTrips) {
  // Byte for byte, which is what the info hash depends on.
  for (const auto *text : {"i42e", "4:spam", "l4:spami7ee",
                           "d3:cow3:moo4:spam4:eggse", "de", "le"}) {
    EXPECT_EQ(decode(text).encode(), text) << text;
  }
}

TEST(BencodeTest, theRawEncodingIsKept) {
  // What the info hash is taken over. It has to be the bytes as they were
  // found, so this is the substring of the input, not a re-encoding.
  const std::string input = "d4:infod3:key5:valueee";
  EXPECT_EQ(decode(input).find("info")->rawEncoding(), "d3:key5:valuee");
}

TEST(BencodeTest, keysOutOfOrderAreRefused) {
  // Two encodings of one dictionary would be two info hashes for one torrent,
  // so an input that breaks the ordering rule cannot be trusted to identify
  // anything and is rejected rather than quietly sorted.
  EXPECT_THROW((void)decode("d4:spam4:eggs3:cow3:mooe"), std::runtime_error);
}

TEST(BencodeTest, duplicateKeysAreRefused) {
  // A duplicate is a special case of out-of-order: the second key is not
  // greater than the first.
  EXPECT_THROW((void)decode("d3:cow3:moo3:cow3:mooe"), std::runtime_error);
}

TEST(BencodeTest, leadingZerosAreRefused) {
  // "i03e" and "i3e" would be two spellings of one number, and so two hashes
  // for one torrent.
  EXPECT_THROW((void)decode("i03e"), std::runtime_error);
  EXPECT_THROW((void)decode("i-0e"), std::runtime_error);
  EXPECT_THROW((void)decode("i-03e"), std::runtime_error);
}

TEST(BencodeTest, trailingBytesAreRefused) {
  // A torrent file with something after its top-level dictionary is not a
  // torrent file, and the spec asks that the hash be taken only from input
  // that was fully validated.
  EXPECT_THROW((void)decode("i1ei2e"), std::runtime_error);
  EXPECT_THROW((void)decode("4:spamX"), std::runtime_error);
}

TEST(BencodeTest, truncatedInputIsRefused) {
  for (const auto *text : {"i3", "l4:spam", "d3:cow3:moo", "5:abc", "i", "d"}) {
    EXPECT_THROW((void)decode(text), std::runtime_error) << text;
  }
}

TEST(BencodeTest, nonsenseIsRefused) {
  for (const auto *text : {"", "x", "d3:cowi1ee4:spam", "di1ei2ee"}) {
    EXPECT_THROW((void)decode(text), std::runtime_error) << text;
  }
}

TEST(BencodeTest, anOverlongNumberIsRefusedRatherThanWrapping) {
  EXPECT_THROW((void)decode("i99999999999999999999999e"), std::runtime_error);
}

TEST(BencodeTest, aStringLengthPastTheEndIsRefused) {
  EXPECT_THROW((void)decode("100:short"), std::runtime_error);
}

TEST(BencodeTest, askingForTheWrongTypeThrows) {
  EXPECT_THROW((void)decode("i1e").asString(), std::runtime_error);
  EXPECT_THROW((void)decode("4:spam").asInteger(), std::runtime_error);
  EXPECT_THROW((void)decode("le").asDict(), std::runtime_error);
}

TEST(BencodeTest, builtValuesEncode) {
  EXPECT_EQ(Value::integer(-7).encode(), "i-7e");
  EXPECT_EQ(Value::string("hi").encode(), "2:hi");
  EXPECT_EQ(Value::list({Value::integer(1)}).encode(), "li1ee");
  // Built out of order; the encoding sorts them, because that is the rule.
  EXPECT_EQ(Value::dict({{"b", Value::integer(2)}, {"a", Value::integer(1)}})
                .encode(),
            "d1:ai1e1:bi2ee");
}

} // namespace

// vi: set sw=2 sts=2 ts=2 et:
