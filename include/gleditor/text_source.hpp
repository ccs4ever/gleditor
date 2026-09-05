/**
 * @file text_source.hpp
 * @brief Where a document's text comes from.
 *
 * A document used to be a file: the constructor took a name, opened it, sorted
 * out its byte order mark and kept the name around to report. That is one
 * answer to the question, and it is the library's business only insofar as
 * opening a file is a thing a program might want done for it.
 *
 * It is not the only answer. Text may be generated, fetched, decompressed, or
 * -- the case this was written for -- reconstructed from a record of the edits
 * that produced it, in which case there is no file to name and the bytes exist
 * only once something has computed them. Splitting the source out means a
 * program can supply text however it likes without the document model growing
 * a case for each way.
 */
#ifndef GLEDITOR_TEXT_SOURCE_H
#define GLEDITOR_TEXT_SOURCE_H

#include <cstdint>
#include <gleditor/glyphcache/types.hpp>
#include <gleditor/layout_box.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gleditor {

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

/**
 * @brief One typed run of a source's content: plain text, or a whole media
 *        file's bytes tagged with its MIME type.
 *
 * What lets pieces() emit a figure a PDF embeds without the document model
 * growing a case for it: the figure is just another piece, interleaved with
 * the text pieces around it, and a caller that understands both (xudu's
 * ingest, inserting one Store op per piece) ends up with the same store an
 * ordinary mixed text-and-image xanadoc would have. A caller that does not
 * understand pieces() -- anything only asking for text() -- never sees a
 * media piece at all, since text() and forcedBreaks() keep their existing,
 * text-only meaning; see pieces()'s own comment.
 */
struct ContentPiece {
  std::string bytes;
  /// Empty for plain text -- the overwhelmingly common case, and the only
  /// kind text()-only sources ever produce. Set to a real MIME type (e.g.
  /// "image/png") for a piece that is a whole media file's bytes, meant to
  /// reach Store::insertMedia() rather than Store::insert().
  std::string mimeType;
  /// Whether a forced page break belongs immediately after this piece, in
  /// the concatenation of all of pieces()' bytes -- the reason this cannot
  /// simply reuse forcedBreaks(), whose offsets are into text() alone and
  /// would misalign the moment a media piece's raw bytes (never part of
  /// text()) sit between two pages' worth of text.
  bool pageBreakAfter{false};
  /// Set when this piece's bytes are identical to an earlier piece in the
  /// same pieces() call -- e.g. a letterhead logo embedded on every page of
  /// a PDF -- naming that earlier piece's index. A caller inserting through
  /// Store should route this piece through Store::insertSpan() against the
  /// span the earlier piece was inserted as, rather than calling
  /// Store::insertMedia() again and storing the same bytes a second time.
  /// std::nullopt for the overwhelmingly common case of a piece with no
  /// earlier duplicate.
  std::optional<std::size_t> duplicateOfPieceIndex;
};

/**
 * @brief Supplies a document's initial content.
 *
 * Implementations are asked once, when the document is built, and on whatever
 * thread built it -- which is a background loader thread for anything opened
 * from the command line. An implementation that touches shared state has to
 * say so.
 */
class TextSource {
public:
  TextSource()          = default;
  virtual ~TextSource() = default;

  TextSource(const TextSource &)            = delete;
  TextSource &operator=(const TextSource &) = delete;
  TextSource(TextSource &&)                 = delete;
  TextSource &operator=(TextSource &&)      = delete;

  /**
   * @brief The document's text, as UTF-8.
   *
   * Need not be valid UTF-8: the document validates what it is given and
   * repairs it, because a source reading somebody else's file cannot promise
   * what is in it. Returning invalid text is therefore allowed and produces a
   * document with the bad sequences replaced, rather than an exception.
   */
  [[nodiscard]] virtual std::string text() const = 0;

  /// What to call this document in diagnostics and in the window. Need not be
  /// a path, and need not be unique.
  [[nodiscard]] virtual std::string name() const = 0;

  /**
   * @brief Byte offsets into text() where a page must end, whatever room is
   *        left on it.
   *
   * Empty by default: a plain file or an in-memory buffer has no opinion
   * about where it paginates, which is what every existing source keeps
   * meaning by not overriding this. A source that does have forced breaks --
   * xudu's, reading them out of Version::forcedBreaks() -- returns them
   * sorted ascending, which is what lets a document find "the next one after
   * here" by taking the first that is greater than the current offset rather
   * than scanning for a minimum.
   */
  [[nodiscard]] virtual std::vector<std::uint32_t> forcedBreaks() const {
    return {};
  }

  /**
   * @brief Which decorations apply where in text(), as byte ranges into it.
   *
   * Empty by default, for the same reason forcedBreaks() is: plain text has
   * no opinion about how it should be shown. Ranges may overlap; a glyph
   * within more than one gets every decoration named across all of them.
   */
  [[nodiscard]] virtual std::vector<DecoratedRange> decoratedRanges() const {
    return {};
  }

  /**
   * @brief Byte ranges into text() that must not be split across a page
   *        boundary -- see gleditor::text::LayoutOptions::atomicRanges,
   *        which this feeds after Doc translates offsets into a given
   *        page's own slice-relative coordinates the same way it already
   *        does for decoratedRanges().
   *
   * Empty by default, for the same reason forcedBreaks() and
   * decoratedRanges() are: plain text has no content that must stay whole
   * across a page break. xudu's source overrides this to name each embedded
   * media placeholder's reserved run of blank lines.
   */
  [[nodiscard]] virtual std::vector<AtomicRange> atomicRanges() const {
    return {};
  }

  /**
   * @brief This document's page geometry.
   *
   * Defaults to gleditor::letterPage -- 8.5x11in Letter, the size every
   * gleditor/xudu document had before a page's size became something a
   * source could choose -- so a source that does not override this changes
   * nothing for it. A source wanting its own page size (a different paper
   * size, or PageSizing::FitContent to let a page grow to whatever landed
   * on it, the way zigzag's cells already do by calling TextLayout directly
   * rather than through a Doc) overrides this instead.
   */
  [[nodiscard]] virtual PageSize pageSize() const { return letterPage; }

  /**
   * @brief text(), split into typed pieces a caller that cares about media
   *        can insert individually instead of as one plain-text block.
   *
   * Default is the whole of text() as one plain piece with no forced break
   * after it -- exactly what every source except PdfTextSource means by not
   * overriding this, the same way not overriding forcedBreaks() means "no
   * opinion". A caller that only ever uses text()/forcedBreaks() is
   * unaffected by any source overriding this: the two accessors keep their
   * existing, independent meaning regardless of what pieces() returns.
   */
  [[nodiscard]] virtual std::vector<ContentPiece> pieces() const {
    return {ContentPiece{text(), {}, false}};
  }
};

/**
 * @brief Decode a byte string that may carry a byte order mark.
 *
 * UTF-8 with a mark has it removed. UTF-16 and UTF-32, which are recognised by
 * their marks and nothing else, throw: they are not supported, and quietly
 * treating their bytes as UTF-8 produces a document of replacement characters
 * that looks like a corrupt file rather than an unsupported encoding.
 *
 * Anything else is returned unchanged and left for the document to validate. A
 * file with no mark is UTF-8 by assumption, which is the only assumption
 * available -- there is no way to tell a Latin-1 file from a UTF-8 one that
 * happens to be ASCII.
 */
[[nodiscard]] std::string stripByteOrderMark(std::string bytes);

/// Reads the whole of a file. What a plain editor opens from its command line.
/// Automatically detects PDF files and paginates them per page.
class FileTextSource : public TextSource {
public:
  explicit FileTextSource(std::string path);

  /// @throws std::runtime_error if the file cannot be read or PDF is locked,
  ///         and std::logic_error for a UTF-16 or UTF-32 byte order mark.
  [[nodiscard]] std::string text() const override;
  [[nodiscard]] std::string name() const override { return filePath; }
  [[nodiscard]] std::vector<std::uint32_t> forcedBreaks() const override;
  [[nodiscard]] std::vector<ContentPiece> pieces() const override;

private:
  void ensureLoaded() const;

  std::string filePath;
  mutable bool loaded{false};
  mutable std::string content;
  mutable std::vector<std::uint32_t> breaks;
  mutable std::vector<ContentPiece> piecesCache;
};

/// Reads a PDF document using poppler-cpp, paginating per PDF page via
/// forcedBreaks.
class PdfTextSource : public TextSource {
public:
  explicit PdfTextSource(std::string path);
  PdfTextSource(const char *data, std::size_t size, std::string aName = {});

  [[nodiscard]] std::string text() const override { return buffer; }
  [[nodiscard]] std::string name() const override { return label; }
  [[nodiscard]] std::vector<std::uint32_t> forcedBreaks() const override {
    return breaks;
  }
  [[nodiscard]] std::size_t pageCount() const { return numPages; }
  /// Per page: that page's text as one piece (the same bytes text()'s own
  /// concatenation carries for it), then one piece per embedded figure found
  /// on it, each tagged "image/png" -- see PdfImageExtractor in
  /// text_source.cpp. A figure follows its page's text rather than being
  /// interleaved at its exact reading position: doing that precisely would
  /// mean reconciling core poppler's page-content coordinate space (where
  /// the image extraction runs) with poppler-cpp's text_list() coordinate
  /// space (where the text layout comes from), and getting that subtly
  /// wrong is worse than not attempting it, per this being exactly the
  /// class of failure Gap E was about. Deduplicated by the PDF's own image
  /// object reference, so a logo repeated across many pages is one piece,
  /// referenced by figures list order rather than stored again.
  [[nodiscard]] std::vector<ContentPiece> pieces() const override {
    return piecesOf;
  }

private:
  void loadPdfFile(const std::string &path);
  void loadPdfData(const char *data, std::size_t size);

  std::string buffer;
  std::string label;
  std::vector<std::uint32_t> breaks;
  std::vector<ContentPiece> piecesOf;
  std::size_t numPages{0};
};

/// Text a caller already has. The source for a document built rather than
/// opened.
class MemoryTextSource : public TextSource {
public:
  explicit MemoryTextSource(std::string contents, std::string aName = {})
      : buffer(std::move(contents)), label(std::move(aName)) {}

  [[nodiscard]] std::string text() const override { return buffer; }
  [[nodiscard]] std::string name() const override { return label; }

private:
  std::string buffer;
  std::string label;
};

} // namespace gleditor

#endif // GLEDITOR_TEXT_SOURCE_H
