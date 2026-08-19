#include "binary_ops.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace xudu {

namespace {

enum OpBinaryKind : std::uint8_t {
  BinInsert            = 0,
  BinDelete            = 1,
  BinRearrange         = 2,
  BinTranscludeInternal = 3,
  BinTranscludeExternal = 4,
  BinLink              = 5,
};

constexpr std::uint8_t FLAG_KIND_MASK         = 0x07;
constexpr std::uint8_t FLAG_SEQUENTIAL        = 0x08;
constexpr std::uint8_t FLAG_LOCAL_SCROLL      = 0x10;
constexpr std::uint8_t FLAG_AT_EQUALS_START   = 0x20;
constexpr std::uint8_t FLAG_SINGLE_BYTE       = 0x40;

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
  val = 0;
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
    out.put(seg.branch);
    writeVarint(out, seg.number);
  }
}

bool readMicroversionId(std::istream &in, MicroversionId &id) {
  std::uint64_t count = 0;
  if (!readVarint(in, count)) {
    return false;
  }
  std::vector<MicroversionId::Segment> segs;
  segs.reserve(count);
  for (std::uint64_t i = 0; i < count; i++) {
    const int c = in.get();
    if (c == std::char_traits<char>::eof()) {
      return false;
    }
    std::uint64_t num = 0;
    if (!readVarint(in, num)) {
      return false;
    }
    segs.push_back(MicroversionId::Segment{
        static_cast<char>(c), static_cast<std::uint32_t>(num)});
  }
  id = MicroversionId(std::move(segs));
  return true;
}

void writeBinaryOpsSpool(std::ostream &out,
                         const std::map<MicroversionId, Op> &ops) {
  out.write(binaryOpsMagic.data(), static_cast<std::streamsize>(binaryOpsMagic.size()));

  MicroversionId lastProduces{};
  for (const auto &[produces, op] : ops) {
    std::uint8_t tag = 0;
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
    }

    lastProduces = produces;
  }
}

void readBinaryOpsSpool(std::istream &in,
                        std::map<MicroversionId, Op> &ops) {
  MicroversionId lastProduces{};
  while (true) {
    const int c = in.get();
    if (c == std::char_traits<char>::eof()) {
      break;
    }
    const auto tag = static_cast<std::uint8_t>(c);
    const bool isSequential = (tag & FLAG_SEQUENTIAL) != 0;
    const auto kindCode = static_cast<OpBinaryKind>(tag & FLAG_KIND_MASK);

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
      if (!readVarint(in, v1) || !readVarint(in, v2) ||
          !readVarint(in, v3) || !readVarint(in, v4)) {
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

    default:
      throw std::runtime_error("unknown binary op kind tag: " + std::to_string(tag));
    }

    ops.emplace(produces, op);
    lastProduces = produces;
  }
}

void writeOsmicTextOpsSpool(std::ostream &out,
                            const std::map<MicroversionId, Op> &ops) {
  for (const auto &[id, op] : ops) {
    out << id.str() << ' ' << opKindName(op.kind) << ' ' << op.at << ' '
        << op.length << ' ' << op.to << ' ' << op.span.start << ' '
        << op.span.length << ' '
        << (op.source.isZero() ? "0" : op.source.str()) << ' ' << op.sourceAt
        << ' ' << op.sourceLength << ' ' << op.link << ' ' << op.span.scroll
        << '\n';
  }
}

void readOsmicTextOpsSpool(std::istream &in,
                           std::map<MicroversionId, Op> &ops) {
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
    } else {
      throw std::runtime_error("unknown operation \"" + kind + "\"");
    }
    ops.emplace(produces, op);
  }
}

void readOpsSpool(std::istream &in, std::map<MicroversionId, Op> &ops) {
  std::string header;
  header.resize(binaryOpsMagic.size());
  in.read(header.data(), static_cast<std::streamsize>(binaryOpsMagic.size()));
  if (in.gcount() == static_cast<std::streamsize>(binaryOpsMagic.size()) &&
      header == binaryOpsMagic) {
    readBinaryOpsSpool(in, ops);
  } else {
    in.clear();
    in.seekg(0, std::ios::beg);
    readOsmicTextOpsSpool(in, ops);
  }
}

} // namespace xudu
