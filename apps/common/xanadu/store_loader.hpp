/**
 * @file store_loader.hpp
 * @brief Singular native store loading and file import pathways for Xanadu.
 */
#ifndef XANADU_STORE_LOADER_HPP
#define XANADU_STORE_LOADER_HPP

#include <filesystem>
#include <memory>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu {

/**
 * @brief Load an on-disk store into an existing Store instance.
 * @param store Destination Store instance.
 * @param path Filesystem path to the store directory.
 */
void loadStore(Store &store, const std::filesystem::path &path);

/**
 * @brief Load an existing on-disk native store.
 *
 * Xanadocs, ZigZag slices, and query stores share this pathway.
 *
 * @param path Filesystem path to the store directory.
 * @param permascroll Sovereign user permascroll stream (optional).
 * @return Unique pointer to the loaded Store.
 */
[[nodiscard]] std::unique_ptr<Store>
loadStore(const std::filesystem::path &path,
          std::shared_ptr<UserPermascroll> permascroll = nullptr);

/**
 * @brief Import a raw file into a native Store using the supplied permascroll.
 *
 * @param source Path to the source file to import.
 * @param permascroll Sovereign user permascroll to receive primedia bytes.
 * @param destination Optional path to save the newly created store to disk.
 * @return Unique pointer to the newly created Store with the imported content.
 */
[[nodiscard]] std::unique_ptr<Store>
importFileStore(const std::filesystem::path &source,
                std::shared_ptr<UserPermascroll> permascroll,
                const std::filesystem::path &destination = {});

} // namespace xanadu

#endif // XANADU_STORE_LOADER_HPP
