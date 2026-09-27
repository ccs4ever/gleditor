#pragma once

#include <filesystem>
#include <iosfwd>

namespace xanadu {

// A ustar envelope carries the two native store files byte for byte.
void writeStoreStream(const std::filesystem::path &directory,
                      std::ostream &out);
void readStoreStream(std::istream &in, const std::filesystem::path &directory);

class TemporaryStreamStore {
public:
  TemporaryStreamStore();
  explicit TemporaryStreamStore(std::istream &in);
  ~TemporaryStreamStore();

  TemporaryStreamStore(const TemporaryStreamStore &)            = delete;
  TemporaryStreamStore &operator=(const TemporaryStreamStore &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

} // namespace xanadu
