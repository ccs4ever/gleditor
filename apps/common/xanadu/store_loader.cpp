/**
 * @file store_loader.cpp
 * @brief Singular native store loading and file import pathways for Xanadu.
 */
#include "common/xanadu/store_loader.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace xanadu {

void loadStore(Store &store, const std::filesystem::path &path) {
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error("Store path does not exist: " + path.string());
  }
  store.load(path.string());
}

std::unique_ptr<Store> loadStore(const std::filesystem::path &path,
                                 std::shared_ptr<UserPermascroll> permascroll) {
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error("Store path does not exist: " + path.string());
  }
  auto store = permascroll ? std::make_unique<Store>(std::move(permascroll))
                           : std::make_unique<Store>();
  store->load(path.string());
  return store;
}

std::unique_ptr<Store>
importFileStore(const std::filesystem::path &source,
                std::shared_ptr<UserPermascroll> permascroll,
                const std::filesystem::path &destination) {
  if (!std::filesystem::exists(source)) {
    throw std::runtime_error("Source file does not exist: " + source.string());
  }

  const auto fileSize = std::filesystem::file_size(source);
  if (fileSize > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(
        "Source file exceeds 4GB limit for native store: " + source.string());
  }

  std::ifstream stream(source, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Failed to open source file: " + source.string());
  }

  std::string content(static_cast<std::size_t>(fileSize), '\0');
  if (fileSize > 0) {
    stream.read(content.data(), static_cast<std::streamsize>(fileSize));
    if (!stream) {
      throw std::runtime_error("Failed to read source file content: " +
                               source.string());
    }
  }

  auto scroll = permascroll ? std::move(permascroll)
                            : std::make_shared<UserPermascroll>();
  auto store  = std::make_unique<Store>(scroll);

  if (!content.empty()) {
    store->insert(MicroversionId{}, 0, content);
  }

  if (!destination.empty()) {
    store->save(destination.string());
  }

  return store;
}

} // namespace xanadu
