/**
 * @file batch_orchestrator.cpp
 * @brief Decoupled batch CLI and headless orchestration pipeline for Xudu.
 */
#include "batch_orchestrator.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gleditor/text_source.hpp>

#include "common/xanadu/format.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/torrent.hpp"

namespace xudu {

LinkType BatchOrchestrator::parseLinkType(const std::string_view str) {
  if (str == "comment" || str == "Comment") {
    return LinkType::Comment;
  }
  if (str == "illustration" || str == "Illustration") {
    return LinkType::Illustration;
  }
  if (str == "disagreement" || str == "Disagreement") {
    return LinkType::Disagreement;
  }
  if (str == "authorship" || str == "Authorship") {
    return LinkType::Authorship;
  }
  if (str == "quotation" || str == "Quotation") {
    return LinkType::Quotation;
  }
  if (str == "format" || str == "Format") {
    return LinkType::Format;
  }
  if (str == "dimension" || str == "Dimension") {
    return LinkType::Dimension;
  }
  return LinkType::Other;
}

ProminenceTier
BatchOrchestrator::parseProminenceTier(const std::string_view str) {
  if (str == "curator" || str == "Curator" || str == "curated" ||
      str == "Curated") {
    return ProminenceTier::Curated;
  }
  if (str == "public" || str == "Public" || str == "reader" ||
      str == "Reader") {
    return ProminenceTier::Public;
  }
  return ProminenceTier::Author;
}

FormatAttribute
BatchOrchestrator::parseFormatAttribute(const std::string_view str) {
  if (str == "italic" || str == "Italic") {
    return FormatAttribute::Italic;
  }
  if (str == "underline" || str == "Underline") {
    return FormatAttribute::Underline;
  }
  if (str == "overline" || str == "Overline") {
    return FormatAttribute::Overline;
  }
  if (str == "strikethrough" || str == "Strikethrough") {
    return FormatAttribute::Strikethrough;
  }
  if (str == "superscript" || str == "Superscript") {
    return FormatAttribute::Superscript;
  }
  if (str == "subscript" || str == "Subscript") {
    return FormatAttribute::Subscript;
  }
  return FormatAttribute::Bold;
}

std::vector<PrimediaSpan> BatchOrchestrator::resolveSingleSpanToken(
    const Session &session, const std::string &token,
    const std::optional<std::uint32_t> defaultDocIdx) {
  if (token.starts_with("vocab:") || token.starts_with("VOCAB:")) {
    const auto attrName = token.substr(6);
    return {vocabularySpanFor(parseFormatAttribute(attrName))};
  }

  const auto atPos = token.find('@');
  if (atPos != std::string::npos) {
    const auto docIdx =
        static_cast<std::uint32_t>(std::stoul(token.substr(0, atPos)));
    auto rem = token.substr(atPos + 1);
    std::string query;
    std::optional<std::uint32_t> customLen;
    const auto colon = rem.rfind(':');
    if (colon != std::string::npos && colon > 0 &&
        std::isdigit(static_cast<unsigned char>(rem[colon + 1]))) {
      query     = rem.substr(0, colon);
      customLen = static_cast<std::uint32_t>(std::stoul(rem.substr(colon + 1)));
    } else {
      query = rem;
    }
    if (query.size() >= 2 && query.front() == '"' && query.back() == '"') {
      query = query.substr(1, query.size() - 2);
    }
    const auto &vList = session.views();
    if (docIdx >= vList.size()) {
      throw std::runtime_error("document index out of range: " + token);
    }
    const auto sIdx     = vList[docIdx].storeIndex;
    const auto docText  = session.store(sIdx).textOf(vList[docIdx].version);
    const auto matchPos = docText.find(query);
    if (matchPos == std::string::npos) {
      throw std::runtime_error("query substring not found in document " +
                               std::to_string(docIdx) + ": " + query);
    }
    const auto ver = session.store(sIdx).rebuild(vList[docIdx].version);
    const auto spanLen =
        customLen ? *customLen : static_cast<std::uint32_t>(query.size());
    return ver.spansFor(static_cast<std::uint32_t>(matchPos), spanLen);
  }

  const auto firstColon = token.find(':');
  if (firstColon == std::string::npos) {
    throw std::runtime_error("invalid span specifier: " + token);
  }
  const auto secondColon = token.find(':', firstColon + 1);
  std::uint32_t docIdx   = 0;
  std::uint32_t start    = 0;
  std::uint32_t len      = 0;
  if (secondColon != std::string::npos) {
    docIdx =
        static_cast<std::uint32_t>(std::stoul(token.substr(0, firstColon)));
    start = static_cast<std::uint32_t>(
        std::stoul(token.substr(firstColon + 1, secondColon - firstColon - 1)));
    len = static_cast<std::uint32_t>(std::stoul(token.substr(secondColon + 1)));
  } else {
    if (!defaultDocIdx) {
      throw std::runtime_error("span specifier missing document index: " +
                               token);
    }
    docIdx = *defaultDocIdx;
    start = static_cast<std::uint32_t>(std::stoul(token.substr(0, firstColon)));
    len = static_cast<std::uint32_t>(std::stoul(token.substr(firstColon + 1)));
  }
  const auto &vList = session.views();
  if (docIdx >= vList.size()) {
    throw std::runtime_error("document index out of range: " + token);
  }
  const auto sIdx = vList[docIdx].storeIndex;
  const auto ver  = session.store(sIdx).rebuild(vList[docIdx].version);
  return ver.spansFor(start, len);
}

std::vector<PrimediaSpan>
BatchOrchestrator::resolveSpans(const Session &session,
                                const std::string &spec) {
  std::vector<PrimediaSpan> result;
  std::stringstream ss(spec);
  std::string token;
  std::optional<std::uint32_t> leadingDocIdx;

  while (std::getline(ss, token, '+')) {
    if (token.empty()) {
      continue;
    }
    auto sp = resolveSingleSpanToken(session, token, leadingDocIdx);
    if (!token.empty() && token.contains(':') && !leadingDocIdx) {
      const auto c = token.find(':');
      if (token.find(':', c + 1) != std::string::npos) {
        leadingDocIdx =
            static_cast<std::uint32_t>(std::stoul(token.substr(0, c)));
      }
    }
    result.insert(result.end(), sp.begin(), sp.end());
  }
  return result;
}

BatchOrchestrator::ExecutionResult
BatchOrchestrator::execute(Session &session,
                           const argparse::ArgumentParser &parser,
                           const bool quiet) {
  const bool headless =
      parser["--headless"] == true || parser["--batch"] == true;
  MicroversionId opening;
  std::vector<std::pair<MicroversionId, std::size_t>> extraImports;

  // If in headless/batch mode, register existing versions for span resolution
  if (headless && session.views().empty() && session.store(0).opCount() > 0) {
    const auto &curVers = session.store(0).currentVersions();
    if (!curVers.empty()) {
      for (const auto &v : curVers) {
        session.viewOpened(v, 0);
      }
    } else {
      session.viewOpened(session.store(0).latest(), 0);
    }
  }

  // Collect import files from --import and positional files
  std::vector<std::string> importFiles;
  if (parser.present<std::vector<std::string>>("--import")) {
    for (const auto &f : parser.get<std::vector<std::string>>("--import")) {
      if (!f.empty()) {
        importFiles.push_back(f);
      }
    }
  }
  if (parser.present<std::vector<std::string>>("files")) {
    for (const auto &f : parser.get<std::vector<std::string>>("files")) {
      if (!f.empty()) {
        importFiles.push_back(f);
      }
    }
  }

  if (!importFiles.empty()) {
    std::size_t startIdx  = 0;
    const auto &firstFile = importFiles[0];
    if (const auto kind = systemDocKindFromUri(firstFile)) {
      const auto openedVer = session.openSystemDoc(*kind);
      opening              = openedVer;
      startIdx             = 1;
      if (!quiet) {
        std::cout << "xudu: opened system doc " << firstFile << " as "
                  << openedVer.str() << "\n";
      }
    } else if (0 == session.store(0).opCount()) {
      const gleditor::FileTextSource source(firstFile);
      MicroversionId imported;
      std::uint32_t at = 0;
      std::vector<PrimediaSpan> insertedSpans;
      const auto pieces = source.pieces();
      insertedSpans.reserve(pieces.size());
      for (const auto &piece : pieces) {
        PrimediaSpan span;
        if (piece.duplicateOfPieceIndex.has_value() &&
            *piece.duplicateOfPieceIndex < insertedSpans.size()) {
          span     = insertedSpans[*piece.duplicateOfPieceIndex];
          imported = session.store(0).insertSpan(imported, at, span);
        } else if (piece.mimeType.empty()) {
          imported = session.store(0).insert(imported, at, piece.bytes);
        } else {
          std::filesystem::path p(firstFile);
          std::string pieceFilePath = firstFile;
          std::string fileName      = p.filename().string();
          if (!std::filesystem::is_regular_file(p) ||
              std::filesystem::file_size(p) != piece.bytes.size()) {
            const auto &stPath  = session.path(0);
            const auto mediaDir = stPath.empty()
                                      ? std::filesystem::temp_directory_path()
                                      : std::filesystem::path(stPath);
            if (!std::filesystem::exists(mediaDir)) {
              std::filesystem::create_directories(mediaDir);
            }
            fileName = "fig_" + std::to_string(insertedSpans.size()) + ".dat";
            p        = mediaDir / fileName;
            pieceFilePath = p.string();
            std::ofstream out(pieceFilePath, std::ios::binary);
            out.write(piece.bytes.data(),
                      static_cast<std::streamsize>(piece.bytes.size()));
          }
          const auto made = makeTorrent(piece.bytes, fileName);
          const auto dataRoot =
              p.parent_path().empty() ? "." : p.parent_path().string();
          session.addTorrentMemory(made.file, dataRoot);

          auto scroll = Scroll::ofTorrentFile(made.hash, 0, pieceFilePath, 0,
                                              piece.bytes.size());
          scroll.defaultMimeType = piece.mimeType;
          if (!scroll.segments.empty()) {
            scroll.segments[0].mimeType = piece.mimeType;
          }
          const auto sId = session.store(0).addScroll(scroll);
          span           = PrimediaSpan{
              .scroll = sId, .start = 0, .length = piece.bytes.size()};
          Op op;
          op.kind  = OpKind::Transclude;
          op.at    = at;
          op.span  = span;
          imported = session.store(0).apply(imported, op);
        }
        insertedSpans.push_back(span);
        at += static_cast<std::uint32_t>(piece.bytes.size());
        if (piece.pageBreakAfter) {
          imported = session.store(0).insertBreak(imported, at);
        }
      }
      session.save(0);
      opening = imported;
      session.viewOpened(imported, 0);
      if (!quiet) {
        std::cout << "xudu: imported " << firstFile << " as " << imported.str()
                  << "\n";
      }
      startIdx = 1;
    }
    for (std::size_t i = startIdx; i < importFiles.size(); ++i) {
      const auto &f = importFiles[i];
      if (const auto kind = systemDocKindFromUri(f)) {
        const auto sIdx      = session.systemStoreIndex(*kind);
        const auto openedVer = session.openSystemDoc(*kind);
        extraImports.emplace_back(openedVer, sIdx);
        if (!quiet) {
          std::cout << "xudu: opened " << f << " in system store " << sIdx
                    << " as " << openedVer.str() << "\n";
        }
      } else {
        const auto [sIdx, imported] = session.importFileToTemporaryStore(f);
        session.viewOpened(imported, sIdx);
        extraImports.emplace_back(imported, sIdx);
        if (!quiet) {
          std::cout << "xudu: imported " << f << " to temp store " << sIdx
                    << " as " << imported.str() << "\n";
        }
      }
    }
  }

  if (parser.present<std::vector<std::string>>("--system-doc")) {
    for (const auto &name :
         parser.get<std::vector<std::string>>("--system-doc")) {
      const std::string uri =
          name.starts_with("system://") ? name : "system://" + name;
      if (const auto kind = systemDocKindFromUri(uri)) {
        const auto sIdx      = session.systemStoreIndex(*kind);
        const auto openedVer = session.openSystemDoc(*kind);
        extraImports.emplace_back(openedVer, sIdx);
        if (!quiet) {
          std::cout << "xudu: opened system doc " << uri << " in store " << sIdx
                    << " as " << openedVer.str() << "\n";
        }
      }
    }
  }

  if (parser.present<std::vector<std::string>>("--open-store")) {
    for (const auto &p : parser.get<std::vector<std::string>>("--open-store")) {
      if (!p.empty() && std::filesystem::exists(p)) {
        const auto sIdx = session.loadAuxiliaryStore(p);
        if (session.store(sIdx).opCount() > 0) {
          const auto latestVer = session.store(sIdx).latest();
          session.viewOpened(latestVer, sIdx);
        }
      }
    }
  }

  if (const auto scriptPath = parser.get<std::string>("--structure-script");
      !scriptPath.empty()) {
    std::ifstream script(scriptPath);
    if (!script) {
      throw std::runtime_error("cannot open structure script: " + scriptPath);
    }

    auto &store  = session.store(0);
    auto version = store.latest();
    std::unordered_map<std::string, zigzag::CellRef> cells;
    std::unordered_map<std::string, zigzag::DimRef> dimensions;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(script, line)) {
      ++lineNumber;
      const auto first = line.find_first_not_of(" \t");
      if (first == std::string::npos || line[first] == '#') {
        continue;
      }
      line.erase(0, first);
      const auto space   = line.find_first_of(" \t");
      const auto command = line.substr(0, space);
      const auto argument =
          space == std::string::npos ? std::string{} : line.substr(space + 1);
      const auto fail = [&](const std::string &reason) {
        std::string message = "structure script ";
        message += scriptPath;
        message += ":";
        message += std::to_string(lineNumber);
        message += ": ";
        message += reason;
        throw std::runtime_error(message);
      };

      if (command == "genesis") {
        if (!version.isZero()) {
          fail("genesis requires an empty store");
        }
        version = store.sliceGenesis({});
      } else if (command == "dimension") {
        if (argument.empty()) {
          fail("dimension requires a name");
        }
        const auto made = store.makeDimension(version, argument);
        version         = made.version;
        dimensions.emplace(argument, made.dim);
      } else if (command == "cell") {
        const auto nameEnd = argument.find_first_of(" \t");
        if (nameEnd == std::string::npos) {
          fail("cell requires a name and text");
        }
        const auto name = argument.substr(0, nameEnd);
        const auto text =
            argument.substr(argument.find_first_not_of(" \t", nameEnd));
        version = store.makeCell(version, text);
        cells.emplace(name, store.cellRefOf(version));
      } else if (command == "cell-text") {
        const auto nameEnd = argument.find_first_of(" \t");
        if (nameEnd == std::string::npos) {
          fail("cell-text requires a cell name and text");
        }
        const auto name  = argument.substr(0, nameEnd);
        const auto found = cells.find(name);
        if (found == cells.end()) {
          fail("unknown cell: " + name);
        }
        const auto text =
            argument.substr(argument.find_first_not_of(" \t", nameEnd));
        version = store.setCellText(version, found->second, text);
      } else if (command == "text" || command == "text-append") {
        if (argument.empty()) {
          fail(command + " requires text");
        }
        const auto length = store.rebuild(version).length();
        const auto at =
            command == "text" ? 0U : static_cast<std::uint32_t>(length);
        version = store.insert(version, at, argument);
      } else {
        fail("unknown command: " + command);
      }
    }
    if (version.isZero()) {
      throw std::runtime_error("structure script made no operations: " +
                               scriptPath);
    }
    session.viewOpened(version, 0);
    opening = version;
  }

  if (parser.present<std::vector<std::string>>("--import-branch")) {
    for (const auto &f :
         parser.get<std::vector<std::string>>("--import-branch")) {
      if (!f.empty()) {
        const auto imported = session.importBranch(0, f);
        session.viewOpened(imported, 0);
        if (!quiet) {
          std::cout << "xudu: imported branch " << f << " as " << imported.str()
                    << "\n";
        }
      }
    }
  }

  if (parser.present<std::vector<std::string>>("--import-break")) {
    for (const auto &f :
         parser.get<std::vector<std::string>>("--import-break")) {
      if (!f.empty()) {
        const gleditor::FileTextSource source(f);
        const auto curVer = !session.views().empty()
                                ? session.views()[0].version
                                : session.store(0).latest();
        const auto len    = session.store(0).rebuild(curVer).length();
        auto nextVer      = session.store(0).insertBreak(
            curVer, static_cast<std::uint32_t>(len));
        nextVer = session.store(0).insert(
            nextVer, static_cast<std::uint32_t>(len), source.text());
        for (const auto brk : source.forcedBreaks()) {
          nextVer = session.store(0).insertBreak(
              nextVer, static_cast<std::uint32_t>(len + brk));
        }
        session.save(0);
        if (!session.views().empty()) {
          session.views()[0].version = nextVer;
          session.views()[0].pieces  = session.store(0).rebuild(nextVer);
        } else {
          session.viewOpened(nextVer, 0);
        }
        opening = nextVer;
      }
    }
  }

  if (parser.present<std::vector<std::string>>("--transclude")) {
    for (const auto &spec :
         parser.get<std::vector<std::string>>("--transclude")) {
      const auto comma = spec.find(',');
      if (comma != std::string::npos) {
        const auto leftStr  = spec.substr(0, comma);
        const auto rightStr = spec.substr(comma + 1);
        const auto c1       = leftStr.find(':');
        const auto c2       = leftStr.rfind(':');
        const auto c3       = rightStr.find(':');
        if (c1 != std::string::npos && c2 != std::string::npos &&
            c3 != std::string::npos) {
          const auto srcDoc =
              static_cast<std::uint32_t>(std::stoul(leftStr.substr(0, c1)));
          const auto srcStart = static_cast<std::uint32_t>(
              std::stoul(leftStr.substr(c1 + 1, c2 - c1 - 1)));
          const auto srcLen =
              static_cast<std::uint32_t>(std::stoul(leftStr.substr(c2 + 1)));

          const auto destDoc =
              static_cast<std::uint32_t>(std::stoul(rightStr.substr(0, c3)));
          const auto destPosStr = rightStr.substr(c3 + 1);
          const auto vList      = session.views();
          if (srcDoc < vList.size() && destDoc < vList.size()) {
            const auto &destSt = session.store(vList[destDoc].storeIndex);
            const auto destLen =
                destSt.rebuild(vList[destDoc].version).length();
            const auto destPos =
                (destPosStr == "append" || destPosStr == "end")
                    ? static_cast<std::uint32_t>(destLen)
                    : static_cast<std::uint32_t>(std::stoul(destPosStr));
            session.transclude(destDoc, destPos, srcDoc, srcStart, srcLen);
          }
        }
      }
    }
  }

  if (parser.present<std::vector<std::string>>("--transclude-text")) {
    for (const auto &spec :
         parser.get<std::vector<std::string>>("--transclude-text")) {
      const auto comma = spec.rfind(',');
      if (comma != std::string::npos) {
        const auto leftStr   = spec.substr(0, comma);
        const auto rightStr  = spec.substr(comma + 1);
        const auto atOrColon = leftStr.find('@');
        const auto c1 =
            (atOrColon != std::string::npos) ? atOrColon : leftStr.find(':');
        const auto c2 = rightStr.find(':');
        if (c1 != std::string::npos && c2 != std::string::npos) {
          const auto srcDoc =
              static_cast<std::uint32_t>(std::stoul(leftStr.substr(0, c1)));
          const auto query = leftStr.substr(c1 + 1);
          const auto destDoc =
              static_cast<std::uint32_t>(std::stoul(rightStr.substr(0, c2)));
          const auto destPosStr = rightStr.substr(c2 + 1);
          const auto vList      = session.views();
          if (srcDoc < vList.size() && destDoc < vList.size()) {
            const auto &destSt = session.store(vList[destDoc].storeIndex);
            const auto destLen =
                destSt.rebuild(vList[destDoc].version).length();
            const auto destPos =
                (destPosStr == "append" || destPosStr == "end")
                    ? static_cast<std::uint32_t>(destLen)
                    : static_cast<std::uint32_t>(std::stoul(destPosStr));
            session.transcludeText(destDoc, destPos, srcDoc, query);
          }
        }
      }
    }
  }

  if (parser.present<std::vector<std::string>>("--insert-text")) {
    for (const auto &spec :
         parser.get<std::vector<std::string>>("--insert-text")) {
      const auto c1 = spec.find(':');
      if (c1 != std::string::npos) {
        const auto c2 = spec.find(':', c1 + 1);
        if (c2 != std::string::npos) {
          const auto docIdx =
              static_cast<std::uint32_t>(std::stoul(spec.substr(0, c1)));
          const auto posStr     = spec.substr(c1 + 1, c2 - c1 - 1);
          const auto textOrFile = spec.substr(c2 + 1);
          std::string content;
          bool isFile = false;
          if (std::filesystem::exists(textOrFile)) {
            isFile = true;
            std::ifstream in(textOrFile, std::ios::binary);
            content.assign(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
          } else {
            content = textOrFile;
          }
          const auto vList = session.views();
          if (docIdx < vList.size()) {
            const auto &st    = session.store(vList[docIdx].storeIndex);
            const auto curLen = st.rebuild(vList[docIdx].version).length();
            const auto pos =
                (posStr == "append" || posStr == "end")
                    ? static_cast<std::uint32_t>(curLen)
                    : static_cast<std::uint32_t>(std::stoul(posStr));
            const gleditor::MagicMimeDetector magic;
            const auto mime =
                isFile ? magic.identifyFile(textOrFile) : std::string{};
            if (isFile && gleditor::MagicMimeDetector::isMediaMime(mime)) {
              session.insertMedia(docIdx, pos, content, mime, textOrFile);
            } else {
              session.insertText(docIdx, pos, content);
            }
          }
        }
      }
    }
  }

  if (parser.present<std::vector<std::string>>("--link")) {
    for (const auto &spec : parser.get<std::vector<std::string>>("--link")) {
      const auto comma = spec.find(',');
      if (comma != std::string::npos) {
        const auto leftSpec = spec.substr(0, comma);
        const auto rem      = spec.substr(comma + 1);

        static const std::vector<std::string> knownTypes = {
            "comment",   "illustration", "disagreement", "authorship",
            "quotation", "format",       "dimension",    "other"};

        std::size_t foundTypePos = std::string::npos;
        for (const auto &kt : knownTypes) {
          auto pos = rem.find(":" + kt);
          while (pos != std::string::npos) {
            const auto after = pos + 1 + kt.size();
            if (after == rem.size() || rem[after] == ':') {
              if (foundTypePos == std::string::npos || pos > foundTypePos) {
                foundTypePos = pos;
              }
            }
            pos = rem.find(":" + kt, pos + 1);
          }
        }

        std::string rightSpec;
        std::string typeStr  = "quotation";
        std::string tierStr  = "author";
        std::string ownerStr = "Theodor_Holm_Nelson";

        if (foundTypePos != std::string::npos) {
          rightSpec          = rem.substr(0, foundTypePos);
          const auto attrStr = rem.substr(foundTypePos + 1);
          const auto c1      = attrStr.find(':');
          if (c1 != std::string::npos) {
            typeStr       = attrStr.substr(0, c1);
            const auto c2 = attrStr.find(':', c1 + 1);
            if (c2 != std::string::npos) {
              tierStr  = attrStr.substr(c1 + 1, c2 - c1 - 1);
              ownerStr = attrStr.substr(c2 + 1);
            } else {
              tierStr = attrStr.substr(c1 + 1);
            }
          } else {
            typeStr = attrStr;
          }
        } else {
          rightSpec = rem;
        }

        auto leftSpans  = resolveSpans(session, leftSpec);
        auto rightSpans = resolveSpans(session, rightSpec);

        Link l;
        l.type  = parseLinkType(typeStr);
        l.tier  = parseProminenceTier(tierStr);
        l.owner = ownerStr;
        l.left  = std::move(leftSpans);
        l.right = std::move(rightSpans);

        session.addLink(0, std::move(l));
      }
    }
  }

  if (parser.present<std::vector<std::string>>("--format-link")) {
    for (const auto &spec :
         parser.get<std::vector<std::string>>("--format-link")) {
      static const std::vector<std::string> knownAttrs = {
          "bold",          "italic",      "underline", "overline",
          "strikethrough", "superscript", "subscript",
      };

      std::size_t foundAttrPos = std::string::npos;
      for (const auto &ka : knownAttrs) {
        auto pos = spec.find(":" + ka);
        while (pos != std::string::npos) {
          const auto after = pos + 1 + ka.size();
          if (after == spec.size() || spec[after] == ':') {
            if (foundAttrPos == std::string::npos || pos > foundAttrPos) {
              foundAttrPos = pos;
            }
          }
          pos = spec.find(":" + ka, pos + 1);
        }
      }

      std::string spanSpec;
      std::string attrStr  = "bold";
      std::string tierStr  = "author";
      std::string ownerStr = "Theodor_Holm_Nelson";

      if (foundAttrPos != std::string::npos) {
        spanSpec           = spec.substr(0, foundAttrPos);
        const auto attrRem = spec.substr(foundAttrPos + 1);
        const auto c1      = attrRem.find(':');
        if (c1 != std::string::npos) {
          attrStr       = attrRem.substr(0, c1);
          const auto c2 = attrRem.find(':', c1 + 1);
          if (c2 != std::string::npos) {
            tierStr  = attrRem.substr(c1 + 1, c2 - c1 - 1);
            ownerStr = attrRem.substr(c2 + 1);
          } else {
            tierStr = attrRem.substr(c1 + 1);
          }
        } else {
          attrStr = attrRem;
        }
      } else {
        spanSpec = spec;
      }

      auto targetSpans = resolveSpans(session, spanSpec);
      Link l;
      l.type  = LinkType::Format;
      l.tier  = parseProminenceTier(tierStr);
      l.owner = ownerStr;
      l.left  = std::move(targetSpans);
      l.right = {vocabularySpanFor(parseFormatAttribute(attrStr))};
      session.addLink(0, std::move(l));
    }
  }

  if (parser.present<std::vector<std::string>>("--dimension-link")) {
    for (const auto &spec :
         parser.get<std::vector<std::string>>("--dimension-link")) {
      const auto comma = spec.find(',');
      if (comma != std::string::npos) {
        const auto leftSpec = spec.substr(0, comma);
        const auto rem      = spec.substr(comma + 1);
        std::string rightSpec;
        std::string dimName = "dimension:d.concept";
        const auto dimPos   = rem.find(":dimension:");
        const auto dDotPos  = rem.find(":d.");
        if (dimPos != std::string::npos) {
          rightSpec = rem.substr(0, dimPos);
          dimName   = rem.substr(dimPos + 1);
        } else if (dDotPos != std::string::npos) {
          rightSpec = rem.substr(0, dDotPos);
          dimName   = "dimension:" + rem.substr(dDotPos + 1);
        } else {
          const auto colon = rem.rfind(':');
          rightSpec = (colon != std::string::npos) ? rem.substr(0, colon) : rem;
          dimName   = (colon != std::string::npos) ? rem.substr(colon + 1)
                                                   : "dimension:d.concept";
        }
        auto leftSpans  = resolveSpans(session, leftSpec);
        auto rightSpans = resolveSpans(session, rightSpec);
        Link l;
        l.type  = LinkType::Dimension;
        l.tier  = ProminenceTier::Author;
        l.owner = dimName;
        l.left  = std::move(leftSpans);
        l.right = std::move(rightSpans);
        session.addLink(0, std::move(l));
      }
    }
  }

  const bool hasScript =
      (parser.is_used("--do") || parser.is_used("--select") ||
       parser.is_used("--type") || parser.is_used("--key") ||
       parser.is_used("--click") || parser.is_used("--pick"));

  if (headless && !hasScript) {
    // Headless export temporarily opens every historical version so span
    // tokens can resolve against them. Those helper views are not a request
    // for a side-by-side presentation: persist one current head for each
    // exported store, while interactive multi-view sessions retain all heads.
    if (parser["--export-osmic"] == true) {
      for (std::size_t i = 0; i < session.storeCount(); ++i) {
        const auto latest = session.store(i).latest();
        if (!latest.isZero()) {
          session.store(i).repointCurrentVersion(latest);
        }
      }
      // Span resolution may have opened every historical state. Those views
      // are batch-only scaffolding, not a request to persist a gallery of
      // current heads; saveAll() otherwise serialises each one.
      session.views().clear();
    }
    session.saveAll();
    if (parser["--export-osmic"] == true) {
      session.saveOsmicTextAll();
    }
    if (const auto outPerma = parser.get<std::string>("--dump-permascroll");
        !outPerma.empty()) {
      session.dumpPermascroll(outPerma);
    }
    return {.shouldExit   = true,
            .exitCode     = 0,
            .opening      = opening,
            .extraImports = std::move(extraImports)};
  }

  return {.shouldExit   = false,
          .exitCode     = 0,
          .opening      = opening,
          .extraImports = std::move(extraImports)};
}

} // namespace xudu
