#ifndef XUDU_CORE_MAGIC_MIME_HPP
#define XUDU_CORE_MAGIC_MIME_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace xudu {

/**
 * @class MagicMimeDetector
 * @brief Thread-safe MIME type detector backed by libmagic.
 */
class MagicMimeDetector {
public:
  MagicMimeDetector();
  ~MagicMimeDetector();

  MagicMimeDetector(const MagicMimeDetector &)            = delete;
  MagicMimeDetector &operator=(const MagicMimeDetector &) = delete;
  MagicMimeDetector(MagicMimeDetector &&)                 = delete;
  MagicMimeDetector &operator=(MagicMimeDetector &&)      = delete;

  [[nodiscard]] std::string identifyBuffer(const void *data,
                                           std::size_t size) const;
  [[nodiscard]] std::string identifyFile(const std::string &path) const;

  [[nodiscard]] static bool isAudioMime(std::string_view mime);
  [[nodiscard]] static bool isVideoMime(std::string_view mime);
  [[nodiscard]] static bool isImageMime(std::string_view mime);
  [[nodiscard]] static bool isPdfMime(std::string_view mime);
  [[nodiscard]] static bool isMediaMime(std::string_view mime);

private:
  void *cookie{nullptr};
};

} // namespace xudu

namespace gleditor {
using MagicMimeDetector = xudu::MagicMimeDetector;
} // namespace gleditor

#endif // XUDU_CORE_MAGIC_MIME_HPP
