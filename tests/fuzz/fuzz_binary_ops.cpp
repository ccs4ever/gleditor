/**
 * @file fuzz_binary_ops.cpp
 * @brief LLVM libFuzzer harness for binary and text ops spool decoding.
 */
#include <cstddef>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>

#include "common/xanadu/binary_ops.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (0 == size) {
    return 0;
  }
  const std::string input(reinterpret_cast<const char *>(data), size);

  std::istringstream in1(input);
  std::vector<xanadu::OpRecord> ops1;
  try {
    xanadu::readOpsSpool(in1, ops1);
  } catch (...) {
  }

  std::istringstream in2(input);
  std::vector<xanadu::OpRecord> ops2;
  try {
    xanadu::readBinaryOpsSpool(in2, ops2);
  } catch (...) {
  }

  std::istringstream in3(input);
  std::vector<xanadu::OpRecord> ops3;
  try {
    xanadu::readOsmicTextOpsSpool(in3, ops3);
  } catch (...) {
  }

  std::istringstream in4(input);
  std::uint64_t val = 0;
  try {
    static_cast<void>(xanadu::readVarint(in4, val));
  } catch (...) {
  }

  std::istringstream in5(input);
  xanadu::MicroversionId id;
  try {
    static_cast<void>(xanadu::readMicroversionId(in5, id));
  } catch (...) {
  }

  return 0;
}
