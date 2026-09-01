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
#include <string>
#include <utility>
#include <vector>

namespace gleditor {

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

private:
  void ensureLoaded() const;

  std::string filePath;
  mutable bool loaded{false};
  mutable std::string content;
  mutable std::vector<std::uint32_t> breaks;
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

private:
  void loadPdfFile(const std::string &path);
  void loadPdfData(const char *data, std::size_t size);

  std::string buffer;
  std::string label;
  std::vector<std::uint32_t> breaks;
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
