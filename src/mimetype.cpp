/**
 * @file mimetype.cpp
 * @brief Implementation of RFC 2045/2046 MIME media types and libmagic
 *        detection.
 */
#include <gleditor/mimetype.hpp>

#include <algorithm>
#include <cctype>
#include <magic.h>
#include <string>
#include <string_view>
#include <utility>

namespace gleditor {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

std::string toLower(const std::string_view s) {
  std::string result;
  result.reserve(s.size());
  for (const char c : s) {
    result.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return result;
}

struct ThreadLocalMagic {
  magic_t cookie{nullptr};

  ThreadLocalMagic() {
    cookie = magic_open(MAGIC_MIME_TYPE);
    if (nullptr != cookie) {
      if (0 != magic_load(cookie, nullptr)) {
        magic_close(cookie);
        cookie = nullptr;
      }
    }
  }

  ~ThreadLocalMagic() {
    if (nullptr != cookie) {
      magic_close(cookie);
      cookie = nullptr;
    }
  }
};

} // namespace

const MimeType MimeType::TextPlain{"text", "plain"};
const MimeType MimeType::TextMarkdown{"text", "markdown"};
const MimeType MimeType::TextHtml{"text", "html"};
const MimeType MimeType::ImagePng{"image", "png"};
const MimeType MimeType::ImageJpeg{"image", "jpeg"};
const MimeType MimeType::ImageWebp{"image", "webp"};
const MimeType MimeType::ImageGif{"image", "gif"};
const MimeType MimeType::ImageSvg{"image", "svg+xml"};
const MimeType MimeType::OctetStream{"application", "octet-stream"};

MimeType::MimeType() : type_("application"), subtype_("octet-stream") {}

MimeType::MimeType(std::string type, std::string subtype,
                   std::string parameters)
    : type_(toLower(trim(type))), subtype_(toLower(trim(subtype))),
      parameters_(trim(parameters)) {}

MimeType::MimeType(const std::string_view fullType) { *this = parse(fullType); }

std::string MimeType::essence() const {
  if (type_.empty() && subtype_.empty()) {
    return "";
  }
  return type_ + "/" + subtype_;
}

std::string MimeType::str() const {
  const std::string ess = essence();
  if (parameters_.empty()) {
    return ess;
  }
  return ess + "; " + parameters_;
}

MimeType MimeType::parse(const std::string_view raw) {
  const std::string_view trimmed = trim(raw);
  if (trimmed.empty()) {
    return MimeType::OctetStream;
  }

  const auto semiPos           = trimmed.find(';');
  std::string_view essencePart = (std::string_view::npos != semiPos)
                                     ? trimmed.substr(0, semiPos)
                                     : trimmed;
  std::string_view paramPart   = (std::string_view::npos != semiPos)
                                     ? trimmed.substr(semiPos + 1)
                                     : std::string_view{};

  essencePart = trim(essencePart);

  const auto slashPos = essencePart.find('/');
  if (std::string_view::npos == slashPos) {
    return MimeType(toLower(trim(essencePart)), "",
                    std::string(trim(paramPart)));
  }

  const std::string type    = toLower(trim(essencePart.substr(0, slashPos)));
  const std::string subtype = toLower(trim(essencePart.substr(slashPos + 1)));

  return MimeType(type, subtype, std::string(trim(paramPart)));
}

MimeType MimeDetector::detectBuffer(const std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) {
    return MimeType::OctetStream;
  }

  thread_local ThreadLocalMagic magic;
  if (nullptr == magic.cookie) {
    return MimeType::OctetStream;
  }

  const char *const mime =
      magic_buffer(magic.cookie, bytes.data(), bytes.size());
  if (nullptr == mime) {
    return MimeType::OctetStream;
  }

  return MimeType::parse(mime);
}

MimeType MimeDetector::detectBuffer(const std::string_view bytes) {
  return detectBuffer(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
}

MimeType MimeDetector::detectFile(const std::string &filePath) {
  thread_local ThreadLocalMagic magic;
  if (nullptr == magic.cookie) {
    return MimeType::OctetStream;
  }

  const char *const mime = magic_file(magic.cookie, filePath.c_str());
  if (nullptr == mime) {
    return MimeType::OctetStream;
  }

  return MimeType::parse(mime);
}

} // namespace gleditor
