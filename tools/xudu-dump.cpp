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

void dumpScrolls(const std::filesystem::path &path) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream fields(line);
    std::string kind;
    fields >> kind;
    if ("scroll" == kind) {
      std::uint32_t id{};
      std::string publisher;
      std::string salt;
      std::string mime;
      fields >> id >> publisher >> salt >> mime;
      std::cout << "scroll " << id << "  publisher=" << publisher
                << " salt=" << salt << " mime=" << mime << '\n';
    } else if ("segment" == kind) {
      std::uint32_t id{};
      std::uint64_t at{};
      std::uint64_t length{};
      std::string torrent;
      std::uint64_t streamOffset{};
      std::uint32_t fileIndex{};
      std::string filePath;
      std::string mime;
      fields >> id >> at >> length >> torrent >> streamOffset >> fileIndex >>
          filePath >> mime;
      std::cout << "segment " << id << "  at=" << at << " len=" << length
                << " torrent=" << torrent << " streamOffset=" << streamOffset
                << " fileIndex=" << fileIndex << " path=" << filePath
                << " mime=" << mime << '\n';
    } else if ("localsegment" == kind) {
      std::uint64_t at{};
      std::uint64_t length{};
      std::string mime;
      fields >> at >> length >> mime;
      std::cout << "localsegment  at=" << at << " len=" << length
                << " mime=" << mime << '\n';
    } else {
      trouble(path.string() + ": unrecognised line \"" + line + "\"");
    }
  }
}

void dumpLinks(const std::filesystem::path &path) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream fields(line);
    std::uint64_t id{};
    std::string type;
    std::string owner;
    std::size_t leftCount{};
    std::size_t rightCount{};
    fields >> id >> type >> owner >> leftCount >> rightCount;
    if (!fields) {
      trouble(path.string() + ": unrecognised line \"" + line + "\"");
      continue;
    }
    std::ostringstream out;
    out << "link " << id << "  type=" << type << " owner=" << owner;
    for (std::size_t end = 0; end < leftCount + rightCount; end++) {
      std::uint32_t scroll{};
      std::uint64_t start{};
      std::uint64_t length{};
      fields >> scroll >> start >> length;
      if (!fields) {
        trouble(path.string() + ": link " + std::to_string(id) +
                " names fewer spans than it claims");
        break;
      }
      out << (end < leftCount ? " left=" : " right=") << scroll << ':' << start
          << ',' << start + length;
    }
    std::cout << out.str() << '\n';
  }
}

void dumpText(const std::filesystem::path &path, const char *const label) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      std::cout << label << "  " << line << '\n';
    }
  }
}

void usage() {
  std::cerr
      << "usage: xudu-dump [--section=SECTION] <store-directory|ops-file>\n"
         "\n"
         "  Renders a xudu store as text without going through the loader,\n"
         "  so that a store the loader refuses can still be looked at.\n"
         "\n"
         "  SECTION is one of: all (default), header, ops, scrolls, links,\n"
         "  primedia, versions.\n"
         "\n"
         "  --section=ops is the one to diff across a format change: it\n"
         "  renders what each operation means, so a change that preserves\n"
         "  meaning produces identical output. --section=header is where a\n"
         "  version bump is supposed to show.\n";
}

} // namespace

int main(int argc, char **argv) {
  std::string section = "all";
  std::filesystem::path target;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg.starts_with("--section=")) {
      section = arg.substr(std::strlen("--section="));
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
      !wants("links") && !wants("primedia") && !wants("versions")) {
    std::cerr << "xudu-dump: no such section \"" << section << "\"\n";
    usage();
    return 2;
  }

  // A bare file is taken as an operations segment, which is the common case
  // when something has gone wrong with one in particular.
  if (!std::filesystem::is_directory(target)) {
    const auto file = readOpsFile(target);
    if (wants("header")) {
      dumpOpsHeader(file);
    }
    if (wants("ops")) {
      dumpOps(file, {});
    }
    return sawTrouble ? 1 : 0;
  }

  const auto exists = [&target](const char *const name) {
    return std::filesystem::exists(target / name);
  };

  std::string primedia;
  if (exists("primedia.spool")) {
    primedia = readWhole(target / "primedia.spool");
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

  if (wants("primedia")) {
    std::cout << "primedia  bytes=" << primedia.size();
    if (!primedia.empty()) {
      std::cout << " head=" << excerpt(primedia);
    }
    std::cout << '\n';
  }
  if (wants("scrolls") && exists("scrolls.spool")) {
    dumpScrolls(target / "scrolls.spool");
  }
  if (wants("scrolls") && exists("origins.spool")) {
    dumpText(target / "origins.spool", "origin");
  }
  if (wants("links") && exists("links.spool")) {
    dumpLinks(target / "links.spool");
  }
  if (wants("versions")) {
    if (exists("current.yaml")) {
      dumpText(target / "current.yaml", "current");
    }
    if (exists("versions.yaml")) {
      dumpText(target / "versions.yaml", "versions");
    }
  }

  return sawTrouble ? 1 : 0;
}
