/**
 * @file zz9p.cpp
 * @brief Implementation of the 9Z2026 wire protocol codec.
 */
#include "common/xanadu/zigzag/zz9p.hpp"

#include <cstring>
#include <sstream>

namespace zigzag::zz9p {

namespace {

class ByteWriter {
public:
  void reserve(const std::size_t n) { buf_.reserve(n); }
  [[nodiscard]] std::vector<std::uint8_t> release() { return std::move(buf_); }

  template <typename T> void write(const T &val) {
    const auto *src = reinterpret_cast<const std::uint8_t *>(&val);
    buf_.insert(buf_.end(), src, src + sizeof(T));
  }

  void writeString(const std::string_view s) {
    const auto len = static_cast<std::uint16_t>(s.size());
    write(len);
    const auto *src = reinterpret_cast<const std::uint8_t *>(s.data());
    buf_.insert(buf_.end(), src, src + len);
  }

  void writeBytes(const std::span<const std::uint8_t> bytes) {
    const auto len = static_cast<std::uint32_t>(bytes.size());
    write(len);
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
  }

  void setHeader(const MsgType type, const std::uint16_t tag) {
    Header h{
        .size = static_cast<std::uint32_t>(buf_.size()),
        .type = static_cast<std::uint8_t>(type),
        .tag  = tag,
    };
    std::memcpy(buf_.data(), &h, sizeof(Header));
  }

private:
  std::vector<std::uint8_t> buf_;
};

class ByteReader {
public:
  explicit ByteReader(const std::span<const std::uint8_t> bytes)
      : data_(bytes.data()), size_(bytes.size()) {}

  [[nodiscard]] bool hasRemaining(const std::size_t n) const noexcept {
    return pos_ + n <= size_;
  }

  template <typename T> [[nodiscard]] std::optional<T> read() noexcept {
    if (!hasRemaining(sizeof(T))) {
      return std::nullopt;
    }
    T val{};
    std::memcpy(&val, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return val;
  }

  [[nodiscard]] std::optional<std::string> readString() {
    const auto len = read<std::uint16_t>();
    if (!len || !hasRemaining(*len)) {
      return std::nullopt;
    }
    std::string s(reinterpret_cast<const char *>(data_ + pos_), *len);
    pos_ += *len;
    return s;
  }

  [[nodiscard]] std::optional<std::vector<std::uint8_t>> readBytes() {
    const auto len = read<std::uint32_t>();
    if (!len || !hasRemaining(*len)) {
      return std::nullopt;
    }
    std::vector<std::uint8_t> v(data_ + pos_, data_ + pos_ + *len);
    pos_ += *len;
    return v;
  }

private:
  const std::uint8_t *data_{nullptr};
  std::size_t size_{0};
  std::size_t pos_{0};
};

} // namespace

std::vector<std::uint8_t> Codec::encode(const Message &msg) {
  ByteWriter w;
  w.reserve(128);

  // Reserve space for 7-byte header
  const Header placeholder{};
  w.write(placeholder);

  switch (msg.type) {
  case MsgType::Tversion: {
    const auto &p = std::get<MsgTversion>(msg.payload);
    w.write(p.msize);
    w.writeString(p.version);
    break;
  }
  case MsgType::Rversion: {
    const auto &p = std::get<MsgRversion>(msg.payload);
    w.write(p.msize);
    w.writeString(p.version);
    break;
  }
  case MsgType::Tauth: {
    const auto &p = std::get<MsgTauth>(msg.payload);
    w.write(p.afid);
    w.writeString(p.uname);
    w.writeString(p.aname);
    break;
  }
  case MsgType::Rauth: {
    const auto &p = std::get<MsgRauth>(msg.payload);
    w.write(p.aqid);
    break;
  }
  case MsgType::Tattach: {
    const auto &p = std::get<MsgTattach>(msg.payload);
    w.write(p.fid);
    w.write(p.afid);
    w.writeString(p.uname);
    w.writeString(p.aname);
    break;
  }
  case MsgType::Rattach: {
    const auto &p = std::get<MsgRattach>(msg.payload);
    w.write(p.qid);
    w.write(p.rootCell);
    break;
  }
  case MsgType::Tclunk: {
    const auto &p = std::get<MsgTclunk>(msg.payload);
    w.write(p.fid);
    break;
  }
  case MsgType::Rclunk:
    break;
  case MsgType::Tflush: {
    const auto &p = std::get<MsgTflush>(msg.payload);
    w.write(p.oldtag);
    break;
  }
  case MsgType::Rflush:
    break;
  case MsgType::Zstat: {
    const auto &p = std::get<MsgZstat>(msg.payload);
    w.write(p.cell);
    break;
  }
  case MsgType::Rstat: {
    const auto &p = std::get<MsgRstat>(msg.payload);
    w.write(p.cell);
    w.write(p.flags);
    w.write(p.spanLength);
    w.write(p.linkCount);
    break;
  }
  case MsgType::Zhop: {
    const auto &p = std::get<MsgZhop>(msg.payload);
    w.write(p.startCell);
    w.write(p.dim);
    w.write(static_cast<std::uint8_t>(p.dir));
    w.write(p.hops);
    break;
  }
  case MsgType::Rhop: {
    const auto &p = std::get<MsgRhop>(msg.payload);
    w.write(p.targetCell);
    break;
  }
  case MsgType::Zread: {
    const auto &p = std::get<MsgZread>(msg.payload);
    w.write(p.cell);
    w.write(p.offset);
    w.write(p.count);
    break;
  }
  case MsgType::Rread: {
    const auto &p = std::get<MsgRread>(msg.payload);
    w.writeBytes(p.data);
    break;
  }
  case MsgType::Zdims: {
    const auto &p = std::get<MsgZdims>(msg.payload);
    w.write(p.cell);
    break;
  }
  case MsgType::Rdims: {
    const auto &p    = std::get<MsgRdims>(msg.payload);
    const auto count = static_cast<std::uint32_t>(p.dimensions.size());
    w.write(count);
    for (const auto d : p.dimensions) {
      w.write(d);
    }
    break;
  }
  case MsgType::Zlink: {
    const auto &p = std::get<MsgZlink>(msg.payload);
    w.write(p.fromCell);
    w.write(p.dim);
    w.write(static_cast<std::uint8_t>(p.dir));
    w.write(p.toCell);
    break;
  }
  case MsgType::Rlink: {
    const auto &p = std::get<MsgRlink>(msg.payload);
    w.write(static_cast<std::uint8_t>(p.success ? 1 : 0));
    break;
  }
  case MsgType::Zmake: {
    const auto &p = std::get<MsgZmake>(msg.payload);
    w.write(p.suggestedId);
    w.write(p.valueKind);
    w.write(p.valueBits);
    break;
  }
  case MsgType::Rmake: {
    const auto &p = std::get<MsgRmake>(msg.payload);
    w.write(p.newCell);
    break;
  }
  case MsgType::Zwatch: {
    const auto &p = std::get<MsgZwatch>(msg.payload);
    w.write(p.cell);
    w.write(p.dim);
    w.write(static_cast<std::uint8_t>(p.dir));
    w.write(p.mask);
    break;
  }
  case MsgType::Rwatch: {
    const auto &p = std::get<MsgRwatch>(msg.payload);
    w.write(p.watchId);
    break;
  }
  case MsgType::Rerror: {
    const auto &p = std::get<MsgRerror>(msg.payload);
    w.writeString(p.ename);
    break;
  }
  }

  w.setHeader(msg.type, msg.tag);
  return w.release();
}

std::expected<Message, std::string>
Codec::decode(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < sizeof(Header)) {
    return std::unexpected("Payload smaller than 9Z header");
  }

  Header h{};
  std::memcpy(&h, bytes.data(), sizeof(Header));

  if (h.size > kMaxPayloadBytes) {
    return std::unexpected("9Z frame exceeds kMaxPayloadBytes limit");
  }
  if (bytes.size() < h.size) {
    return std::unexpected("Incomplete 9Z frame");
  }

  ByteReader r(bytes.subspan(sizeof(Header), h.size - sizeof(Header)));
  Message msg{.type = static_cast<MsgType>(h.type), .tag = h.tag};

  switch (msg.type) {
  case MsgType::Tversion: {
    auto msize = r.read<std::uint32_t>();
    auto ver   = r.readString();
    if (!msize || !ver) return std::unexpected("Malformed Tversion");
    msg.payload = MsgTversion{.msize = *msize, .version = std::move(*ver)};
    break;
  }
  case MsgType::Rversion: {
    auto msize = r.read<std::uint32_t>();
    auto ver   = r.readString();
    if (!msize || !ver) return std::unexpected("Malformed Rversion");
    msg.payload = MsgRversion{.msize = *msize, .version = std::move(*ver)};
    break;
  }
  case MsgType::Tauth: {
    auto afid  = r.read<std::uint32_t>();
    auto uname = r.readString();
    auto aname = r.readString();
    if (!afid || !uname || !aname) return std::unexpected("Malformed Tauth");
    msg.payload = MsgTauth{
        .afid = *afid, .uname = std::move(*uname), .aname = std::move(*aname)};
    break;
  }
  case MsgType::Rauth: {
    auto aqid = r.read<std::uint32_t>();
    if (!aqid) return std::unexpected("Malformed Rauth");
    msg.payload = MsgRauth{.aqid = *aqid};
    break;
  }
  case MsgType::Tattach: {
    auto fid   = r.read<std::uint32_t>();
    auto afid  = r.read<std::uint32_t>();
    auto uname = r.readString();
    auto aname = r.readString();
    if (!fid || !afid || !uname || !aname)
      return std::unexpected("Malformed Tattach");
    msg.payload = MsgTattach{.fid   = *fid,
                             .afid  = *afid,
                             .uname = std::move(*uname),
                             .aname = std::move(*aname)};
    break;
  }
  case MsgType::Rattach: {
    auto qid      = r.read<std::uint32_t>();
    auto rootCell = r.read<CellRef>();
    if (!qid || !rootCell) return std::unexpected("Malformed Rattach");
    msg.payload = MsgRattach{.qid = *qid, .rootCell = *rootCell};
    break;
  }
  case MsgType::Tclunk: {
    auto fid = r.read<std::uint32_t>();
    if (!fid) return std::unexpected("Malformed Tclunk");
    msg.payload = MsgTclunk{.fid = *fid};
    break;
  }
  case MsgType::Rclunk:
    msg.payload = MsgRclunk{};
    break;
  case MsgType::Tflush: {
    auto oldtag = r.read<std::uint16_t>();
    if (!oldtag) return std::unexpected("Malformed Tflush");
    msg.payload = MsgTflush{.oldtag = *oldtag};
    break;
  }
  case MsgType::Rflush:
    msg.payload = MsgRflush{};
    break;
  case MsgType::Zstat: {
    auto cell = r.read<CellRef>();
    if (!cell) return std::unexpected("Malformed Zstat");
    msg.payload = MsgZstat{.cell = *cell};
    break;
  }
  case MsgType::Rstat: {
    auto cell       = r.read<CellRef>();
    auto flags      = r.read<std::uint32_t>();
    auto spanLength = r.read<std::uint64_t>();
    auto linkCount  = r.read<std::uint32_t>();
    if (!cell || !flags || !spanLength || !linkCount)
      return std::unexpected("Malformed Rstat");
    msg.payload = MsgRstat{.cell       = *cell,
                           .flags      = *flags,
                           .spanLength = *spanLength,
                           .linkCount  = *linkCount};
    break;
  }
  case MsgType::Zhop: {
    auto startCell = r.read<CellRef>();
    auto dim       = r.read<DimRef>();
    auto dir       = r.read<std::uint8_t>();
    auto hops      = r.read<std::uint32_t>();
    if (!startCell || !dim || !dir || !hops)
      return std::unexpected("Malformed Zhop");
    msg.payload = MsgZhop{.startCell = *startCell,
                          .dim       = *dim,
                          .dir       = static_cast<DimVector>(*dir),
                          .hops      = *hops};
    break;
  }
  case MsgType::Rhop: {
    auto targetCell = r.read<CellRef>();
    if (!targetCell) return std::unexpected("Malformed Rhop");
    msg.payload = MsgRhop{.targetCell = *targetCell};
    break;
  }
  case MsgType::Zread: {
    auto cell   = r.read<CellRef>();
    auto offset = r.read<std::uint64_t>();
    auto count  = r.read<std::uint32_t>();
    if (!cell || !offset || !count) return std::unexpected("Malformed Zread");
    msg.payload = MsgZread{.cell = *cell, .offset = *offset, .count = *count};
    break;
  }
  case MsgType::Rread: {
    auto data = r.readBytes();
    if (!data) return std::unexpected("Malformed Rread");
    msg.payload = MsgRread{.data = std::move(*data)};
    break;
  }
  case MsgType::Zdims: {
    auto cell = r.read<CellRef>();
    if (!cell) return std::unexpected("Malformed Zdims");
    msg.payload = MsgZdims{.cell = *cell};
    break;
  }
  case MsgType::Rdims: {
    auto count = r.read<std::uint32_t>();
    if (!count) return std::unexpected("Malformed Rdims count");
    std::vector<DimRef> dims;
    dims.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
      auto d = r.read<DimRef>();
      if (!d) return std::unexpected("Truncated Rdims list");
      dims.push_back(*d);
    }
    msg.payload = MsgRdims{.dimensions = std::move(dims)};
    break;
  }
  case MsgType::Zlink: {
    auto fromCell = r.read<CellRef>();
    auto dim      = r.read<DimRef>();
    auto dir      = r.read<std::uint8_t>();
    auto toCell   = r.read<CellRef>();
    if (!fromCell || !dim || !dir || !toCell)
      return std::unexpected("Malformed Zlink");
    msg.payload = MsgZlink{.fromCell = *fromCell,
                           .dim      = *dim,
                           .dir      = static_cast<DimVector>(*dir),
                           .toCell   = *toCell};
    break;
  }
  case MsgType::Rlink: {
    auto success = r.read<std::uint8_t>();
    if (!success) return std::unexpected("Malformed Rlink");
    msg.payload = MsgRlink{.success = (*success != 0)};
    break;
  }
  case MsgType::Zmake: {
    auto suggestedId = r.read<CellRef>();
    auto valueKind   = r.read<std::uint8_t>();
    auto valueBits   = r.read<std::uint64_t>();
    if (!suggestedId || !valueKind || !valueBits)
      return std::unexpected("Malformed Zmake");
    msg.payload = MsgZmake{.suggestedId = *suggestedId,
                           .valueKind   = *valueKind,
                           .valueBits   = *valueBits};
    break;
  }
  case MsgType::Rmake: {
    auto newCell = r.read<CellRef>();
    if (!newCell) return std::unexpected("Malformed Rmake");
    msg.payload = MsgRmake{.newCell = *newCell};
    break;
  }
  case MsgType::Zwatch: {
    auto cell = r.read<CellRef>();
    auto dim  = r.read<DimRef>();
    auto dir  = r.read<std::uint8_t>();
    auto mask = r.read<std::uint32_t>();
    if (!cell || !dim || !dir || !mask)
      return std::unexpected("Malformed Zwatch");
    msg.payload = MsgZwatch{.cell = *cell,
                            .dim  = *dim,
                            .dir  = static_cast<DimVector>(*dir),
                            .mask = *mask};
    break;
  }
  case MsgType::Rwatch: {
    auto watchId = r.read<std::uint32_t>();
    if (!watchId) return std::unexpected("Malformed Rwatch");
    msg.payload = MsgRwatch{.watchId = *watchId};
    break;
  }
  case MsgType::Rerror: {
    auto ename = r.readString();
    if (!ename) return std::unexpected("Malformed Rerror");
    msg.payload = MsgRerror{.ename = std::move(*ename)};
    break;
  }
  }

  return msg;
}

xanadu::bencode::Value Codec::toBencode(const Message &msg) {
  xanadu::bencode::Dict d;
  d["type"] =
      xanadu::bencode::Value::integer(static_cast<std::int64_t>(msg.type));
  d["tag"] = xanadu::bencode::Value::integer(msg.tag);

  switch (msg.type) {
  case MsgType::Tversion: {
    const auto &p = std::get<MsgTversion>(msg.payload);
    d["msize"]    = xanadu::bencode::Value::integer(p.msize);
    d["version"]  = xanadu::bencode::Value::string(p.version);
    break;
  }
  case MsgType::Rversion: {
    const auto &p = std::get<MsgRversion>(msg.payload);
    d["msize"]    = xanadu::bencode::Value::integer(p.msize);
    d["version"]  = xanadu::bencode::Value::string(p.version);
    break;
  }
  case MsgType::Tauth: {
    const auto &p = std::get<MsgTauth>(msg.payload);
    d["afid"]     = xanadu::bencode::Value::integer(p.afid);
    d["uname"]    = xanadu::bencode::Value::string(p.uname);
    d["aname"]    = xanadu::bencode::Value::string(p.aname);
    break;
  }
  case MsgType::Rauth: {
    const auto &p = std::get<MsgRauth>(msg.payload);
    d["aqid"]     = xanadu::bencode::Value::integer(p.aqid);
    break;
  }
  case MsgType::Tattach: {
    const auto &p = std::get<MsgTattach>(msg.payload);
    d["fid"]      = xanadu::bencode::Value::integer(p.fid);
    d["afid"]     = xanadu::bencode::Value::integer(p.afid);
    d["uname"]    = xanadu::bencode::Value::string(p.uname);
    d["aname"]    = xanadu::bencode::Value::string(p.aname);
    break;
  }
  case MsgType::Rattach: {
    const auto &p = std::get<MsgRattach>(msg.payload);
    d["qid"]      = xanadu::bencode::Value::integer(p.qid);
    d["rootCell"] = xanadu::bencode::Value::integer(p.rootCell);
    break;
  }
  case MsgType::Tclunk: {
    const auto &p = std::get<MsgTclunk>(msg.payload);
    d["fid"]      = xanadu::bencode::Value::integer(p.fid);
    break;
  }
  case MsgType::Rclunk:
    break;
  case MsgType::Tflush: {
    const auto &p = std::get<MsgTflush>(msg.payload);
    d["oldtag"]   = xanadu::bencode::Value::integer(p.oldtag);
    break;
  }
  case MsgType::Rflush:
    break;
  case MsgType::Zstat: {
    const auto &p = std::get<MsgZstat>(msg.payload);
    d["cell"]     = xanadu::bencode::Value::integer(p.cell);
    break;
  }
  case MsgType::Rstat: {
    const auto &p   = std::get<MsgRstat>(msg.payload);
    d["cell"]       = xanadu::bencode::Value::integer(p.cell);
    d["flags"]      = xanadu::bencode::Value::integer(p.flags);
    d["spanLength"] = xanadu::bencode::Value::integer(
        static_cast<std::int64_t>(p.spanLength));
    d["linkCount"] = xanadu::bencode::Value::integer(p.linkCount);
    break;
  }
  case MsgType::Zhop: {
    const auto &p  = std::get<MsgZhop>(msg.payload);
    d["startCell"] = xanadu::bencode::Value::integer(p.startCell);
    d["dim"]       = xanadu::bencode::Value::integer(p.dim);
    d["dir"] =
        xanadu::bencode::Value::integer(static_cast<std::int64_t>(p.dir));
    d["hops"] = xanadu::bencode::Value::integer(p.hops);
    break;
  }
  case MsgType::Rhop: {
    const auto &p   = std::get<MsgRhop>(msg.payload);
    d["targetCell"] = xanadu::bencode::Value::integer(p.targetCell);
    break;
  }
  case MsgType::Zread: {
    const auto &p = std::get<MsgZread>(msg.payload);
    d["cell"]     = xanadu::bencode::Value::integer(p.cell);
    d["offset"] =
        xanadu::bencode::Value::integer(static_cast<std::int64_t>(p.offset));
    d["count"] = xanadu::bencode::Value::integer(p.count);
    break;
  }
  case MsgType::Rread: {
    const auto &p = std::get<MsgRread>(msg.payload);
    std::string s(reinterpret_cast<const char *>(p.data.data()), p.data.size());
    d["data"] = xanadu::bencode::Value::string(std::move(s));
    break;
  }
  case MsgType::Zdims: {
    const auto &p = std::get<MsgZdims>(msg.payload);
    d["cell"]     = xanadu::bencode::Value::integer(p.cell);
    break;
  }
  case MsgType::Rdims: {
    const auto &p = std::get<MsgRdims>(msg.payload);
    xanadu::bencode::List l;
    for (const auto dim : p.dimensions) {
      l.push_back(xanadu::bencode::Value::integer(dim));
    }
    d["dims"] = xanadu::bencode::Value::list(std::move(l));
    break;
  }
  case MsgType::Zlink: {
    const auto &p = std::get<MsgZlink>(msg.payload);
    d["fromCell"] = xanadu::bencode::Value::integer(p.fromCell);
    d["dim"]      = xanadu::bencode::Value::integer(p.dim);
    d["dir"] =
        xanadu::bencode::Value::integer(static_cast<std::int64_t>(p.dir));
    d["toCell"] = xanadu::bencode::Value::integer(p.toCell);
    break;
  }
  case MsgType::Rlink: {
    const auto &p = std::get<MsgRlink>(msg.payload);
    d["success"]  = xanadu::bencode::Value::integer(p.success ? 1 : 0);
    break;
  }
  case MsgType::Zmake: {
    const auto &p  = std::get<MsgZmake>(msg.payload);
    d["suggested"] = xanadu::bencode::Value::integer(p.suggestedId);
    d["valueKind"] = xanadu::bencode::Value::integer(p.valueKind);
    d["valueBits"] =
        xanadu::bencode::Value::integer(static_cast<std::int64_t>(p.valueBits));
    break;
  }
  case MsgType::Rmake: {
    const auto &p = std::get<MsgRmake>(msg.payload);
    d["newCell"]  = xanadu::bencode::Value::integer(p.newCell);
    break;
  }
  case MsgType::Zwatch: {
    const auto &p = std::get<MsgZwatch>(msg.payload);
    d["cell"]     = xanadu::bencode::Value::integer(p.cell);
    d["dim"]      = xanadu::bencode::Value::integer(p.dim);
    d["dir"] =
        xanadu::bencode::Value::integer(static_cast<std::int64_t>(p.dir));
    d["mask"] = xanadu::bencode::Value::integer(p.mask);
    break;
  }
  case MsgType::Rwatch: {
    const auto &p = std::get<MsgRwatch>(msg.payload);
    d["watchId"]  = xanadu::bencode::Value::integer(p.watchId);
    break;
  }
  case MsgType::Rerror: {
    const auto &p = std::get<MsgRerror>(msg.payload);
    d["ename"]    = xanadu::bencode::Value::string(p.ename);
    break;
  }
  }

  return xanadu::bencode::Value::dict(std::move(d));
}

std::expected<Message, std::string>
Codec::fromBencode(const xanadu::bencode::Value &val) {
  if (!val.isDict()) {
    return std::unexpected("Root 9Z Bencode value must be a dictionary");
  }

  const auto typeVal = val.find("type");
  const auto tagVal  = val.find("tag");
  if (!typeVal || !tagVal || !typeVal->isInteger() || !tagVal->isInteger()) {
    return std::unexpected("Missing type or tag in 9Z Bencode dictionary");
  }

  Message msg{
      .type = static_cast<MsgType>(typeVal->asInteger()),
      .tag  = static_cast<std::uint16_t>(tagVal->asInteger()),
  };

  switch (msg.type) {
  case MsgType::Tversion: {
    const auto msize = val.find("msize");
    const auto ver   = val.find("version");
    if (!msize || !ver) return std::unexpected("Malformed Tversion bencode");
    msg.payload = MsgTversion{
        .msize   = static_cast<std::uint32_t>(msize->asInteger()),
        .version = ver->asString(),
    };
    break;
  }
  case MsgType::Rversion: {
    const auto msize = val.find("msize");
    const auto ver   = val.find("version");
    if (!msize || !ver) return std::unexpected("Malformed Rversion bencode");
    msg.payload = MsgRversion{
        .msize   = static_cast<std::uint32_t>(msize->asInteger()),
        .version = ver->asString(),
    };
    break;
  }
  case MsgType::Tauth: {
    const auto afid  = val.find("afid");
    const auto uname = val.find("uname");
    const auto aname = val.find("aname");
    if (!afid || !uname || !aname)
      return std::unexpected("Malformed Tauth bencode");
    msg.payload = MsgTauth{
        .afid  = static_cast<std::uint32_t>(afid->asInteger()),
        .uname = uname->asString(),
        .aname = aname->asString(),
    };
    break;
  }
  case MsgType::Rauth: {
    const auto aqid = val.find("aqid");
    if (!aqid) return std::unexpected("Malformed Rauth bencode");
    msg.payload =
        MsgRauth{.aqid = static_cast<std::uint32_t>(aqid->asInteger())};
    break;
  }
  case MsgType::Tattach: {
    const auto fid   = val.find("fid");
    const auto afid  = val.find("afid");
    const auto uname = val.find("uname");
    const auto aname = val.find("aname");
    if (!fid || !afid || !uname || !aname)
      return std::unexpected("Malformed Tattach bencode");
    msg.payload = MsgTattach{
        .fid   = static_cast<std::uint32_t>(fid->asInteger()),
        .afid  = static_cast<std::uint32_t>(afid->asInteger()),
        .uname = uname->asString(),
        .aname = aname->asString(),
    };
    break;
  }
  case MsgType::Rattach: {
    const auto qid      = val.find("qid");
    const auto rootCell = val.find("rootCell");
    if (!qid || !rootCell) return std::unexpected("Malformed Rattach bencode");
    msg.payload = MsgRattach{
        .qid      = static_cast<std::uint32_t>(qid->asInteger()),
        .rootCell = static_cast<CellRef>(rootCell->asInteger()),
    };
    break;
  }
  case MsgType::Tclunk: {
    const auto fid = val.find("fid");
    if (!fid) return std::unexpected("Malformed Tclunk bencode");
    msg.payload =
        MsgTclunk{.fid = static_cast<std::uint32_t>(fid->asInteger())};
    break;
  }
  case MsgType::Rclunk:
    msg.payload = MsgRclunk{};
    break;
  case MsgType::Tflush: {
    const auto oldtag = val.find("oldtag");
    if (!oldtag) return std::unexpected("Malformed Tflush bencode");
    msg.payload =
        MsgTflush{.oldtag = static_cast<std::uint16_t>(oldtag->asInteger())};
    break;
  }
  case MsgType::Rflush:
    msg.payload = MsgRflush{};
    break;
  case MsgType::Zstat: {
    const auto cell = val.find("cell");
    if (!cell) return std::unexpected("Malformed Zstat bencode");
    msg.payload = MsgZstat{.cell = static_cast<CellRef>(cell->asInteger())};
    break;
  }
  case MsgType::Rstat: {
    const auto cell       = val.find("cell");
    const auto flags      = val.find("flags");
    const auto spanLength = val.find("spanLength");
    const auto linkCount  = val.find("linkCount");
    if (!cell || !flags || !spanLength || !linkCount)
      return std::unexpected("Malformed Rstat bencode");
    msg.payload = MsgRstat{
        .cell       = static_cast<CellRef>(cell->asInteger()),
        .flags      = static_cast<std::uint32_t>(flags->asInteger()),
        .spanLength = static_cast<std::uint64_t>(spanLength->asInteger()),
        .linkCount  = static_cast<std::uint32_t>(linkCount->asInteger()),
    };
    break;
  }
  case MsgType::Zhop: {
    const auto startCell = val.find("startCell");
    const auto dim       = val.find("dim");
    const auto dir       = val.find("dir");
    const auto hops      = val.find("hops");
    if (!startCell || !dim || !dir || !hops)
      return std::unexpected("Malformed Zhop bencode");
    msg.payload = MsgZhop{
        .startCell = static_cast<CellRef>(startCell->asInteger()),
        .dim       = static_cast<DimRef>(dim->asInteger()),
        .dir       = static_cast<DimVector>(dir->asInteger()),
        .hops      = static_cast<std::uint32_t>(hops->asInteger()),
    };
    break;
  }
  case MsgType::Rhop: {
    const auto targetCell = val.find("targetCell");
    if (!targetCell) return std::unexpected("Malformed Rhop bencode");
    msg.payload =
        MsgRhop{.targetCell = static_cast<CellRef>(targetCell->asInteger())};
    break;
  }
  case MsgType::Zread: {
    const auto cell   = val.find("cell");
    const auto offset = val.find("offset");
    const auto count  = val.find("count");
    if (!cell || !offset || !count)
      return std::unexpected("Malformed Zread bencode");
    msg.payload = MsgZread{
        .cell   = static_cast<CellRef>(cell->asInteger()),
        .offset = static_cast<std::uint64_t>(offset->asInteger()),
        .count  = static_cast<std::uint32_t>(count->asInteger()),
    };
    break;
  }
  case MsgType::Rread: {
    const auto data = val.find("data");
    if (!data || !data->isString())
      return std::unexpected("Malformed Rread bencode");
    const auto &s = data->asString();
    std::vector<std::uint8_t> v(s.begin(), s.end());
    msg.payload = MsgRread{.data = std::move(v)};
    break;
  }
  case MsgType::Zdims: {
    const auto cell = val.find("cell");
    if (!cell) return std::unexpected("Malformed Zdims bencode");
    msg.payload = MsgZdims{.cell = static_cast<CellRef>(cell->asInteger())};
    break;
  }
  case MsgType::Rdims: {
    const auto dims = val.find("dims");
    if (!dims || !dims->isList())
      return std::unexpected("Malformed Rdims bencode");
    std::vector<DimRef> dList;
    for (const auto &item : dims->asList()) {
      dList.push_back(static_cast<DimRef>(item.asInteger()));
    }
    msg.payload = MsgRdims{.dimensions = std::move(dList)};
    break;
  }
  case MsgType::Zlink: {
    const auto fromCell = val.find("fromCell");
    const auto dim      = val.find("dim");
    const auto dir      = val.find("dir");
    const auto toCell   = val.find("toCell");
    if (!fromCell || !dim || !dir || !toCell)
      return std::unexpected("Malformed Zlink bencode");
    msg.payload = MsgZlink{
        .fromCell = static_cast<CellRef>(fromCell->asInteger()),
        .dim      = static_cast<DimRef>(dim->asInteger()),
        .dir      = static_cast<DimVector>(dir->asInteger()),
        .toCell   = static_cast<CellRef>(toCell->asInteger()),
    };
    break;
  }
  case MsgType::Rlink: {
    const auto success = val.find("success");
    if (!success) return std::unexpected("Malformed Rlink bencode");
    msg.payload = MsgRlink{.success = (success->asInteger() != 0)};
    break;
  }
  case MsgType::Zmake: {
    const auto suggested = val.find("suggested");
    const auto valueKind = val.find("valueKind");
    const auto valueBits = val.find("valueBits");
    if (!suggested || !valueKind || !valueBits)
      return std::unexpected("Malformed Zmake bencode");
    msg.payload = MsgZmake{
        .suggestedId = static_cast<CellRef>(suggested->asInteger()),
        .valueKind   = static_cast<std::uint8_t>(valueKind->asInteger()),
        .valueBits   = static_cast<std::uint64_t>(valueBits->asInteger()),
    };
    break;
  }
  case MsgType::Rmake: {
    const auto newCell = val.find("newCell");
    if (!newCell) return std::unexpected("Malformed Rmake bencode");
    msg.payload =
        MsgRmake{.newCell = static_cast<CellRef>(newCell->asInteger())};
    break;
  }
  case MsgType::Zwatch: {
    const auto cell = val.find("cell");
    const auto dim  = val.find("dim");
    const auto dir  = val.find("dir");
    const auto mask = val.find("mask");
    if (!cell || !dim || !dir || !mask)
      return std::unexpected("Malformed Zwatch bencode");
    msg.payload = MsgZwatch{
        .cell = static_cast<CellRef>(cell->asInteger()),
        .dim  = static_cast<DimRef>(dim->asInteger()),
        .dir  = static_cast<DimVector>(dir->asInteger()),
        .mask = static_cast<std::uint32_t>(mask->asInteger()),
    };
    break;
  }
  case MsgType::Rwatch: {
    const auto watchId = val.find("watchId");
    if (!watchId) return std::unexpected("Malformed Rwatch bencode");
    msg.payload =
        MsgRwatch{.watchId = static_cast<std::uint32_t>(watchId->asInteger())};
    break;
  }
  case MsgType::Rerror: {
    const auto ename = val.find("ename");
    if (!ename) return std::unexpected("Malformed Rerror bencode");
    msg.payload = MsgRerror{.ename = ename->asString()};
    break;
  }
  }

  return msg;
}

} // namespace zigzag::zz9p
