#include <cstddef>
#include <fstream>
#include <ios>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <magic.h>
#include <poppler-document.h>
#include <poppler-page.h>

#include <gleditor/text_source.hpp>

namespace {

/// Byte at @p index widened through unsigned char. `char` is signed on most
/// targets, so comparing a raw one against 0xEF or 0xFF is never true and
/// silently disables mark detection.
unsigned char byteAt(const std::string &str, const std::size_t index) {
  return static_cast<unsigned char>(str[index]);
}

bool startsWith(const std::string &str,
                const std::initializer_list<unsigned char> mark) {
  if (str.size() < mark.size()) {
    return false;
  }
  std::size_t i = 0;
  for (const auto expected : mark) {
    if (byteAt(str, i++) != expected) {
      return false;
    }
  }
  return true;
}

class MagicMimeDetector {
public:
  MagicMimeDetector() {
    cookie = magic_open(MAGIC_MIME_TYPE | MAGIC_SYMLINK);
    if (cookie != nullptr) {
      if (magic_load(cookie, nullptr) != 0) {
        magic_close(cookie);
        cookie = nullptr;
      }
    }
  }

  ~MagicMimeDetector() {
    if (cookie != nullptr) {
      magic_close(cookie);
    }
  }

  MagicMimeDetector(const MagicMimeDetector &)            = delete;
  MagicMimeDetector &operator=(const MagicMimeDetector &) = delete;

  [[nodiscard]] std::string identifyBuffer(const void *data,
                                           const std::size_t size) const {
    if (cookie == nullptr || data == nullptr || size == 0) {
      return {};
    }
    const char *res = magic_buffer(cookie, data, size);
    return res != nullptr ? std::string(res) : std::string{};
  }

  [[nodiscard]] std::string identifyFile(const std::string &path) const {
    if (cookie == nullptr || path.empty()) {
      return {};
    }
    const char *res = magic_file(cookie, path.c_str());
    return res != nullptr ? std::string(res) : std::string{};
  }

private:
  magic_t cookie{nullptr};
};

bool isPdfMime(const std::string &mime) {
  return mime.find("application/pdf") != std::string::npos;
}

bool isPdfFile(const std::string &filePath, const std::string &rawBytes) {
  MagicMimeDetector magic;
  if (!rawBytes.empty()) {
    const auto mime = magic.identifyBuffer(rawBytes.data(), rawBytes.size());
    if (isPdfMime(mime)) {
      return true;
    }
  }
  if (!filePath.empty()) {
    const auto mime = magic.identifyFile(filePath);
    if (isPdfMime(mime)) {
      return true;
    }
  }
  return false;
}

void extractPdfDocument(const poppler::document &doc, std::string &outText,
                        std::vector<std::uint32_t> &outBreaks) {
  outText.clear();
  outBreaks.clear();
  const int pageCount = doc.pages();
  for (int i = 0; i < pageCount; ++i) {
    std::unique_ptr<poppler::page> p(doc.create_page(i));
    if (!p) {
      continue;
    }
    auto ustr       = p->text(poppler::rectf(), poppler::page::physical_layout);
    auto utf8Bytes  = ustr.to_utf8();
    std::string str = std::string(utf8Bytes.data(), utf8Bytes.size());
    if (str.empty()) {
      ustr =
          p->text(poppler::rectf(), poppler::page::non_raw_non_physical_layout);
      utf8Bytes = ustr.to_utf8();
      str       = std::string(utf8Bytes.data(), utf8Bytes.size());
    }
    while (!str.empty() && (str.back() == '\x0c' || str.back() == '\r')) {
      str.pop_back();
    }
    if (str.empty()) {
      str = "\n";
    }
    outText += str;
    if (!outText.empty() && outText.back() != '\n') {
      outText += '\n';
    }
    outBreaks.push_back(static_cast<std::uint32_t>(outText.size()));
  }
}

} // namespace

namespace gleditor {

std::string stripByteOrderMark(std::string bytes) {
  if (startsWith(bytes, {0xEF, 0xBB, 0xBF})) {
    return bytes.substr(3);
  }
  // Tested before UTF-16, because a little-endian UTF-32 mark begins with the
  // whole of a little-endian UTF-16 one: checking the shorter first would
  // report every UTF-32LE file as UTF-16LE.
  if (startsWith(bytes, {0x00, 0x00, 0xFE, 0xFF}) ||
      startsWith(bytes, {0xFF, 0xFE, 0x00, 0x00})) {
    throw std::logic_error("utf32 not supported yet");
  }
  if (startsWith(bytes, {0xFE, 0xFF}) || startsWith(bytes, {0xFF, 0xFE})) {
    throw std::logic_error("utf16 not supported yet");
  }
  return bytes;
}

FileTextSource::FileTextSource(std::string path) : filePath(std::move(path)) {}

void FileTextSource::ensureLoaded() const {
  if (loaded) {
    return;
  }
  std::ifstream file(filePath, std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("failed to open file: " + filePath);
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string raw = ss.str();

  if (isPdfFile(filePath, raw)) {
    PdfTextSource pdf(filePath);
    content = pdf.text();
    breaks  = pdf.forcedBreaks();
  } else {
    content = stripByteOrderMark(raw);
    breaks.clear();
  }
  loaded = true;
}

std::string FileTextSource::text() const {
  ensureLoaded();
  return content;
}

std::vector<std::uint32_t> FileTextSource::forcedBreaks() const {
  ensureLoaded();
  return breaks;
}

PdfTextSource::PdfTextSource(std::string path) : label(path) {
  loadPdfFile(path);
}

PdfTextSource::PdfTextSource(const char *data, const std::size_t size,
                             std::string aName)
    : label(std::move(aName)) {
  loadPdfData(data, size);
}

void PdfTextSource::loadPdfFile(const std::string &path) {
  std::unique_ptr<poppler::document> doc(
      poppler::document::load_from_file(path));
  if (!doc) {
    throw std::runtime_error("failed to open PDF file: " + path);
  }
  if (doc->is_locked()) {
    throw std::runtime_error("PDF is password-protected: " + path);
  }
  numPages = static_cast<std::size_t>(doc->pages());
  extractPdfDocument(*doc, buffer, breaks);
}

void PdfTextSource::loadPdfData(const char *data, const std::size_t size) {
  std::unique_ptr<poppler::document> doc(
      poppler::document::load_from_raw_data(data, static_cast<int>(size)));
  if (!doc) {
    throw std::runtime_error("failed to open PDF data: " + label);
  }
  if (doc->is_locked()) {
    throw std::runtime_error("PDF is password-protected: " + label);
  }
  numPages = static_cast<std::size_t>(doc->pages());
  extractPdfDocument(*doc, buffer, breaks);
}

} // namespace gleditor
