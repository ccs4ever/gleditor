/**
 * @file fuzz_link_package.cpp
 * @brief LLVM libFuzzer harness for link package, publication, and blessing
 * decoding.
 */
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/blessing.hpp"
#include "common/xanadu/link_package.hpp"
#include "common/xanadu/mutable_link.hpp"
#include "common/xanadu/publication.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (0 == size) {
    return 0;
  }
  const std::string_view sv(reinterpret_cast<const char *>(data), size);

  try {
    std::ignore = xanadu::bencode::decode(sv);
  } catch (...) {
  }

  std::ignore = xanadu::decodeLinkPackage(sv);
  std::ignore = xanadu::decodeBlessing(sv);
  std::ignore = xanadu::decodePublication(sv);
  std::ignore = xanadu::decodeMutablePointer(sv);

  try {
    std::ignore = xanadu::MutableLink::parse(sv);
  } catch (...) {
  }

  return 0;
}
