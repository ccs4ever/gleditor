/**
 * @file xudu-dump.cpp
 * @brief Render any section of a xudu store as text.
 *
 * R11 says a format is free to stop being human-readable, on the condition
 * that a tool can show it: "debuggability is a tool's job, not a format's".
 * This is that tool, and it lands before the formats it exists to read stop
 * being plaintext rather than after, because a binary format whose only reader
 * is the loader you are debugging is how "we will write the tool later"
 * becomes "we cannot debug the loader".
 *
 * **Nothing here goes through Store::load().** That is the whole design, not
 * an oversight: the case this exists for is a store the loader refuses, and a
 * dump tool that needs the loader to work first cannot be pointed at one. It
 * reads the bytes and renders what it finds, reports what it cannot make sense
 * of, and never throws.
 *
 * It reuses the layout *types* -- OpsSegmentHeader, CompactOpNode,
 * MicroversionId -- because those are the description of the bytes rather than
 * the machinery for reading them. It does not reuse SegmentedOpsSpool.
 *
 * The output is meant to be diffed. Migration steps 10 and 11 change how a
 * store is written without changing what it says, so `--section=ops` before
 * and after such a change must be identical, and `--section=header` is where
 * a version bump is allowed to show. That is why every record renders what a
 * field *means* -- including the text an operation's span names, which is what
 * would have caught migration step 1's silent field shift the day it landed.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <xudu/core/compact_op.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/segmented_ops_spool.hpp>
#include <xudu/core/store_tables.hpp>

namespace {

using xudu::CompactOpNode;
using xudu::MicroversionId;
using xudu::OpsSegmentHeader;

/// Whether anything could not be made sense of. The exit status, so that a
/// script can tell "dumped a healthy store" from "dumped what it could".
bool sawTrouble = false;

void trouble(const std::string &what) {
  std::cerr << "xudu-dump: " << what << '\n';
  sawTrouble = true;
}

std::string readWhole(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @p text with anything unprintable spelled out, cut at @p limit characters.
/// A dump is read by a person and pasted into a bug report, so a raw video
/// frame in a span must not take the terminal with it.
///
/// Not called `quoted`. That name is std::quoted's, and an unqualified call
/// with a std::string argument finds it by ADL and prefers it -- binding a
/// reference beats converting to string_view -- so half the call sites here
/// silently got a stream manipulator that escapes quotes, passes newlines
/// through raw and honours no limit at all.
std::string excerpt(const std::string_view text, const std::size_t limit = 48) {
  std::string out  = "\"";
  std::size_t used = 0;
  for (const auto c : text) {
    if (used >= limit) {
      out += "\"...";
      return out;
    }
    const auto byte = static_cast<unsigned char>(c);
    if ('"' == c || '\\' == c) {
      out += '\\';
      out += c;
      used += 2;
    } else if ('\n' == c) {
      out += "\\n";
      used += 2;
    } else if ('\t' == c) {
      out += "\\t";
      used += 2;
    } else if (byte < 0x20 || byte >= 0x7f) {
      std::array<char, 8> hex{};
      std::snprintf(hex.data(), hex.size(), "\\x%02x", byte);
      out += hex.data();
      used += 4;
    } else {
      out += c;
      used++;
    }
  }
  out += '"';
  return out;
}

// -- ops.nodes ---------------------------------------------------------------

/// What an ops segment file turned out to hold. Deliberately tolerant: a
/// header that fails a check is still reported field by field, because the
/// field that is wrong is the thing being looked for.
struct OpsFile {
  bool hasHeader{false};
  OpsSegmentHeader header{};
  std::vector<CompactOpNode> nodes;
  std::uint32_t firstOpIndex{1};
};

OpsFile readOpsFile(const std::filesystem::path &path) {
  OpsFile file;
  const auto bytes = readWhole(path);
  if (bytes.size() < sizeof(OpsSegmentHeader)) {
    trouble(path.string() + ": " + std::to_string(bytes.size()) +
            " bytes, too short to hold a header");
    return file;
  }
  std::memcpy(&file.header, bytes.data(), sizeof(OpsSegmentHeader));
  file.hasHeader = file.header.signature == xudu::opsSegmentSignature;
  if (!file.hasHeader) {
    trouble(path.string() +
            ": no operations segment signature. Written before headers "
            "existed, or not an operations segment at all.");
    return file;
  }
  file.firstOpIndex = file.header.firstOpIndex;

  const auto start = file.header.headerBytes;
  const auto size  = file.header.nodeSize;
  if (0 == size || start > bytes.size()) {
    trouble(path.string() + ": header says nodes start at " +
            std::to_string(start) + " and are " + std::to_string(size) +
            " bytes; the file is " + std::to_string(bytes.size()));
    return file;
  }
  if (size != sizeof(CompactOpNode)) {
    // Reported, not fatal: saying how many operations of the other size are
    // in there is more use than refusing to look.
    trouble(path.string() + ": nodes are " + std::to_string(size) +
            " bytes and this build's are " +
            std::to_string(sizeof(CompactOpNode)) +
            "; not rendering operations, since every field would be at the "
            "wrong offset");
    return file;
  }
  const auto after = bytes.size() - start;
  if (0 != after % size) {
    trouble(path.string() + ": " + std::to_string(after % size) +
            " bytes left over after the last whole operation");
  }
  file.nodes.resize(after / size);
  if (!file.nodes.empty()) {
    std::memcpy(file.nodes.data(), bytes.data() + start,
                file.nodes.size() * sizeof(CompactOpNode));
  }
  return file;
}

/// The microversion each operation produces, worked out from the tree exactly
/// as the loader does, keyed by spool index. Reimplemented here in six lines
/// rather than borrowed, because borrowing it would mean borrowing the loader.
std::map<std::uint32_t, MicroversionId> namesOf(const OpsFile &file) {
  std::map<std::uint32_t, MicroversionId> names;
  names[0] = MicroversionId{};
  for (std::size_t i = 0; i < file.nodes.size(); i++) {
    const auto index  = static_cast<std::uint32_t>(file.firstOpIndex + i);
    const auto &node  = file.nodes[i];
    const auto parent = names.find(node.parentIndex);
    if (parent == names.end() || node.parentIndex >= index) {
      continue; // an unknown or impossible parent: this one has no name here
    }
    names[index] = 0 == node.branchOrdinal
                       ? parent->second.next()
                       : parent->second.branch(node.branchOrdinal);
  }
  return names;
}

void dumpOpsHeader(const OpsFile &file) {
  if (!file.hasHeader) {
    std::cout << "header    absent\n";
    return;
  }
  const auto &h = file.header;
  std::cout << "header    signature=ok format=" << h.formatVersion
            << " headerBytes=" << h.headerBytes << " nodeSize=" << h.nodeSize
            << '\n';
  std::cout << "header    flags=0x" << std::hex << h.flags << std::dec
            << " firstOpIndex=" << h.firstOpIndex
            << " nodeCount=" << h.nodeCount << " present=" << file.nodes.size()
            << '\n';
  bool rooted = false;
  for (const auto byte : h.merkleRoot) {
    rooted = rooted || 0 != byte;
  }
  std::cout << "header    merkleRoot="
            << ((h.flags & xudu::opsSegmentFlagMerkleRoot) != 0 ? "present"
                : rooted ? "set-but-unflagged"
                         : "absent")
            << " reservedZero=" << h.reservedZero << '\n';
  if (h.nodeCount != file.nodes.size()) {
    trouble("header says " + std::to_string(h.nodeCount) +
            " operations, file holds " + std::to_string(file.nodes.size()));
  }
}

void dumpOps(const OpsFile &file, const std::string &primedia) {
  const auto names = namesOf(file);
  for (std::size_t i = 0; i < file.nodes.size(); i++) {
    const auto index = static_cast<std::uint32_t>(file.firstOpIndex + i);
    const auto &node = file.nodes[i];
    const auto named = names.find(index);
    const auto span  = node.span();

    std::ostringstream line;
    line << "op " << index << "  produces="
         << (named == names.end() ? std::string{"?"} : named->second.str())
         << " parent=" << node.parentIndex << " branch=" << node.branchOrdinal
         << " kind=" << xudu::opKindName(node.kind) << " flags=0x" << std::hex
         << static_cast<unsigned>(node.flags) << std::dec << " at=" << node.at
         << " len=" << node.length << " to=" << node.to
         << " src=" << node.sourceOpIndex << " srcAt=" << node.sourceAt
         << " srcLen=" << node.sourceLength << " scroll=" << node.scrollId
         << " span=[" << span.start << ',' << span.start + span.length << ')'
         << " link=" << node.linkId << " value=" << node.value;

    // What `flags` means, for the one kind it means anything for. The raw byte
    // stays above -- this is the decode beside it, so that a dump says
    // "setLink negward" rather than leaving a reader to know that bit 3 is the
    // direction. A Structure op's `to` and `link` are cell references and its
    // `src` is the previous operation on the same cell, which is what tells a
    // reader whose link it is.
    if (xudu::OpKind::Structure == node.kind) {
      const auto verb = xudu::structureVerbOf(node.flags);
      line << "  [" << xudu::structureVerbName(verb);
      if (xudu::StructureVerb::SetLink == verb) {
        line << (xudu::structureIsNegward(node.flags) ? " negward" : " posward")
             << " dim=" << node.linkId << " -> "
             << (0 == node.to ? std::string{"nothing"}
                              : std::to_string(node.to))
             << " cell@" << node.sourceOpIndex;
      } else if (xudu::ValueKind::None != xudu::valueKindOf(node.flags)) {
        line << ' ' << xudu::valueKindName(xudu::valueKindOf(node.flags));
      }
      line << ']';
    }

    // The text the span names, which is the whole point of rendering an
    // operation rather than hexdumping it: a change that shifted a field puts
    // garbage here, and a diff of two dumps says so on the line it happened.
    if (xudu::localScroll == node.scrollId && 0 != span.length &&
        span.start + span.length <= primedia.size()) {
      line << " text="
           << excerpt(std::string_view{primedia}.substr(
                  static_cast<std::size_t>(span.start),
                  static_cast<std::size_t>(span.length)));
    }
    std::cout << line.str() << '\n';
  }
}

// -- the side tables ---------------------------------------------------------
//
// Plaintext today and binary sections of one container after migration step
// 11. Parsed and re-rendered rather than echoed, so that the output is the
// same either side of that change and the diff says whether the meaning
// survived.

/// One segment's fields, in the order the plaintext table wrote them, plus
/// the two it silently dropped: whether the stretch is withheld, and what the
/// record of that says.
std::string segmentFields(const xudu::ScrollSegment &segment) {
  std::ostringstream out;
  out << "at=" << segment.at << " len=" << segment.length
      << " torrent=" << segment.torrent.hex()
      << " streamOffset=" << segment.streamOffset
      << " fileIndex=" << segment.fileIndex
      << " path=" << (segment.path.empty() ? "-" : segment.path)
      << " mime=" << segment.mimeType;
  if (segment.isWithheld()) {
    out << " withheld=" << static_cast<int>(segment.kind);
  }
  if (segment.holeRecord.has_value()) {
    out << " hole=[" << segment.holeRecord->at << ','
        << segment.holeRecord->end() << ')';
    if (segment.holeRecord->transcopyright.has_value()) {
      out << " transcopyright="
          << segment.holeRecord->transcopyright->priceAtomicUnits
          << segment.holeRecord->transcopyright->currencySymbol;
    }
  }
  return out.str();
}

/// The side tables, from the one container they live in. Rendered in the same
/// shape the plaintext files were rendered in, so that migration step 11's
/// conversion is a diff of this output rather than a claim about it.
void dumpTables(const xudu::StoreTables &tables, bool wantScrolls,
                bool wantLinks) {
  if (wantScrolls) {
    for (std::size_t i = 0; i < tables.scrolls.size(); i++) {
      const auto &scroll = tables.scrolls[i];
      const auto id      = i + 1;
      std::cout << "scroll " << id << "  publisher="
                << (scroll.isNamed() ? scroll.publisher.hex() : "-")
                << " salt=" << (scroll.salt.empty() ? "-" : scroll.salt)
                << " mime=" << scroll.defaultMimeType << '\n';
      for (const auto &segment : scroll.segments) {
        std::cout << "segment " << id << "  " << segmentFields(segment) << '\n';
      }
    }
    for (const auto &segment : tables.localSegments) {
      std::cout << "localsegment  " << segmentFields(segment) << '\n';
    }
  }
  if (wantLinks) {
    for (const auto &[id, link] : tables.links) {
      std::ostringstream out;
      out << "link " << id << "  type=" << xudu::linkTypeName(link.type)
          << " tier=" << xudu::prominenceTierName(link.tier)
          << " owner=" << (link.owner.empty() ? "-" : link.owner)
          << " curator=" << (link.curator.empty() ? "-" : link.curator);
      for (const auto &span : link.left) {
        out << " left=" << span.scroll << ':' << span.start << ','
            << span.start + span.length;
      }
      for (const auto &span : link.right) {
        out << " right=" << span.scroll << ':' << span.start << ','
            << span.start + span.length;
      }
      std::cout << out.str() << '\n';
    }
  }
}

/// The author-facing metadata, from the same container. Was two YAML files
/// echoed line by line; is now rendered from the tables, so that the move is a
/// diff of this output rather than a claim about it.
void dumpVersions(const xudu::StoreTables &tables) {
  for (const auto &id : tables.currentVersions) {
    std::cout << "current  " << id.str() << '\n';
  }
  for (const auto &[id, annotation] : tables.versionAnnotations) {
    std::cout << "version " << id.str() << "  alias="
              << (annotation.alias.empty() ? "-" : annotation.alias)
              << " tag=" << (annotation.tag.empty() ? "-" : annotation.tag)
              << " timestamp="
              << (annotation.timestamp.empty() ? "-" : annotation.timestamp)
              << " description=" << excerpt(annotation.description) << '\n';
  }
}

void usage() {
  std::cerr
      << "usage: xudu-dump [--section=SECTION] [--permascroll=PATH]\n"
         "                 <store-directory|ops-file>\n"
         "\n"
         "  Renders a xudu store as text without going through the loader,\n"
         "  so that a store the loader refuses can still be looked at.\n"
         "\n"
         "  SECTION is one of: all (default), header, ops, scrolls, links,\n"
         "  versions.\n"
         "\n"
         "  --section=ops is the one to diff across a format change: it\n"
         "  renders what each operation means, so a change that preserves\n"
         "  meaning produces identical output. --section=header is where a\n"
         "  version bump is supposed to show.\n"
         "\n"
         "  A store holds no primedia of its own -- what was typed lives in\n"
         "  the author's permascroll -- so --permascroll is what lets an\n"
         "  operation render the text its span names. PATH is the permascroll\n"
         "  directory or its active segment file. Without it every other\n"
         "  field still renders; only text= is missing.\n";
}

} // namespace

int main(int argc, char **argv) {
  std::string section = "all";
  std::filesystem::path permascroll;
  std::filesystem::path target;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg.starts_with("--section=")) {
      section = arg.substr(std::strlen("--section="));
    } else if (arg.starts_with("--permascroll=")) {
      permascroll = arg.substr(std::strlen("--permascroll="));
    } else if ("-h" == arg || "--help" == arg) {
      usage();
      return 0;
    } else if (arg.starts_with("-")) {
      usage();
      return 2;
    } else if (target.empty()) {
      target = arg;
    } else {
      usage();
      return 2;
    }
  }
  if (target.empty()) {
    usage();
    return 2;
  }

  const auto wants = [&section](const char *const name) {
    return "all" == section || section == name;
  };
  if (!wants("header") && !wants("ops") && !wants("scrolls") &&
      !wants("links") && !wants("versions")) {
    std::cerr << "xudu-dump: no such section \"" << section << "\"\n";
    usage();
    return 2;
  }

  // The author's permascroll, which is where the content an operation names
  // actually lives. A directory is taken as a permascroll's own storage
  // directory; a file is taken as its active segment, which is what
  // --dump-permascroll writes.
  std::string primedia;
  if (!permascroll.empty()) {
    const auto activeSegment = std::filesystem::is_directory(permascroll)
                                   ? permascroll / "active.primedia"
                                   : permascroll;
    primedia                 = readWhole(activeSegment);
    if (primedia.empty()) {
      trouble(activeSegment.string() +
              ": no primedia here, so operations render without the text "
              "their spans name");
    }
  }

  // A bare file is taken as an operations segment, which is the common case
  // when something has gone wrong with one in particular.
  if (!std::filesystem::is_directory(target)) {
    const auto file = readOpsFile(target);
    if (wants("header")) {
      dumpOpsHeader(file);
    }
    if (wants("ops")) {
      dumpOps(file, primedia);
    }
    return sawTrouble ? 1 : 0;
  }

  const auto exists = [&target](const char *const name) {
    return std::filesystem::exists(target / name);
  };

  // A store from before primedia stopped being siloed per document. Said
  // rather than ignored: its operations name addresses in *this* file, so
  // pointing --permascroll at it is what makes the dump mean anything.
  if (exists("primedia.spool") && permascroll.empty()) {
    trouble((target / "primedia.spool").string() +
            ": a store carrying its own copy of the permascroll. Pass "
            "--permascroll=" +
            (target / "primedia.spool").string() +
            " to render the text its operations name.");
  }

  if (exists("ops.nodes") && (wants("header") || wants("ops"))) {
    const auto file = readOpsFile(target / "ops.nodes");
    if (wants("header")) {
      dumpOpsHeader(file);
    }
    if (wants("ops")) {
      dumpOps(file, primedia);
    }
  } else if (exists("ops.spool") && wants("ops")) {
    trouble(
        (target / "ops.spool").string() +
        ": a store from before operations were kept as nodes. Open and save "
        "it with xudu to bring it forward; this renders nodes only.");
  }

  if (wants("scrolls") || wants("links") || wants("versions")) {
    if (exists("store.tables")) {
      try {
        const auto tables = xudu::readStoreTables(target / "store.tables");
        dumpTables(tables, wants("scrolls"), wants("links"));
        if (wants("versions")) {
          dumpVersions(tables);
        }
      } catch (const std::exception &e) {
        trouble(e.what());
      }
    } else if (wants("versions") &&
               (exists("current.yaml") || exists("versions.yaml"))) {
      trouble((target / "current.yaml").string() +
              ": author-facing metadata from before it moved into "
              "store.tables. Open and save the store with xudu to bring it "
              "forward.");
    }
  }

  return sawTrouble ? 1 : 0;
}
