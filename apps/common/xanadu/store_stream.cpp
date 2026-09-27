#include "store_stream.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace xanadu {
namespace {

constexpr std::size_t blockSize = 512;
using Block                     = std::array<char, blockSize>;

void putOctal(Block &block, const std::size_t offset, const std::size_t width,
              const std::uint64_t value) {
  if (width < 2 || width > 21) throw std::logic_error("invalid tar field");
  char field[22]{};
  const auto count =
      std::snprintf(field, sizeof(field), "%0*llo", static_cast<int>(width - 1),
                    static_cast<unsigned long long>(value));
  if (count < 0 || static_cast<std::size_t>(count) >= width) {
    throw std::runtime_error("store file exceeds ustar size limit");
  }
  std::memcpy(block.data() + offset, field, static_cast<std::size_t>(count));
}

std::uint64_t parseOctal(const Block &block, const std::size_t offset,
                         const std::size_t width) {
  const char *first = block.data() + offset;
  const char *last  = first + width;
  while (first < last && (*first == ' ' || *first == '\0')) ++first;
  const char *end = first;
  while (end < last && *end >= '0' && *end <= '7') ++end;
  std::uint64_t value = 0;
  if (first == end || std::from_chars(first, end, value, 8).ec != std::errc{}) {
    throw std::runtime_error("invalid tar numeric field");
  }
  while (end < last && (*end == ' ' || *end == '\0')) ++end;
  if (end != last) throw std::runtime_error("invalid tar numeric field");
  return value;
}

std::uint64_t checksum(const Block &block) {
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < block.size(); ++i) {
    result += (i >= 148 && i < 156) ? static_cast<unsigned char>(' ')
                                    : static_cast<unsigned char>(block[i]);
  }
  return result;
}

void transfer(std::istream &in, std::ostream &out, std::uint64_t bytes) {
  std::array<char, 65536> buffer{};
  while (bytes > 0) {
    const auto chunk = static_cast<std::streamsize>(
        std::min<std::uint64_t>(bytes, buffer.size()));
    in.read(buffer.data(), chunk);
    if (in.gcount() != chunk)
      throw std::runtime_error("truncated store stream");
    out.write(buffer.data(), chunk);
    if (!out) throw std::runtime_error("cannot write store stream");
    bytes -= static_cast<std::uint64_t>(chunk);
  }
}

void skipPadding(std::istream &in, const std::uint64_t size) {
  const auto padding = (blockSize - size % blockSize) % blockSize;
  Block discarded{};
  in.read(discarded.data(), static_cast<std::streamsize>(padding));
  if (in.gcount() != static_cast<std::streamsize>(padding)) {
    throw std::runtime_error("truncated store stream padding");
  }
}

void writeEntry(const std::filesystem::path &directory, const char *name,
                std::ostream &out) {
  const auto path = directory / name;
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot open native store file: " + path.string());
  const auto size = std::filesystem::file_size(path);
  Block header{};
  std::memcpy(header.data(), name, std::strlen(name));
  putOctal(header, 100, 8, 0644);
  putOctal(header, 108, 8, 0);
  putOctal(header, 116, 8, 0);
  putOctal(header, 124, 12, size);
  putOctal(header, 136, 12, 0);
  header[156] = '0';
  std::memcpy(header.data() + 257, "ustar", 5);
  std::memcpy(header.data() + 263, "00", 2);
  putOctal(header, 148, 8, checksum(header));
  header[155] = ' ';
  out.write(header.data(), header.size());
  transfer(file, out, size);
  Block padding{};
  const auto count = (blockSize - size % blockSize) % blockSize;
  out.write(padding.data(), static_cast<std::streamsize>(count));
  if (!out) throw std::runtime_error("cannot write store stream");
}

} // namespace

void writeStoreStream(const std::filesystem::path &directory,
                      std::ostream &out) {
  writeEntry(directory, "ops.nodes", out);
  writeEntry(directory, "store.tables", out);
  const Block end{};
  out.write(end.data(), end.size());
  out.write(end.data(), end.size());
  if (!out) throw std::runtime_error("cannot finish store stream");
}

void readStoreStream(std::istream &in, const std::filesystem::path &directory) {
  std::filesystem::create_directories(directory);
  bool gotOps    = false;
  bool gotTables = false;
  for (;;) {
    Block header{};
    in.read(header.data(), header.size());
    if (in.gcount() != static_cast<std::streamsize>(header.size())) {
      throw std::runtime_error("truncated store stream header");
    }
    if (header == Block{}) break;
    if (std::memcmp(header.data() + 257, "ustar", 5) != 0 ||
        parseOctal(header, 148, 8) != checksum(header)) {
      throw std::runtime_error("invalid store tar header");
    }
    const std::string name(header.data(), ::strnlen(header.data(), 100));
    if (header[156] != '0' && header[156] != '\0') {
      throw std::runtime_error("store stream contains a non-file entry");
    }
    bool *found = nullptr;
    if (name == "ops.nodes" || name == "./ops.nodes") {
      found = &gotOps;
    } else if (name == "store.tables" || name == "./store.tables") {
      found = &gotTables;
    } else {
      throw std::runtime_error("unexpected file in store stream: " + name);
    }
    if (*found)
      throw std::runtime_error("duplicate native store file: " + name);
    *found          = true;
    const auto size = parseOctal(header, 124, 12);
    const auto target =
        directory / (found == &gotOps ? "ops.nodes" : "store.tables");
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot create native store file");
    transfer(in, file, size);
    file.close();
    if (!file) throw std::runtime_error("cannot finish native store file");
    skipPadding(in, size);
  }
  if (!gotOps || !gotTables) {
    throw std::runtime_error("store stream needs ops.nodes and store.tables");
  }
}

TemporaryStreamStore::TemporaryStreamStore() {
  auto pattern =
      (std::filesystem::temp_directory_path() / "xanadu-store-stream-XXXXXX")
          .string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    throw std::runtime_error("cannot create temporary store directory");
  }
  path_ = pattern;
}

TemporaryStreamStore::TemporaryStreamStore(std::istream &in)
    : TemporaryStreamStore() {
  try {
    readStoreStream(in, path_);
  } catch (...) {
    std::filesystem::remove_all(path_);
    throw;
  }
}

TemporaryStreamStore::~TemporaryStreamStore() {
  if (!path_.empty()) {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
}

} // namespace xanadu
