#include "xudu/core/magic_mime.hpp"

#include <magic.h>

namespace xudu {

MagicMimeDetector::MagicMimeDetector() {
  cookie = magic_open(MAGIC_MIME_TYPE | MAGIC_SYMLINK);
  if (cookie != nullptr) {
    if (magic_load(static_cast<magic_t>(cookie), nullptr) != 0) {
      magic_close(static_cast<magic_t>(cookie));
      cookie = nullptr;
    }
  }
}

MagicMimeDetector::~MagicMimeDetector() {
  if (cookie != nullptr) {
    magic_close(static_cast<magic_t>(cookie));
  }
}

std::string MagicMimeDetector::identifyBuffer(const void *data,
                                              const std::size_t size) const {
  if (cookie == nullptr || data == nullptr || size == 0) {
    return {};
  }
  const char *res =
      magic_buffer(static_cast<magic_t>(cookie), data, size);
  return res != nullptr ? std::string(res) : std::string{};
}

std::string MagicMimeDetector::identifyFile(const std::string &path) const {
  if (cookie == nullptr || path.empty()) {
    return {};
  }
  const char *res =
      magic_file(static_cast<magic_t>(cookie), path.c_str());
  return res != nullptr ? std::string(res) : std::string{};
}

bool MagicMimeDetector::isAudioMime(const std::string_view mime) {
  return mime.find("audio/") != std::string_view::npos;
}

bool MagicMimeDetector::isVideoMime(const std::string_view mime) {
  return mime.find("video/") != std::string_view::npos;
}

bool MagicMimeDetector::isImageMime(const std::string_view mime) {
  return mime.find("image/") != std::string_view::npos;
}

bool MagicMimeDetector::isPdfMime(const std::string_view mime) {
  return mime.find("application/pdf") != std::string_view::npos;
}

bool MagicMimeDetector::isMediaMime(const std::string_view mime) {
  return isAudioMime(mime) || isVideoMime(mime) || isImageMime(mime);
}

} // namespace xudu
