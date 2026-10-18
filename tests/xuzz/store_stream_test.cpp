#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

#include <common/xanadu/store_stream.hpp>

namespace {

std::string bytes(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot open test store file");
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

TEST(StoreStreamTest, preservesBothNativeFilesByteForByte) {
  const auto source =
      std::filesystem::path("tests/samples/xudu/core_hypertext/xanadoc_a");
  std::stringstream wire(std::ios::in | std::ios::out | std::ios::binary);
  xanadu::writeStoreStream(source, wire);
  wire.seekg(0);
  xanadu::TemporaryStreamStore imported(wire);
  EXPECT_EQ(bytes(source / "ops.nodes"), bytes(imported.path() / "ops.nodes"));
  EXPECT_EQ(bytes(source / "store.tables"),
            bytes(imported.path() / "store.tables"));
}

TEST(StoreStreamTest, refusesIncompleteArchive) {
  std::stringstream empty(std::ios::in | std::ios::out | std::ios::binary);
  empty << std::string(1024, '\0');
  empty.seekg(0);
  EXPECT_THROW(xanadu::TemporaryStreamStore imported(empty),
               std::runtime_error);
}

} // namespace
