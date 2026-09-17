#ifndef XANADU_STORE_LOADER_HPP
#define XANADU_STORE_LOADER_HPP

#include "common/xanadu/store.hpp"

#include <filesystem>
#include <memory>

namespace xanadu {

void loadStore(Store &store, const std::filesystem::path &path);

/// Open an existing native store. Xanadocs and ZigZag slices share this path;
/// their presentation is derived from the operations after loading.
[[nodiscard]] std::unique_ptr<Store>
loadStore(const std::filesystem::path &path,
          std::shared_ptr<UserPermascroll> permascroll = nullptr);

/// Import a text source into a native store using the supplied permascroll.
[[nodiscard]] std::unique_ptr<Store>
importFileStore(const std::filesystem::path &source,
                std::shared_ptr<UserPermascroll> permascroll,
                const std::filesystem::path &destination = {});

} // namespace xanadu

#endif
