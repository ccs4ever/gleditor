#include <cstddef>
#include <fstream>
#include <ios>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <magic.h>
#include <poppler-document.h>
#include <poppler-page.h>

// Core poppler: the OutputDev/PDFDoc/GfxState/Stream mechanism pdfimages
// itself uses to get at embedded figures, which poppler-cpp's page API
// cannot reach at all (see PdfImageExtractor's own comment). Unlike
// poppler-cpp, this is POPPLER_PRIVATE_EXPORT -- poppler's own label for
// unstable API -- and the installed core is already at soversion 162, so
// this interface churns; an owner who upgrades poppler should expect to fix
// this file.
#include <GfxState.h>
#include <Object.h>
#include <OutputDev.h>
#include <PDFDoc.h>
#include <Stream.h>
#include <goo/GooString.h>

#ifdef GLEDITOR_HAVE_SDL_IMAGE
#if GLEDITOR_SDL_MAJOR == 3
#include <SDL3_image/SDL_image.h>
#else
#include <SDL2/SDL_image.h>
#endif
#endif

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

} // namespace

namespace gleditor {

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
  const char *res = magic_buffer(static_cast<magic_t>(cookie), data, size);
  return res != nullptr ? std::string(res) : std::string{};
}

std::string MagicMimeDetector::identifyFile(const std::string &path) const {
  if (cookie == nullptr || path.empty()) {
    return {};
  }
  const char *res = magic_file(static_cast<magic_t>(cookie), path.c_str());
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

} // namespace gleditor

namespace {

bool isPdfFile(const std::string &filePath, const std::string &rawBytes) {
  gleditor::MagicMimeDetector magic;
  if (!rawBytes.empty()) {
    const auto mime = magic.identifyBuffer(rawBytes.data(), rawBytes.size());
    if (gleditor::MagicMimeDetector::isPdfMime(mime)) {
      return true;
    }
  }
  if (!filePath.empty()) {
    const auto mime = magic.identifyFile(filePath);
    if (gleditor::MagicMimeDetector::isPdfMime(mime)) {
      return true;
    }
  }
  return false;
}

/// A figure found on a PDF page, plus the PDF object reference it was
/// decoded from when one exists -- how extractPdfDocument() later tells two
/// occurrences of the *same* embedded image (a repeated letterhead logo,
/// say) from two images that merely happen to look alike. Declared outside
/// the GLEDITOR_HAVE_SDL_IMAGE guard below so extractPdfDocument()'s common
/// code compiles the same way whether or not figure extraction itself is
/// available.
struct PdfPageImage {
  std::string png;
  std::optional<std::pair<int, int>> ref;
};

#ifdef GLEDITOR_HAVE_SDL_IMAGE

/// Encode @p width x @p height RGB24 pixels as PNG bytes, entirely in
/// memory. The one place SDL3_image's IMG_SavePNG_IO (SDL2_image's
/// IMG_SavePNG_RW) gets used for encoding rather than decoding in this
/// codebase -- everywhere else only ever reads an already-encoded image.
std::string encodeRgbAsPng(const std::vector<unsigned char> &rgb,
                           const int width, const int height) {
  if (rgb.size() !=
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3) {
    return {};
  }
#if GLEDITOR_SDL_MAJOR == 3
  SDL_Surface *surface =
      SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGB24,
                            const_cast<unsigned char *>(rgb.data()), width * 3);
  if (nullptr == surface) {
    return {};
  }
  std::string png;
  if (SDL_IOStream *io = SDL_IOFromDynamicMem(); nullptr != io) {
    if (IMG_SavePNG_IO(surface, io, false)) {
      const auto size  = SDL_GetIOSize(io);
      auto *const data = static_cast<const char *>(SDL_GetPointerProperty(
          SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER,
          nullptr));
      if (nullptr != data && size > 0) {
        png.assign(data, static_cast<std::size_t>(size));
      }
    }
    SDL_CloseIO(io);
  }
  SDL_DestroySurface(surface);
  return png;
#else
  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
      const_cast<unsigned char *>(rgb.data()), width, height, 24, width * 3,
      SDL_PIXELFORMAT_RGB24);
  if (nullptr == surface) {
    return {};
  }
  // SDL2's SDL_RWFromMem needs a fixed-size buffer up front, unlike SDL3's
  // dynamically-growing memory stream. PNG compression never expands
  // incompressible input by more than a small, bounded overhead, so the
  // uncompressed size plus a generous margin for chunk headers is a safe
  // upper bound rather than a guess.
  std::string png(rgb.size() + 4096, '\0');
  std::string result;
  if (SDL_RWops *rw = SDL_RWFromMem(png.data(), static_cast<int>(png.size()));
      nullptr != rw) {
    if (IMG_SavePNG_RW(surface, rw, 0) == 0) {
      const auto written = SDL_RWtell(rw);
      if (written > 0) {
        result.assign(png.data(), static_cast<std::size_t>(written));
      }
    }
    SDL_RWclose(rw);
  }
  SDL_FreeSurface(surface);
  return result;
#endif
}

/// Extracts embedded figures from a PDF via poppler core's OutputDev
/// mechanism -- the one pdfimages itself uses -- because poppler-cpp cannot:
/// its page API is orientation/duration/page_rect/label/transition/search/
/// text/text_list and nothing else (verified against poppler-page.h and
/// libpoppler-cpp.so's exported symbols at poppler 26.07.0).
/// poppler::image exists but is only ever produced by whole-page
/// rasterisation, which was rejected: it destroys the text layer along with
/// everything xanalogical about the document.
///
/// Only three of OutputDev's methods are genuinely pure virtual
/// (upsideDown/useDrawChar/interpretType3Chars); everything else already has
/// an empty default body, which is what makes a subclass that only cares
/// about images this small.
class PdfImageExtractor : public OutputDev {
public:
  [[nodiscard]] bool upsideDown() override { return false; }
  [[nodiscard]] bool useDrawChar() override { return false; }
  [[nodiscard]] bool interpretType3Chars() override { return false; }
  // needNonText() gates whether Gfx processes non-text content (images,
  // paths) at all -- true, the default every real OutputDev that draws
  // anything but glyphs keeps, is what this needs despite useDrawChar()
  // being false; the two are independent questions ("do you want drawChar
  // calls" vs "do you want non-text calls"), not two spellings of the same
  // "text or not" switch, which is the mistake that first left this
  // override here returning false and drawImage() never firing at all.

  void startPage(int pageNum, GfxState * /*state*/, XRef * /*xref*/) override {
    currentPage = pageNum;
  }

  // Plain colour image, the common case for a photo or diagram.
  void drawImage(GfxState * /*state*/, Object *ref, Stream *str,
                 const int width, const int height, GfxImageColorMap *colorMap,
                 bool /*interpolate*/, const int * /*maskColors*/,
                 bool /*inlineImg*/) override {
    capture(ref, str, width, height, colorMap);
  }

  // An image with a soft (alpha) mask. The mask itself is not extracted here
  // -- reconstructing it into a real RGBA figure needs decoding a second
  // stream and compositing it, which is more machinery than this pass's
  // scope -- so the base colour image is captured as if it were opaque. A
  // documented simplification, not silently wrong: it under-serves an image
  // whose transparency was load-bearing (a logo meant to sit over varied
  // backgrounds, say) rather than misrepresenting an ordinary photo.
  void drawSoftMaskedImage(GfxState * /*state*/, Object *ref, Stream *str,
                           const int width, const int height,
                           GfxImageColorMap *colorMap, bool /*interpolate*/,
                           Stream * /*maskStr*/, int /*maskWidth*/,
                           int /*maskHeight*/,
                           GfxImageColorMap * /*maskColorMap*/,
                           bool /*maskInterpolate*/) override {
    capture(ref, str, width, height, colorMap);
  }

  // A 1-bit stencil mask -- decorative line art or glyphs from an embedded
  // raster font, not the photos/diagrams "figure" means here -- is
  // deliberately not captured, to avoid every checkbox and bullet a page
  // draws this way turning into its own spurious image span.
  void drawImageMask(GfxState * /*state*/, Object * /*ref*/, Stream * /*str*/,
                     int /*width*/, int /*height*/, bool /*invert*/,
                     bool /*interpolate*/, bool /*inlineImg*/) override {}

  /// Every figure found on @p pageNum (1-based, matching PDFDoc's own page
  /// numbering), in the order poppler's content-stream walk produced them.
  [[nodiscard]] const std::vector<PdfPageImage> &
  imagesOnPage(const int pageNum) const {
    static const std::vector<PdfPageImage> empty;
    const auto found = byPage.find(pageNum);
    return found != byPage.end() ? found->second : empty;
  }

private:
  void capture(Object *ref, Stream *str, const int width, const int height,
               GfxImageColorMap *colorMap) {
    if (width <= 0 || height <= 0 || nullptr == colorMap || !colorMap->isOk()) {
      return;
    }
    // Decoded once per distinct PDF object, however many pages repeat it (a
    // letterhead logo, say); the ref itself travels with the decoded bytes so
    // extractPdfDocument() can also skip *storing* a repeat, not just
    // re-decoding it.
    std::string png;
    std::optional<std::pair<int, int>> refKey;
    if (nullptr != ref && ref->isRef()) {
      refKey           = std::make_pair(ref->getRefNum(), ref->getRefGen());
      const auto found = decoded.find(*refKey);
      if (found != decoded.end()) {
        png = found->second;
      } else {
        png              = decodeOne(str, width, height, colorMap);
        decoded[*refKey] = png;
      }
    } else {
      png = decodeOne(str, width, height, colorMap);
    }
    if (!png.empty()) {
      byPage[currentPage].push_back(PdfPageImage{png, refKey});
    }
  }

  [[nodiscard]] static std::string decodeOne(Stream *str, const int width,
                                             const int height,
                                             GfxImageColorMap *colorMap) {
    ImageStream imgStr(str, width, colorMap->getNumPixelComps(),
                       colorMap->getBits());
    if (!imgStr.rewind()) {
      return {};
    }
    std::vector<unsigned char> rgb(static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 3);
    for (int y = 0; y < height; ++y) {
      unsigned char *line = imgStr.getLine();
      if (nullptr == line) {
        break;
      }
      colorMap->getRGBLine(
          line, rgb.data() + (static_cast<std::size_t>(y) * width * 3), width);
    }
    imgStr.close();
    return encodeRgbAsPng(rgb, width, height);
  }

  int currentPage{0};
  std::map<std::pair<int, int>, std::string> decoded;
  std::map<int, std::vector<PdfPageImage>> byPage;
};

#endif // GLEDITOR_HAVE_SDL_IMAGE

void extractPdfDocument(const poppler::document &doc, PDFDoc *coreDoc,
                        std::string &outText,
                        std::vector<std::uint32_t> &outBreaks,
                        std::vector<gleditor::ContentPiece> &outPieces) {
  outText.clear();
  outBreaks.clear();
  outPieces.clear();
  const int pageCount = doc.pages();
  // Maps a PDF image object ref to the index in outPieces its first
  // occurrence landed at, across the whole document -- what lets a later
  // page's occurrence of the same figure (a repeated letterhead logo, say)
  // point back at that piece via duplicateOfPieceIndex instead of carrying
  // its own copy of the (already ref-deduplicated, decode-wise) bytes.
  std::map<std::pair<int, int>, std::size_t> firstPieceIndexForRef;

#ifdef GLEDITOR_HAVE_SDL_IMAGE
  PdfImageExtractor imageExtractor;
  if (nullptr != coreDoc && coreDoc->isOk() && pageCount > 0) {
    // One pass over the whole document rather than one displayPage() call
    // per page: startPage() still reports which page each drawImage() came
    // from, so nothing about per-page grouping is lost, and the content
    // streams are only walked once.
    coreDoc->displayPages(&imageExtractor, 1, pageCount, 72.0, 72.0, 0, true,
                          false, false);
  }
#endif

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
    const bool hadText = !str.empty();
    if (str.empty()) {
      str = "\n";
    }
    if (str.back() != '\n') {
      str += '\n';
    }
    outText += str;
    outBreaks.push_back(static_cast<std::uint32_t>(outText.size()));

#ifdef GLEDITOR_HAVE_SDL_IMAGE
    const auto &pageImages = imageExtractor.imagesOnPage(i + 1);
#else
    const std::vector<PdfPageImage> pageImages;
#endif
    if (!hadText && pageImages.empty()) {
      std::cerr << "warning: PDF page " << (i + 1)
                << " yielded neither text nor images -- ingesting as empty\n";
    }

    outPieces.push_back(gleditor::ContentPiece{str, {}, pageImages.empty()});
    for (std::size_t imgIdx = 0; imgIdx < pageImages.size(); ++imgIdx) {
      const bool isLastOnPage = (imgIdx + 1 == pageImages.size());
      gleditor::ContentPiece piece{pageImages[imgIdx].png, "image/png",
                                   isLastOnPage};
      if (pageImages[imgIdx].ref.has_value()) {
        const auto &refKey = *pageImages[imgIdx].ref;
        const auto found   = firstPieceIndexForRef.find(refKey);
        if (found != firstPieceIndexForRef.end()) {
          piece.duplicateOfPieceIndex = found->second;
        } else {
          firstPieceIndexForRef[refKey] = outPieces.size();
        }
      }
      outPieces.push_back(std::move(piece));
    }
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
    content     = pdf.text();
    breaks      = pdf.forcedBreaks();
    piecesCache = pdf.pieces();
  } else {
    content = stripByteOrderMark(raw);
    breaks.clear();
    piecesCache = {ContentPiece{content, {}, false}};
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

std::vector<ContentPiece> FileTextSource::pieces() const {
  ensureLoaded();
  return piecesCache;
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

  // A second, independent parse of the same file for figure extraction: core
  // poppler's PDFDoc/OutputDev and poppler-cpp's document/page share no
  // public handle to hand between them, so getting at both text and images
  // means opening the file twice rather than one API lending the other its
  // internal state. A PDFDoc that fails to open (isOk() false) is passed
  // through as nullptr, which extractPdfDocument() already treats as "no
  // figures on any page" -- losing the images costs nothing the text
  // extraction above did not already succeed at getting.
  auto coreDoc = std::make_unique<PDFDoc>(std::make_unique<GooString>(path));
  extractPdfDocument(*doc, coreDoc->isOk() ? coreDoc.get() : nullptr, buffer,
                     breaks, piecesOf);
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

  auto memStream = std::make_unique<MemStream>(
      data, 0, static_cast<Goffset>(size), Object::null());
  auto coreDoc = std::make_unique<PDFDoc>(std::move(memStream));
  extractPdfDocument(*doc, coreDoc->isOk() ? coreDoc.get() : nullptr, buffer,
                     breaks, piecesOf);
}

} // namespace gleditor
