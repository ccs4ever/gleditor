/**
 * @file mimetype.hpp
 * @brief IETF RFC 2045 / RFC 2046 / RFC 6838 MIME media type encapsulation and
 *        libmagic detector.
 */
#ifndef GLEDITOR_MIMETYPE_HPP
#define GLEDITOR_MIMETYPE_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gleditor {

/**
 * @class MimeType
 * @brief Encapsulates a standard MIME media type (type, subtype, parameters).
 */
class MimeType {
public:
  MimeType();
  MimeType(std::string type, std::string subtype, std::string parameters = "");
  explicit MimeType(std::string_view fullType);

  [[nodiscard]] const std::string &type() const { return type_; }
  [[nodiscard]] const std::string &subtype() const { return subtype_; }
  [[nodiscard]] const std::string &parameters() const { return parameters_; }

  [[nodiscard]] std::string str() const;
  [[nodiscard]] std::string essence() const; // "type/subtype"

  [[nodiscard]] bool empty() const { return type_.empty() && subtype_.empty(); }

  [[nodiscard]] bool isText() const { return type_ == "text"; }
  [[nodiscard]] bool isImage() const { return type_ == "image"; }
  [[nodiscard]] bool isAudio() const { return type_ == "audio"; }
  [[nodiscard]] bool isVideo() const { return type_ == "video"; }
  [[nodiscard]] bool isBinary() const { return !isText(); }

  // Common standard MIME type instances
  static const MimeType TextPlain;
  static const MimeType TextMarkdown;
  static const MimeType TextHtml;
  static const MimeType ImagePng;
  static const MimeType ImageJpeg;
  static const MimeType ImageWebp;
  static const MimeType ImageGif;
  static const MimeType ImageSvg;
  static const MimeType OctetStream;

  [[nodiscard]] static MimeType parse(std::string_view raw);

  bool operator==(const MimeType &oth) const = default;

private:
  std::string type_{"application"};
  std::string subtype_{"octet-stream"};
  std::string parameters_;
};

/**
 * @class MimeDetector
 * @brief Thread-safe MIME type detector utilizing libmagic.
 */
class MimeDetector {
public:
  /// Detect MIME type from raw memory bytes using libmagic.
  [[nodiscard]] static MimeType
  detectBuffer(std::span<const std::uint8_t> bytes);
  [[nodiscard]] static MimeType detectBuffer(std::string_view bytes);

  /// Detect MIME type from a file path using libmagic.
  [[nodiscard]] static MimeType detectFile(const std::string &filePath);
};

} // namespace gleditor

#endif // GLEDITOR_MIMETYPE_HPP
