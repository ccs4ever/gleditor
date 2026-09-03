/**
 * @file source_grounder.cpp
 * @brief Implementation of text and binary subspan search and SHA-256
 * calculation.
 */
#include <gleditor/source_grounder.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>

#include <gleditor/mimetype.hpp>

namespace gleditor {

namespace {

std::string extractContext(std::string_view text, std::size_t offset,
                           std::size_t length) {
  constexpr std::size_t contextPadding = 32;
  const std::size_t start =
      (offset > contextPadding) ? (offset - contextPadding) : 0;
  const std::size_t end =
      std::min(text.size(), offset + length + contextPadding);
  return std::string{text.substr(start, end - start)};
}

/// Helper to tokenize non-whitespace word tokens from a string view
std::vector<std::string_view> tokenizeWords(std::string_view text) {
  std::vector<std::string_view> tokens;
  std::size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() &&
           std::isspace(static_cast<unsigned char>(text[pos]))) {
      pos++;
    }
    if (pos >= text.size()) {
      break;
    }
    const std::size_t start = pos;
    while (pos < text.size() &&
           !std::isspace(static_cast<unsigned char>(text[pos]))) {
      pos++;
    }
    tokens.push_back(text.substr(start, pos - start));
  }
  return tokens;
}

} // namespace

std::optional<SubspanMatch>
SourceGrounder::locateTextSubspan(std::string_view parentText,
                                  std::string_view quoteSnippet) {
  if (parentText.empty() || quoteSnippet.empty()) {
    return std::nullopt;
  }

  // 1. Exact match attempt
  const auto exactPos = parentText.find(quoteSnippet);
  if (exactPos != std::string_view::npos) {
    return SubspanMatch{
        .offset             = static_cast<std::uint64_t>(exactPos),
        .length             = static_cast<std::uint64_t>(quoteSnippet.size()),
        .mimeType           = "text/plain",
        .sourcePathOrMagnet = "",
        .contextSnippet =
            extractContext(parentText, exactPos, quoteSnippet.size()),
    };
  }

  // 2. Whitespace-normalized match attempt
  const auto quoteTokens = tokenizeWords(quoteSnippet);
  if (quoteTokens.empty()) {
    return std::nullopt;
  }

  // Search for the sequence of tokens in parentText
  std::size_t searchStart = 0;
  while (searchStart < parentText.size()) {
    const auto firstPos = parentText.find(quoteTokens.front(), searchStart);
    if (firstPos == std::string_view::npos) {
      break;
    }

    std::size_t currentPos = firstPos + quoteTokens.front().size();
    bool match             = true;
    for (std::size_t i = 1; i < quoteTokens.size(); i++) {
      while (currentPos < parentText.size() &&
             std::isspace(static_cast<unsigned char>(parentText[currentPos]))) {
        currentPos++;
      }
      if (currentPos + quoteTokens[i].size() > parentText.size() ||
          parentText.substr(currentPos, quoteTokens[i].size()) !=
              quoteTokens[i]) {
        match = false;
        break;
      }
      currentPos += quoteTokens[i].size();
    }

    if (match) {
      const std::size_t matchLength = currentPos - firstPos;
      return SubspanMatch{
          .offset             = static_cast<std::uint64_t>(firstPos),
          .length             = static_cast<std::uint64_t>(matchLength),
          .mimeType           = "text/plain",
          .sourcePathOrMagnet = "",
          .contextSnippet = extractContext(parentText, firstPos, matchLength),
      };
    }

    searchStart = firstPos + 1;
  }

  return std::nullopt;
}

std::optional<SubspanMatch> SourceGrounder::locateBinarySubspan(
    std::span<const std::uint8_t> parentBytes,
    std::span<const std::uint8_t> excerptBytes) {
  if (parentBytes.empty() || excerptBytes.empty() ||
      excerptBytes.size() > parentBytes.size()) {
    return std::nullopt;
  }

  const auto it = std::search(parentBytes.begin(), parentBytes.end(),
                              excerptBytes.begin(), excerptBytes.end());
  if (it == parentBytes.end()) {
    return std::nullopt;
  }

  const auto offset =
      static_cast<std::uint64_t>(std::distance(parentBytes.begin(), it));
  const auto detectedMime = MimeDetector::detectBuffer(excerptBytes);
  return SubspanMatch{
      .offset             = offset,
      .length             = static_cast<std::uint64_t>(excerptBytes.size()),
      .mimeType           = detectedMime.essence(),
      .sourcePathOrMagnet = "",
      .contextSnippet     = "",
  };
}

std::optional<SubspanMatch>
SourceGrounder::groundFile(const std::filesystem::path &sourceFile,
                           std::string_view excerptSnippet) {
  if (!std::filesystem::exists(sourceFile)) {
    return std::nullopt;
  }

  std::ifstream file(sourceFile, std::ios::binary);
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::string buffer((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
  auto match = locateTextSubspan(buffer, excerptSnippet);
  if (match) {
    match->sourcePathOrMagnet = sourceFile.string();
    const auto fileMime       = MimeDetector::detectFile(sourceFile.string());
    if (fileMime.isText()) {
      match->mimeType = fileMime.essence();
    }
  }
  return match;
}

std::optional<SubspanMatch>
SourceGrounder::groundFileBinary(const std::filesystem::path &sourceFile,
                                 std::span<const std::uint8_t> excerptBytes) {
  if (!std::filesystem::exists(sourceFile)) {
    return std::nullopt;
  }

  std::ifstream file(sourceFile, std::ios::binary);
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> buffer((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
  auto match = locateBinarySubspan(buffer, excerptBytes);
  if (match) {
    match->sourcePathOrMagnet = sourceFile.string();
  }
  return match;
}

std::string SourceGrounder::computeSha256(std::span<const std::uint8_t> bytes) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(bytes.data(), bytes.size(), digest.data());

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (const auto b : digest) {
    oss << std::setw(2) << static_cast<unsigned int>(b);
  }
  return oss.str();
}

std::string SourceGrounder::computeSha256(std::string_view text) {
  return computeSha256(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

} // namespace gleditor
