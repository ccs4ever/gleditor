#ifndef VAGENT_NATIVE_HOST_LOADER_HPP
#define VAGENT_NATIVE_HOST_LOADER_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace xanadu::vagent::native {

extern "C" {

struct VagentGuestContext {
  uint32_t abi_version{1};
  const char *staging_dir{nullptr};
  const char *module_name{nullptr};
  void *user_data{nullptr};
};

using VagentManifestFn   = int (*)(const VagentGuestContext *ctx,
                                   char *out_manifest, size_t max_len);
using VagentTurnFn       = int (*)(const VagentGuestContext *ctx,
                                   const uint8_t *in_turn, size_t in_len,
                                   uint8_t *out_turn, size_t *out_len);
using VagentApiVersionFn = uint32_t (*)();
using VagentBuildFn      = int (*)(const void *api, void *builder);
using VagentSkillsFn     = const void *(*)();

} // extern "C"

struct LoadedLibraryInfo {
  std::string soname;
  std::string path;
  std::string sha256;
};

struct LoadedModule {
  void *module_handle{nullptr};
  std::vector<void *> bundled_handles;
  std::vector<LoadedLibraryInfo> loaded_libraries;
  LoadedLibraryInfo module_info;
  std::string staging_dir_str;
  std::string module_name_str;
  VagentGuestContext guest_context{};
  VagentManifestFn manifest_fn{nullptr};
  VagentTurnFn turn_fn{nullptr};
  VagentApiVersionFn api_version_fn{nullptr};
  VagentBuildFn build_fn{nullptr};
  VagentSkillsFn skills_fn{nullptr};

  ~LoadedModule();
  LoadedModule()                                = default;
  LoadedModule(const LoadedModule &)            = delete;
  LoadedModule &operator=(const LoadedModule &) = delete;
  LoadedModule(LoadedModule &&) noexcept;
  LoadedModule &operator=(LoadedModule &&) noexcept;
};

struct LoaderConfig {
  std::string staging_dir;
  std::string module_filename{"module.so"};
  std::vector<std::string> bundled_sonames; // In topological order
};

/// Step 7: Topological dlopen of bundled libraries, then module.so,
/// and resolution of VagentGuestContext and entry points.
[[nodiscard]] std::expected<LoadedModule, std::string>
loadNativeGuest(const LoaderConfig &config);

/// Helper to compute SHA-256 hex string for a file on disk.
[[nodiscard]] std::expected<std::string, std::string>
computeFileSha256(const std::string &file_path);

} // namespace xanadu::vagent::native

#endif // VAGENT_NATIVE_HOST_LOADER_HPP
