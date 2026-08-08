/**
 * @file torrent_data.hpp
 * @brief Torrent test vectors, built by an independent implementation.
 *
 * The torrent files here were produced by a bencoder written in Python and the
 * expected info hashes computed there with hashlib. Checking this codebase
 * against itself would prove only that it is self-consistent, which is
 * worthless for an identity every reference depends on: a systematic mistake
 * in the encoding would produce a stable, agreed-upon, wrong hash.
 *
 * They are given as hex rather than as string literals with escapes in them. A
 * piece hash is twenty arbitrary bytes and some of them are NUL, which a
 * std::string built from a `const char *` truncates at -- silently, producing
 * a torrent that is merely too short.
 */
#ifndef XUDU_TESTS_TORRENT_DATA_H
#define XUDU_TESTS_TORRENT_DATA_H

#include <cstddef>
#include <string>
#include <string_view>

namespace xudu_test {

inline std::string fromHex(const std::string_view text) {
  const auto digit = [](const char chr) {
    if (chr >= '0' && chr <= '9') {
      return chr - '0';
    }
    return (chr - 'a') + 10;
  };
  std::string out;
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
    out.push_back(static_cast<char>((digit(text[i]) << 4) | digit(text[i + 1])));
  }
  return out;
}

/// One file, 62 bytes, 32-byte pieces -- so two pieces and the second short.
inline const std::string singleFileTorrent = fromHex(
    "64383a616e6e6f756e636533313a687474703a2f2f747261636b65722e696e76616c"
    "69642f616e6e6f756e6365343a696e666f64363a6c656e67746869363265343a6e61"
    "6d65373a666f782e74787431323a7069656365206c656e67746869333265363a7069"
    "6563657334303a76d837a234e4e611cd5bcb78e50a3d37d6d5c5aca93a2ca1827934"
    "66c81fa280c44f74093f5333b56565");
inline const std::string singleFileText =
    "The quick brown fox jumped over the lazy dog. And many more...";
inline constexpr const char *singleFileHash =
    "f38d28d42380fd20594a3fe9b7c6a012bc818650";

/// Two files, 27 and 28 bytes, 32-byte pieces -- so the first piece straddles
/// the boundary between them.
inline const std::string multiFileTorrent = fromHex(
    "64383a616e6e6f756e636533313a687474703a2f2f747261636b65722e696e76616c"
    "69642f616e6e6f756e6365343a696e666f64353a66696c65736c64363a6c656e6774"
    "6869323765343a706174686c373a6f6e652e747874656564363a6c656e6774686932"
    "3865343a706174686c333a737562373a74776f2e747874656565343a6e616d65343a"
    "7061697231323a7069656365206c656e67746869333265363a70696563657334303a"
    "6f169df74656acdcf64b2f14154d43fa18e898fc96d36647fbe9e4a671116c18523c"
    "270070e6848b6565");
inline const std::string multiFileFirst  = "Hello from the first file. ";
inline const std::string multiFileSecond = "And the second file follows.";
inline const std::string multiFileWhole  = multiFileFirst + multiFileSecond;
inline constexpr const char *multiFileHash =
    "dfb9ceab43e464f31c0e95f9240c985bfe46958d";

} // namespace xudu_test

#endif // XUDU_TESTS_TORRENT_DATA_H
// vi: set sw=2 sts=2 ts=2 et:
