#include "common/xanadu/store_loader.hpp"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace xanadu {

void loadStore(Store &store, const std::filesystem::path &path) {
  store.load(path.string());
}

std::unique_ptr<Store>
loadStore(const std::filesystem::path &path,
          std::shared_ptr<UserPermascroll> permascroll) {
  auto store = std::make_unique<Store>(std::move(permascroll));
  loadStore(*store, path);
  return store;
}

std::unique_ptr<Store>
importFileStore(const std::filesystem::path &source,
                std::shared_ptr<UserPermascroll> permascroll,
                const std::filesystem::path &destination) {
  auto store = std::make_unique<Store>(std::move(permascroll));
  std::ifstream in(source, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open source file: " + source.string());
  std::string bytes((std::istreambuf_iterator<char>(in)), {});
  MicroversionId version;
  if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("source exceeds the store address space");
  version = store->insert(version, 0, bytes);
  if (!version.isZero()) {
    store->setCurrentVersions({version});
  }
  if (!destination.empty()) {
    store->save(destination.string());
  }
  return store;
}

} // namespace xanadu
