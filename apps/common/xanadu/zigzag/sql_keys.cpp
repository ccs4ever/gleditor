/**
 * @file sql_keys.cpp
 * @brief The normative ExternalKey/value encodings for SqlLens
 * (design/sql-to-zigzag.md §4.1).
 */
#include "common/xanadu/zigzag/sql_keys.hpp"

#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/xanadu/provenance.hpp"
#include "common/xanadu/scalar.hpp"

namespace zigzag {

namespace {

constexpr char kHex[] = "0123456789abcdef";

} // namespace

std::string sqlKeyEncode(std::string_view component) {
  std::string out;
  out.reserve(component.size());
  for (const unsigned char c : component) {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '~';
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::string sqlHashHex(std::string_view data) {
  return xanadu::sha256Hex(data);
}

std::string encodeSqlValue(const SqlValue &v) {
  return std::visit(
      [](const auto &x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "n:";
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return "i:" + std::to_string(x);
        } else if constexpr (std::is_same_v<T, double>) {
          // IEEE-754 bit pattern, canonical (NaN folds to one pattern).
          char buf[17];
          std::snprintf(buf, sizeof buf, "%016" PRIx64,
                        xanadu::canonicalDoubleBits(x));
          return "f:" + std::string(buf, 16);
        } else if constexpr (std::is_same_v<T, std::string>) {
          return "t:" + sqlKeyEncode(x);
        } else {
          // b: -- sha256 of the complete fetched value, independent of the
          // cell's display budget (§3.5).
          const std::string_view sv(reinterpret_cast<const char *>(x.data()),
                                    x.size());
          return "b:" + sqlHashHex(sv);
        }
      },
      v);
}

std::string encodeSqlValues(std::span<const SqlValue> values) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    out += encodeSqlValue(values[i]);
  }
  return out;
}

std::string dbidForPath(std::string_view path) {
  char resolved[PATH_MAX];
  if (::realpath(std::string(path).c_str(), resolved) != nullptr) {
    return "f:" + sqlHashHex(std::string_view(resolved));
  }
  // realpath failed (ENOENT): hash the spelling -- the key is still stable
  // per-path, just not symlink-fused.
  return "f:" + sqlHashHex(path);
}

std::string displaySqlValue(const SqlValue &v, bool truncated) {
  return std::visit(
      [&](const auto &x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "NULL";
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return std::to_string(x);
        } else if constexpr (std::is_same_v<T, double>) {
          char buf[32];
          std::snprintf(buf, sizeof buf, "%.17g", x);
          return buf;
        } else if constexpr (std::is_same_v<T, std::string>) {
          return x;
        } else {
          return "blob(" + std::to_string(x.size()) + " B" +
                 (truncated ? ", truncated" : "") + ")";
        }
      },
      v);
}

std::string displaySqlValues(std::span<const SqlValue> values) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    out += displaySqlValue(values[i]);
  }
  return out;
}

} // namespace zigzag
