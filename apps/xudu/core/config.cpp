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

#include "yaml.hpp"

namespace xudu {

namespace {

/// The value of @p name, or nothing. getenv rather than a cached copy: a test
/// sets these, and reading them once at start-up would ignore that.
std::string environment(const char *const name) {
  const auto *const value = std::getenv(name);
  return nullptr == value ? std::string{} : std::string{value};
}

} // namespace

std::string Config::toYaml() const {
  std::string out =
      "# Who this machine publishes xanadocs as. The name and email go into\n"
      "# the authorship record sealed with every document; the key is what\n"
      "# GnuPG signs that record with, before the content is sealed.\n";
  yaml::write(out, "author", author.name);
  yaml::write(out, "email", author.email);
  yaml::write(out, "gpg_key", author.gpgKey);
  yaml::write(out, "gpg_home", gpgHome);
  yaml::write(out, "gpg_secret_key", gpgSecretKey);
  return out;
}

std::optional<Config> Config::fromYaml(const std::string_view text) {
  const auto entries = yaml::read(text);
  if (!entries) {
    return std::nullopt;
  }
  Config out;
  for (const auto &[key, value, listItem] : *entries) {
    if (listItem) {
      continue;
    }
    if ("author" == key) {
      out.author.name = value;
    } else if ("email" == key) {
      out.author.email = value;
    } else if ("gpg_key" == key) {
      out.author.gpgKey = value;
    } else if ("gpg_home" == key) {
      out.gpgHome = value;
    } else if ("gpg_secret_key" == key) {
      out.gpgSecretKey = value;
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
  auto base = environment("XDG_CONFIG_HOME");
  if (base.empty()) {
    const auto home = environment("HOME");
    if (home.empty()) {
      // Nowhere to put it. The current directory is a worse guess than an
      // obviously wrong path, which at least says what was expected.
      return ".xudu-config.yaml";
    }
    base = (std::filesystem::path(home) / ".config").string();
  }
  return (std::filesystem::path(base) / "xudu" / "config.yaml").string();
}

Config loadConfig(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  const std::string text{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  auto read = Config::fromYaml(text);
  if (!read) {
    throw std::runtime_error(
        path + " is not a xudu configuration file. It should be lines of "
               "`key: \"value\"`; see the comment at the top of one this "
               "program wrote.");
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
    out << config.toYaml();
  }
  std::error_code ignored;
  std::filesystem::permissions(file,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, ignored);
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:
