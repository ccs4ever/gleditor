#include "ops.hpp"

#include <algorithm>
#include <cstdint>
#include <istream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace xudu {

namespace {

void putU8(std::string &out, const std::uint8_t value) {
  out.push_back(static_cast<char>(value));
}

void putU32(std::string &out, const std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    putU8(out, static_cast<std::uint8_t>(value >> shift));
  }
}

void putU64(std::string &out, const std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    putU8(out, static_cast<std::uint8_t>(value >> shift));
  }
}

void putMicroversionId(std::string &out, const MicroversionId &id) {
  const auto &segments = id.segments();
  putU8(out, static_cast<std::uint8_t>(segments.size()));
  for (const auto &segment : segments) {
    putU8(out, static_cast<std::uint8_t>(segment.branch));
    putU32(out, segment.number);
  }
}

/// Reads one byte, or reports there was none left. The only read in here that
/// may legitimately meet end of file: it is how decodeOpRecord() finds the
/// end of the log.
bool getByte(std::istream &in, std::uint8_t &out) {
  const auto ch = in.get();
  if (std::char_traits<char>::eof() == ch) {
    return false;
  }
  out = static_cast<std::uint8_t>(ch);
  return true;
}

std::uint8_t requireU8(std::istream &in) {
  std::uint8_t value{};
  if (!getByte(in, value)) {
    throw std::runtime_error(
        "operations spool: record ends in the middle of a field");
  }
  return value;
}

std::uint32_t requireU32(std::istream &in) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; i++) {
    value = (value << 8) | requireU8(in);
  }
  return value;
}

std::uint64_t requireU64(std::istream &in) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; i++) {
    value = (value << 8) | requireU8(in);
  }
  return value;
}

MicroversionId requireMicroversionId(std::istream &in) {
  const auto count = requireU8(in);
  std::vector<MicroversionId::Segment> segments;
  segments.reserve(count);
  for (std::uint8_t i = 0; i < count; i++) {
    MicroversionId::Segment segment;
    segment.branch = static_cast<char>(requireU8(in));
    segment.number = requireU32(in);
    segments.push_back(segment);
  }
  return MicroversionId{std::move(segments)};
}

} // namespace

const char *opKindName(const OpKind kind) {
  switch (kind) {
  case OpKind::Insert:
    return "insert";
  case OpKind::Delete:
    return "delete";
  case OpKind::Rearrange:
    return "rearrange";
  case OpKind::Transclude:
    return "transclude";
  case OpKind::Link:
    return "link";
  }
  return "insert";
}

const char *linkTypeName(const LinkType type) {
  switch (type) {
  case LinkType::Comment:
    return "comment";
  case LinkType::Illustration:
    return "illustration";
  case LinkType::Disagreement:
    return "disagreement";
  case LinkType::Authorship:
    return "authorship";
  case LinkType::Quotation:
    return "quotation";
  case LinkType::Other:
    return "other";
  }
  return "other";
}

LinkType linkTypeFromName(const std::string &name) {
  // Unrecognised types become Other rather than an error: a store written by
  // something that knows a type this build does not is still readable, and
  // losing the name of a link is better than losing the link.
  if ("comment" == name) {
    return LinkType::Comment;
  }
  if ("illustration" == name) {
    return LinkType::Illustration;
  }
  if ("disagreement" == name) {
    return LinkType::Disagreement;
  }
  if ("authorship" == name) {
    return LinkType::Authorship;
  }
  if ("quotation" == name) {
    return LinkType::Quotation;
  }
  return LinkType::Other;
}

bool Link::touches(const PrimediaSpan &span) const {
  const auto meets = [&span](const PrimediaSpan &end) {
    return !end.intersect(span).empty();
  };
  return std::ranges::any_of(left, meets) || std::ranges::any_of(right, meets);
}

std::string encodeOpRecord(const MicroversionId &produces, const Op &op) {
  std::string out;
  putMicroversionId(out, produces);
  putU8(out, static_cast<std::uint8_t>(op.kind));
  putU32(out, op.at);
  putU32(out, op.length);
  putU32(out, op.to);
  putU64(out, op.span.start);
  putU64(out, op.span.length);
  putU32(out, op.span.scroll);
  putMicroversionId(out, op.source);
  putU32(out, op.sourceAt);
  putU32(out, op.sourceLength);
  putU64(out, op.link);
  return out;
}

bool decodeOpRecord(std::istream &in, MicroversionId &produces, Op &op) {
  std::uint8_t segmentCount{};
  if (!getByte(in, segmentCount)) {
    // Nothing left, and nothing read yet -- the clean end of the log rather
    // than a record cut short.
    return false;
  }
  std::vector<MicroversionId::Segment> segments;
  segments.reserve(segmentCount);
  for (std::uint8_t i = 0; i < segmentCount; i++) {
    MicroversionId::Segment segment;
    segment.branch = static_cast<char>(requireU8(in));
    segment.number = requireU32(in);
    segments.push_back(segment);
  }
  produces = MicroversionId{std::move(segments)};

  const auto kind = requireU8(in);
  if (kind > static_cast<std::uint8_t>(OpKind::Link)) {
    throw std::runtime_error(
        "operations spool: record names an operation kind this build does "
        "not know");
  }
  op.kind         = static_cast<OpKind>(kind);
  op.at           = requireU32(in);
  op.length       = requireU32(in);
  op.to           = requireU32(in);
  op.span.start   = requireU64(in);
  op.span.length  = requireU64(in);
  op.span.scroll  = requireU32(in);
  op.source       = requireMicroversionId(in);
  op.sourceAt     = requireU32(in);
  op.sourceLength = requireU32(in);
  op.link         = requireU64(in);
  return true;
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:
