#include "vagent-native-host/loader.hpp"

#include <array>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <utility>

namespace xanadu::vagent::native {

namespace {

// Self-contained standard SHA-256 implementation for library checksum reporting
struct Sha256Context {
  std::array<uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint64_t count{0};
  std::array<uint8_t, 64> buffer{};
};

inline uint32_t rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

void sha256Transform(std::array<uint32_t, 8> &state, const uint8_t data[64]) {
  static constexpr std::array<uint32_t, 64> K = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

  std::array<uint32_t, 64> w{};
  for (size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
           (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
           (static_cast<uint32_t>(data[i * 4 + 3]));
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 =
        rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 =
        rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1    = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch    = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + s1 + ch + K[i] + w[i];
    const uint32_t s0    = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

void sha256Update(Sha256Context &ctx, const uint8_t *data, size_t len) {
  size_t idx = static_cast<size_t>(ctx.count & 63);
  ctx.count += len;

  if (idx > 0) {
    const size_t left = 64 - idx;
    if (len < left) {
      std::memcpy(&ctx.buffer[idx], data, len);
      return;
    }
    std::memcpy(&ctx.buffer[idx], data, left);
    sha256Transform(ctx.state, ctx.buffer.data());
    data += left;
    len -= left;
    idx = 0;
  }

  while (len >= 64) {
    sha256Transform(ctx.state, data);
    data += 64;
    len -= 64;
  }

  if (len > 0) {
    std::memcpy(ctx.buffer.data(), data, len);
  }
}

std::string sha256Final(Sha256Context &ctx) {
  const uint64_t total_bits = ctx.count * 8;
  size_t idx                = static_cast<size_t>(ctx.count & 63);

  ctx.buffer[idx++] = 0x80;
  if (idx > 56) {
    std::memset(&ctx.buffer[idx], 0, 64 - idx);
    sha256Transform(ctx.state, ctx.buffer.data());
    idx = 0;
  }
  std::memset(&ctx.buffer[idx], 0, 56 - idx);

  for (size_t i = 0; i < 8; ++i) {
    ctx.buffer[63 - i] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xff);
  }
  sha256Transform(ctx.state, ctx.buffer.data());

  std::string hex;
  hex.reserve(64);
  for (size_t i = 0; i < 8; ++i) {
    hex += std::format("{:08x}", ctx.state[i]);
  }
  return hex;
}

} // namespace

LoadedModule::~LoadedModule() {
  if (module_handle) {
    ::dlclose(module_handle);
    module_handle = nullptr;
  }
  for (auto it = bundled_handles.rbegin(); it != bundled_handles.rend(); ++it) {
    if (*it) {
      ::dlclose(*it);
    }
  }
  bundled_handles.clear();
}

LoadedModule::LoadedModule(LoadedModule &&other) noexcept
    : module_handle(std::exchange(other.module_handle, nullptr)),
      bundled_handles(std::move(other.bundled_handles)),
      loaded_libraries(std::move(other.loaded_libraries)),
      module_info(std::move(other.module_info)),
      staging_dir_str(std::move(other.staging_dir_str)),
      module_name_str(std::move(other.module_name_str)),
      guest_context(other.guest_context),
      manifest_fn(std::exchange(other.manifest_fn, nullptr)),
      turn_fn(std::exchange(other.turn_fn, nullptr)),
      api_version_fn(std::exchange(other.api_version_fn, nullptr)),
      build_fn(std::exchange(other.build_fn, nullptr)),
      skills_fn(std::exchange(other.skills_fn, nullptr)) {
  guest_context.staging_dir = staging_dir_str.c_str();
  guest_context.module_name = module_name_str.c_str();
}

LoadedModule &LoadedModule::operator=(LoadedModule &&other) noexcept {
  if (this != &other) {
    if (module_handle) {
      ::dlclose(module_handle);
    }
    for (auto it = bundled_handles.rbegin(); it != bundled_handles.rend();
         ++it) {
      if (*it) {
        ::dlclose(*it);
      }
    }
    module_handle             = std::exchange(other.module_handle, nullptr);
    bundled_handles           = std::move(other.bundled_handles);
    loaded_libraries          = std::move(other.loaded_libraries);
    module_info               = std::move(other.module_info);
    staging_dir_str           = std::move(other.staging_dir_str);
    module_name_str           = std::move(other.module_name_str);
    guest_context             = other.guest_context;
    guest_context.staging_dir = staging_dir_str.c_str();
    guest_context.module_name = module_name_str.c_str();
    manifest_fn               = std::exchange(other.manifest_fn, nullptr);
    turn_fn                   = std::exchange(other.turn_fn, nullptr);
    api_version_fn            = std::exchange(other.api_version_fn, nullptr);
    build_fn                  = std::exchange(other.build_fn, nullptr);
    skills_fn                 = std::exchange(other.skills_fn, nullptr);
  }
  return *this;
}

std::expected<std::string, std::string>
computeFileSha256(const std::string &file_path) {
  std::ifstream file(file_path, std::ios::binary);
  if (!file) {
    return std::unexpected("Failed to open file for hashing: " + file_path);
  }

  Sha256Context ctx{};
  std::array<char, 8192> buffer{};
  while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
    sha256Update(ctx, reinterpret_cast<const uint8_t *>(buffer.data()),
                 static_cast<size_t>(file.gcount()));
  }

  return sha256Final(ctx);
}

std::expected<LoadedModule, std::string>
loadNativeGuest(const LoaderConfig &config) {
  LoadedModule loaded{};
  namespace fs = std::filesystem;

  const fs::path staging = config.staging_dir;
  const fs::path lib_dir = staging / "lib";

  // Step 7: Topological dlopen on bundled libraries in <staging>/lib/
  for (const auto &soname : config.bundled_sonames) {
    fs::path lib_path = lib_dir / soname;
    if (!fs::exists(lib_path)) {
      // Also check staging directly
      lib_path = staging / soname;
      if (!fs::exists(lib_path)) {
        return std::unexpected("Bundled library not found in staging: " +
                               soname);
      }
    }

    ::dlerror(); // Clear any previous error
    void *h = ::dlopen(lib_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
      const char *err = ::dlerror();
      return std::unexpected("Failed to load bundled library " + soname + ": " +
                             (err ? err : "unknown error"));
    }

    loaded.bundled_handles.push_back(h);

    auto hash_res = computeFileSha256(lib_path.string());
    LoadedLibraryInfo info;
    info.soname = soname;
    info.path   = lib_path.string();
    info.sha256 = hash_res ? *hash_res : "";
    loaded.loaded_libraries.push_back(std::move(info));
  }

  // Next, dlopen the module shared library: <staging>/module.so
  fs::path module_path = staging / config.module_filename;
  if (!fs::exists(module_path)) {
    return std::unexpected("Module shared library not found: " +
                           module_path.string());
  }

  ::dlerror();
  void *mod_h = ::dlopen(module_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!mod_h) {
    const char *err = ::dlerror();
    return std::unexpected("Failed to load module " + module_path.string() +
                           ": " + (err ? err : "unknown error"));
  }

  loaded.module_handle = mod_h;

  auto mod_hash_res         = computeFileSha256(module_path.string());
  loaded.module_info.soname = config.module_filename;
  loaded.module_info.path   = module_path.string();
  loaded.module_info.sha256 = mod_hash_res ? *mod_hash_res : "";

  // Resolve VagentGuestContext and entry points
  loaded.manifest_fn = reinterpret_cast<VagentManifestFn>(
      ::dlsym(mod_h, "vagent_agent_manifest_v1"));
  loaded.turn_fn =
      reinterpret_cast<VagentTurnFn>(::dlsym(mod_h, "vagent_agent_turn_v1"));
  loaded.api_version_fn = reinterpret_cast<VagentApiVersionFn>(
      ::dlsym(mod_h, "vagent_native_api_version"));
  loaded.build_fn =
      reinterpret_cast<VagentBuildFn>(::dlsym(mod_h, "vagent_native_build"));
  loaded.skills_fn =
      reinterpret_cast<VagentSkillsFn>(::dlsym(mod_h, "vagent_native_skills"));

  // Check that at least one valid entry point exists
  if (!loaded.turn_fn && !loaded.build_fn && !loaded.manifest_fn) {
    return std::unexpected("Module does not export required entry points "
                           "(vagent_agent_turn_v1 or vagent_agent_manifest_v1 "
                           "or vagent_native_build)");
  }

  // Populate guest context
  loaded.guest_context.abi_version = 1;
  loaded.staging_dir_str           = config.staging_dir;
  loaded.module_name_str           = config.module_filename;
  loaded.guest_context.staging_dir = loaded.staging_dir_str.c_str();
  loaded.guest_context.module_name = loaded.module_name_str.c_str();

  return loaded;
}

} // namespace xanadu::vagent::native
