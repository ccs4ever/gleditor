/**
 * @file fuzz_binary_ops.cpp
 * @brief LLVM libFuzzer harness for binary and text ops spool decoding.
 */
#include <cstddef>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>

#include <xudu/core/binary_ops.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (0 == size) {
    return 0;
  }
  const std::string input(reinterpret_cast<const char *>(data), size);

  std::istringstream in1(input);
  std::vector<xudu::OpRecord> ops1;
  try {
    xudu::readOpsSpool(in1, ops1);
  } catch (...) {
  }

  std::istringstream in2(input);
  std::vector<xudu::OpRecord> ops2;
  try {
    xudu::readBinaryOpsSpool(in2, ops2);
  } catch (...) {
  }

  std::istringstream in3(input);
  std::vector<xudu::OpRecord> ops3;
  try {
    xudu::readOsmicTextOpsSpool(in3, ops3);
  } catch (...) {
  }

  std::istringstream in4(input);
  std::uint64_t val = 0;
  try {
    static_cast<void>(xudu::readVarint(in4, val));
  } catch (...) {
  }

  std::istringstream in5(input);
  xudu::MicroversionId id;
  try {
    static_cast<void>(xudu::readMicroversionId(in5, id));
  } catch (...) {
  }

  return 0;
}
