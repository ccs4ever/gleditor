#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "common/xanadu/provenance.hpp"
#include "common/xanadu/vortex/stdlib_image_format.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace {

using namespace zigzag::vortex;
using ::testing::HasSubstr;
using ::testing::StartsWith;

TEST(StdlibImageFormatTest, EpochCoversOrderedLabelsAndDecodeBoundaries) {
  const auto abi = stdlibImageAbiText();
  EXPECT_THAT(abi, StartsWith("vortex-stdlib-abi-v1\nopcode\t0\tAdd\n"));
  EXPECT_THAT(abi, HasSubstr("verb\t0\tmake\nverb\t1\tsetLink\n"));
  EXPECT_THAT(abi, HasSubstr("verb\t2\tsetValue\nverb\t3\tsplice\n"));
  EXPECT_THAT(abi, HasSubstr("last-content-opcode\t" +
                             std::to_string(static_cast<unsigned>(
                                 kLastContentDecodableOpcode)) +
                             "\n"));
  EXPECT_THAT(abi,
              HasSubstr("last-opcode\t" +
                        std::to_string(static_cast<unsigned>(kLastOpcodeKind)) +
                        "\n"));
  EXPECT_EQ(stdlibImageOpcodeCount(),
            static_cast<std::size_t>(kLastOpcodeKind) + 1);
  EXPECT_EQ(stdlibImageVerbCount(), 4U);

  const auto epoch = stdlibImageEpoch();
  EXPECT_EQ(epoch.size(), 64U);
  EXPECT_EQ(epoch, xanadu::sha256Hex(abi));
  EXPECT_EQ(epoch, stdlibImageEpoch());
  EXPECT_TRUE(std::ranges::all_of(epoch, [](const char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  }));
}

TEST(StdlibImageFormatTest, BoundaryAndLabelChangesChangeTheDigest) {
  const auto epoch    = stdlibImageEpoch();
  auto abi            = stdlibImageAbiText();
  const auto boundary = abi.find("last-content-opcode\t");
  ASSERT_NE(boundary, std::string::npos);
  abi.replace(boundary, abi.find('\n', boundary) - boundary,
              "last-content-opcode\t0");
  EXPECT_NE(xanadu::sha256Hex(abi), epoch);

  abi              = stdlibImageAbiText();
  const auto label = abi.find("\tAdd\n");
  ASSERT_NE(label, std::string::npos);
  abi.replace(label, 5, "\tChangedAdd\n");
  EXPECT_NE(xanadu::sha256Hex(abi), epoch);
}

} // namespace
