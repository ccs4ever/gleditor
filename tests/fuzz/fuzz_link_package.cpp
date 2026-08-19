/**
 * @file fuzz_link_package.cpp
 * @brief LLVM libFuzzer harness for link package, publication, and blessing decoding.
 */
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <xudu/core/bencode.hpp>
#include <xudu/core/blessing.hpp>
#include <xudu/core/link_package.hpp>
#include <xudu/core/mutable_link.hpp>
#include <xudu/core/publication.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (0 == size) {
    return 0;
  }
  const std::string_view sv(reinterpret_cast<const char *>(data), size);

  try {
    static_cast<void>(xudu::bencode::decode(sv));
  } catch (...) {
  }

  static_cast<void>(xudu::decodeLinkPackage(sv));
  static_cast<void>(xudu::decodeBlessing(sv));
  static_cast<void>(xudu::decodePublication(sv));
  static_cast<void>(xudu::decodeMutablePointer(sv));

  try {
    static_cast<void>(xudu::MutableLink::parse(sv));
  } catch (...) {
  }

  return 0;
}
