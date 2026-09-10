#include "binary_ops.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace xanadu {

namespace {

enum OpBinaryKind : std::uint8_t {
  BinInsert             = 0,
  BinDelete             = 1,
  BinRearrange          = 2,
  BinTranscludeInternal = 3,
  BinTranscludeExternal = 4,
  BinLink               = 5,
  BinPageBreak          = 6,
};

/// The operation tag byte, version 3: four bits of kind and four flags above
/// it, which is the whole byte. Version 2 gave the kind three bits and left
/// bit 7 spare; OpKind::Structure would have filled that field exactly, so
/// the spare bit went to the kind instead of to a ninth flag. See
/// OpsSpoolVersion::CompactBinaryV3.
constexpr std::uint8_t FLAG_KIND_MASK       = 0x0F;
constexpr std::uint8_t FLAG_SEQUENTIAL      = 0x10;
constexpr std::uint8_t FLAG_LOCAL_SCROLL    = 0x20;
constexpr std::uint8_t FLAG_AT_EQUALS_START = 0x40;
constexpr std::uint8_t FLAG_SINGLE_BYTE     = 0x80;
static_assert((FLAG_KIND_MASK | FLAG_SEQUENTIAL | FLAG_LOCAL_SCROLL |
               FLAG_AT_EQUALS_START | FLAG_SINGLE_BYTE) == 0xFF,
              "the tag byte is fully spoken for; a further flag needs a "
              "version bump or a second byte");

/// The branch byte: 0-254 name that ordinal directly, and 255 means "the real
/// ordinal is a varint that follows". 254 direct values covers every branch
/// count a document has ever needed here by a wide margin; the escape exists
/// for whatever documents this has not seen yet, at the cost of one byte only
/// when it is actually used.
constexpr std::uint32_t branchOrdinalEscape = 255;

} // namespace

void writeVarint(std::ostream &out, std::uint64_t val) {
  do {
    auto byte = static_cast<std::uint8_t>(val & 0x7FU);
    val >>= 7U;
    if (val != 0) {
      byte |= 0x80U;
    }
    out.put(static_cast<char>(byte));
  } while (val != 0);
}

bool readVarint(std::istream &in, std::uint64_t &val) {
  val       = 0;
  int shift = 0;
  while (true) {
    const int c = in.get();
    if (c == std::char_traits<char>::eof()) {
      return false;
    }
    const auto byte = static_cast<std::uint8_t>(c);
    val |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
    if ((byte & 0x80U) == 0) {
      return true;
    }
    shift += 7;
    if (shift > 63) {
      return false;
    }
  }
}

void writeMicroversionId(std::ostream &out, const MicroversionId &id) {
  const auto &segs = id.segments();
  writeVarint(out, segs.size());
  for (const auto &seg : segs) {
    if (seg.branch < branchOrdinalEscape) {
      out.put(static_cast<char>(seg.branch));
    } else {
      out.put(static_cast<char>(branchOrdinalEscape));
      writeVarint(out, seg.branch);
    }
    writeVarint(out, seg.number);
  }
}

bool readMicroversionId(std::istream &in, MicroversionId &id) {
  std::uint64_t count = 0;
  if (!readVarint(in, count)) {
    return false;
  }
  if (count > 4096) {
    return false;
  }
  std::vector<MicroversionId::Segment> segs;
  segs.reserve(std::min<std::size_t>(static_cast<std::size_t>(count), 64));
  for (std::uint64_t i = 0; i < count; i++) {
    const int c = in.get();
    if (c == std::char_traits<char>::eof()) {
      return false;
    }
    auto branch = static_cast<std::uint32_t>(static_cast<std::uint8_t>(c));
    if (branchOrdinalEscape == branch) {
      std::uint64_t extended = 0;
      if (!readVarint(in, extended)) {
        return false;
      }
      branch = static_cast<std::uint32_t>(extended);
    }
    std::uint64_t num = 0;
    if (!readVarint(in, num)) {
      return false;
    }
    segs.push_back(
        MicroversionId::Segment{branch, static_cast<std::uint32_t>(num)});
  }
  id = MicroversionId(segs);
  return true;
}

void writeBinaryOpsSpool(std::ostream &out, const std::vector<OpRecord> &ops) {
  out.write(binaryOpsMagic.data(),
            static_cast<std::streamsize>(binaryOpsMagic.size()));

  MicroversionId lastProduces{};
  for (const auto &[produces, op] : ops) {
    std::uint8_t tag        = 0;
    const bool isSequential = (produces == lastProduces.next());
    if (isSequential) {
      tag |= FLAG_SEQUENTIAL;
    }

    switch (op.kind) {
    case OpKind::Insert:
      tag |= BinInsert;
      if (op.span.scroll == localScroll) {
        tag |= FLAG_LOCAL_SCROLL;
      }
      if (op.at == op.span.start) {
        tag |= FLAG_AT_EQUALS_START;
      }
      if (op.span.length == 1) {
        tag |= FLAG_SINGLE_BYTE;
      }
      out.put(static_cast<char>(tag));
      if (!isSequential) {
        writeMicroversionId(out, produces);
      }
      writeVarint(out, op.at);
      if (!(tag & FLAG_AT_EQUALS_START)) {
        writeVarint(out, op.span.start);
      }
      if (!(tag & FLAG_SINGLE_BYTE)) {
        writeVarint(out, op.span.length);
      }
      if (!(tag & FLAG_LOCAL_SCROLL)) {
        writeVarint(out, op.span.scroll);
      }
      break;

    case OpKind::Delete:
      tag |= BinDelete;
      if (op.length == 1) {
        tag |= FLAG_SINGLE_BYTE;
      }
      out.put(static_cast<char>(tag));
      if (!isSequential) {
        writeMicroversionId(out, produces);
      }
      writeVarint(out, op.at);
      if (!(tag & FLAG_SINGLE_BYTE)) {
        writeVarint(out, op.length);
      }
      break;

    case OpKind::Rearrange:
      tag |= BinRearrange;
      out.put(static_cast<char>(tag));
      if (!isSequential) {
        writeMicroversionId(out, produces);
      }
      writeVarint(out, op.at);
      writeVarint(out, op.length);
      writeVarint(out, op.to);
      break;

    case OpKind::Transclude:
      if (op.source.isZero()) {
        tag |= BinTranscludeExternal;
        out.put(static_cast<char>(tag));
        if (!isSequential) {
          writeMicroversionId(out, produces);
        }
        writeVarint(out, op.at);
        writeVarint(out, op.span.scroll);
        writeVarint(out, op.span.start);
        writeVarint(out, op.span.length);
      } else {
        tag |= BinTranscludeInternal;
        out.put(static_cast<char>(tag));
        if (!isSequential) {
          writeMicroversionId(out, produces);
        }
        writeVarint(out, op.at);
        writeMicroversionId(out, op.source);
        writeVarint(out, op.sourceAt);
        writeVarint(out, op.sourceLength);
      }
      break;

    case OpKind::Link:
      tag |= BinLink;
      out.put(static_cast<char>(tag));
      if (!isSequential) {
        writeMicroversionId(out, produces);
      }
      writeVarint(out, op.link);
      break;

    case OpKind::PageBreak:
      tag |= BinPageBreak;
      out.put(static_cast<char>(tag));
      if (!isSequential) {
        writeMicroversionId(out, produces);
      }
      writeVarint(out, op.at);
      break;
    }

    lastProduces = produces;
  }
}

void readBinaryOpsSpool(std::istream &in, std::vector<OpRecord> &ops) {
  MicroversionId lastProduces{};
  while (true) {
    const int c = in.get();
    if (c == std::char_traits<char>::eof()) {
      break;
    }
    const auto tag          = static_cast<std::uint8_t>(c);
    const bool isSequential = (tag & FLAG_SEQUENTIAL) != 0;
    const auto kindCode     = static_cast<OpBinaryKind>(tag & FLAG_KIND_MASK);

    MicroversionId produces;
    if (isSequential) {
      produces = lastProduces.next();
    } else {
      if (!readMicroversionId(in, produces)) {
        throw std::runtime_error("malformed binary op: truncated microversion");
      }
    }

    Op op;
    op.parent = produces.parent();

    std::uint64_t v1 = 0, v2 = 0, v3 = 0, v4 = 0;

    switch (kindCode) {
    case BinInsert:
      op.kind = OpKind::Insert;
      if (!readVarint(in, v1)) {
        throw std::runtime_error("malformed binary insert op at");
      }
      op.at = static_cast<std::uint32_t>(v1);

      if (tag & FLAG_AT_EQUALS_START) {
        op.span.start = op.at;
      } else {
        if (!readVarint(in, v2)) {
          throw std::runtime_error("malformed binary insert op start");
        }
        op.span.start = v2;
      }

      if (tag & FLAG_SINGLE_BYTE) {
        op.span.length = 1;
      } else {
        if (!readVarint(in, v3)) {
          throw std::runtime_error("malformed binary insert op length");
        }
        op.span.length = v3;
      }

      if (tag & FLAG_LOCAL_SCROLL) {
        op.span.scroll = localScroll;
      } else {
        if (!readVarint(in, v4)) {
          throw std::runtime_error("malformed binary insert op scroll");
        }
        op.span.scroll = static_cast<ScrollId>(v4);
      }
      break;

    case BinDelete:
      op.kind = OpKind::Delete;
      if (!readVarint(in, v1)) {
        throw std::runtime_error("malformed binary delete op at");
      }
      op.at = static_cast<std::uint32_t>(v1);
      if (tag & FLAG_SINGLE_BYTE) {
        op.length = 1;
      } else {
        if (!readVarint(in, v2)) {
          throw std::runtime_error("malformed binary delete op length");
        }
        op.length = static_cast<std::uint32_t>(v2);
      }
      break;

    case BinRearrange:
      op.kind = OpKind::Rearrange;
      if (!readVarint(in, v1) || !readVarint(in, v2) || !readVarint(in, v3)) {
        throw std::runtime_error("malformed binary rearrange op");
      }
      op.at     = static_cast<std::uint32_t>(v1);
      op.length = static_cast<std::uint32_t>(v2);
      op.to     = static_cast<std::uint32_t>(v3);
      break;

    case BinTranscludeInternal:
      op.kind = OpKind::Transclude;
      if (!readVarint(in, v1)) {
        throw std::runtime_error("malformed binary transclude op");
      }
      op.at = static_cast<std::uint32_t>(v1);
      if (!readMicroversionId(in, op.source)) {
        throw std::runtime_error("malformed binary transclude source");
      }
      if (!readVarint(in, v2) || !readVarint(in, v3)) {
        throw std::runtime_error("malformed binary transclude offsets");
      }
      op.sourceAt     = static_cast<std::uint32_t>(v2);
      op.sourceLength = static_cast<std::uint32_t>(v3);
      break;

    case BinTranscludeExternal:
      op.kind = OpKind::Transclude;
      if (!readVarint(in, v1) || !readVarint(in, v2) || !readVarint(in, v3) ||
          !readVarint(in, v4)) {
        throw std::runtime_error("malformed binary external transclude op");
      }
      op.at          = static_cast<std::uint32_t>(v1);
      op.span.scroll = static_cast<ScrollId>(v2);
      op.span.start  = v3;
      op.span.length = v4;
      break;

    case BinLink:
      op.kind = OpKind::Link;
      if (!readVarint(in, v1)) {
        throw std::runtime_error("malformed binary link op");
      }
      op.link = v1;
      break;

    case BinPageBreak:
      op.kind = OpKind::PageBreak;
      if (!readVarint(in, v1)) {
        throw std::runtime_error("malformed binary pagebreak op");
      }
      op.at = static_cast<std::uint32_t>(v1);
      break;

    default:
      throw std::runtime_error("unknown binary op kind tag: " +
                               std::to_string(tag));
    }

    ops.push_back(OpRecord{produces, op});
    lastProduces = produces;
  }
}

void writeOsmicTextOpsSpool(std::ostream &out,
                            const std::vector<OpRecord> &ops) {
  for (const auto &[id, op] : ops) {
    out << id.str() << ' ' << opKindName(op.kind) << ' ' << op.at << ' '
        << op.length << ' ' << op.to << ' ' << op.span.start << ' '
        << op.span.length << ' ' << (op.source.isZero() ? "0" : op.source.str())
        << ' ' << op.sourceAt << ' ' << op.sourceLength << ' ' << op.link << ' '
        << op.span.scroll << '\n';
  }
}

void readOsmicTextOpsSpool(std::istream &in, std::vector<OpRecord> &ops) {
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream fields(line);
    std::string id;
    std::string kind;
    std::string source;
    Op op;
    fields >> id >> kind >> op.at >> op.length >> op.to >> op.span.start >>
        op.span.length >> source >> op.sourceAt >> op.sourceLength >> op.link;
    if (!(fields >> op.span.scroll)) {
      op.span.scroll = localScroll;
    }
    if (!fields) {
      throw std::runtime_error("malformed operation: " + line);
    }
    const auto produces = MicroversionId::parse(id);
    op.source           = MicroversionId::parse(source);
    op.parent           = produces.parent();
    if ("insert" == kind) {
      op.kind = OpKind::Insert;
    } else if ("delete" == kind) {
      op.kind = OpKind::Delete;
    } else if ("rearrange" == kind) {
      op.kind = OpKind::Rearrange;
    } else if ("transclude" == kind) {
      op.kind = OpKind::Transclude;
    } else if ("link" == kind) {
      op.kind = OpKind::Link;
    } else if ("pagebreak" == kind) {
      op.kind = OpKind::PageBreak;
    } else {
      throw std::runtime_error("unknown operation \"" + kind + "\"");
    }
    ops.push_back(OpRecord{produces, op});
  }
}

const char *opsSpoolVersionName(const OpsSpoolVersion version) {
  switch (version) {
  case OpsSpoolVersion::StandardOsmicText:
    return "OSMIC text (v0)";
  case OpsSpoolVersion::CompactBinaryV3:
    return "Compact binary (v3)";
  }
  return "unknown";
}

OpsSpoolVersion detectOpsSpoolVersion(std::istream &in) {
  std::string prefix(binaryOpsMagicPrefix.size(), '\0');
  in.read(prefix.data(),
          static_cast<std::streamsize>(binaryOpsMagicPrefix.size()));
  if (in.gcount() ==
          static_cast<std::streamsize>(binaryOpsMagicPrefix.size()) &&
      prefix == binaryOpsMagicPrefix) {
    const int ver = in.get();
    if (ver == std::char_traits<char>::eof()) {
      throw std::runtime_error(
          "truncated binary ops spool header: missing version");
    }
    if (ver == static_cast<int>(OpsSpoolVersion::CompactBinaryV3)) {
      return OpsSpoolVersion::CompactBinaryV3;
    }
    // Both numbers, so that a version 1 or 2 file -- which this build
    // deliberately no longer reads, see OpsSpoolVersion -- says what it is
    // rather than only that it is not wanted.
    throw std::runtime_error(
        "binary ops spool is version " + std::to_string(ver) +
        " and this build reads version " +
        std::to_string(static_cast<int>(OpsSpoolVersion::CompactBinaryV3)));
  }
  in.clear();
  in.seekg(0, std::ios::beg);
  return OpsSpoolVersion::StandardOsmicText;
}

void readOpsSpool(std::istream &in, std::vector<OpRecord> &ops) {
  const auto version = detectOpsSpoolVersion(in);
  switch (version) {
  case OpsSpoolVersion::CompactBinaryV3:
    readBinaryOpsSpool(in, ops);
    break;
  case OpsSpoolVersion::StandardOsmicText:
    readOsmicTextOpsSpool(in, ops);
    break;
  }
}

} // namespace xanadu
