/**
 * @file config.cpp
 * @brief Implementation of the per-user settings file.
 */
#include "config.hpp" // IWYU pragma: associated

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include "common/tsv.hpp"

#include <gleditor/paths.hpp>

namespace xanadu {

namespace {

/// The value of @p name, or nothing. getenv rather than a cached copy: a test
/// sets these, and reading them once at start-up would ignore that.
std::string environment(const char *const name) {
  const auto *const value = std::getenv(name);
  return nullptr == value ? std::string{} : std::string{value};
}

} // namespace

std::string Config::toTsv() const {
  std::string out;
  common::tsv::write(out, "author", author.name);
  common::tsv::write(out, "email", author.email);
  common::tsv::write(out, "gpg_key", author.gpgKey);
  common::tsv::write(out, "gpg_home", gpgHome);
  return out;
}

std::optional<Config> Config::fromTsv(const std::string_view text) {
  const auto entries = common::tsv::read(text);
  if (!entries) {
    return std::nullopt;
  }
  Config out;
  for (const auto &[key, value] : *entries) {
    if ("author" == key) {
      out.author.name = value;
    } else if ("email" == key) {
      out.author.email = value;
    } else if ("gpg_key" == key) {
      out.author.gpgKey = value;
    } else if ("gpg_home" == key) {
      out.gpgHome = value;
    }
    // Anything else is somebody's note to themselves, or a setting a later
    // version knows about. Neither is this version's business to complain
    // about.
  }
  return out;
}

std::string configPath() {
  if (const auto named = environment("XUDU_CONFIG"); !named.empty()) {
    return named;
  }
  return gleditor::paths::configPath("xudu", "config.tsv");
}

Config loadConfig(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  const std::string text{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  auto read = Config::fromTsv(text);
  if (!read) {
    throw std::runtime_error(
        path + " is not a xudu configuration file. It should contain escaped "
               "tab-separated key/value rows.");
  }
  return *read;
}

void saveConfig(const Config &config, const std::string &path) {
  const std::filesystem::path file(path);
  if (file.has_parent_path()) {
    std::filesystem::create_directories(file.parent_path());
  }
  {
    std::ofstream out(file, std::ios::trunc);
    out << config.toTsv();
  }
  std::error_code ignored;
  std::filesystem::permissions(file,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, ignored);
}

} // namespace xanadu
