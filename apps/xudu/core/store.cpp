#include "store.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "binary_ops.hpp"
#include "windows_quoting.hpp"

namespace xudu {

namespace {

/// Names of the files a store is written as.
constexpr const char *primediaFile = "primedia.spool";
constexpr const char *opsFile      = "ops.spool"; // pre-node-array stores
constexpr const char *opsNodesFile = "ops.nodes";
constexpr const char *linksFile    = "links.spool";
constexpr const char *originsFile  = "origins.spool"; // pre-scroll stores
constexpr const char *scrollsFile  = "scrolls.spool";

/// Not a protocol limit -- a branch ordinal is a plain std::uint32_t and the
/// compact binary format can encode any value that fits in one, past 254,
/// by escaping to a varint -- only a backstop so a search for a free branch
/// terminates instead of looping forever if every one of four billion
/// somehow were taken.
constexpr std::uint32_t branchSearchCeiling =
    std::numeric_limits<std::uint32_t>::max();

std::string readWholeFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Open @p path for writing one of the store's plain-text spools (scrolls,
/// links, or the OSMIC text ops export), imbued with the classic "C" locale
/// rather than whatever gleditor::initLocale() has set the process to.
///
/// These files are whitespace-delimited plain digits, meant to be read back
/// by splitting on spaces -- not by a locale-aware parser. Most locales'
/// numpunct groups large integers with a thousands separator, which a
/// document long enough to need a four-digit byte offset or length would
/// put in the middle of a field the reader expects to be one token,
/// producing a "malformed" error over a store that was never actually
/// corrupt, only written under the wrong locale.
std::ofstream openTextSpoolForWrite(const std::filesystem::path &path) {
  std::ofstream out(path, std::ios::trunc);
  out.imbue(std::locale::classic());
  return out;
}

/// The read side of openTextSpoolForWrite(), imbued the same way so a store
/// written correctly (in the classic locale) is also read that way,
/// regardless of what locale the reading process happens to be in.
std::ifstream openTextSpoolForRead(const std::filesystem::path &path) {
  std::ifstream in(path);
  in.imbue(std::locale::classic());
  return in;
}

} // namespace

void Store::putOp(const MicroversionId &produces, const Op &op) {
  if (produces.isZero()) {
    throw std::invalid_argument(
        "state zero is the null document and is not produced by an operation");
  }
  if (opsSpool.contains(produces)) {
    throw std::invalid_argument("microversion " + produces.str() +
                                " already has an operation; the operations "
                                "spool is append-only");
  }
  if (op.parent != produces.parent()) {
    throw std::invalid_argument(
        "operation filed under " + produces.str() + " claims parent " +
        op.parent.str() + ", but that name follows " + produces.parent().str());
  }

  const auto parentIdx = opsSpool.indexOf(op.parent);
  const auto sourceIdx = op.source.isZero() ? 0U : opsSpool.indexOf(op.source);
  // The ordinal is what makes the name recoverable from the tree, which is
  // how a sealed segment -- a file of nodes and nothing else -- gets indexed.
  const auto node = CompactOpNode::fromOp(
      op, parentIdx, sourceIdx, CompactOpNode::branchOrdinalFor(produces));
  opsSpool.append(node, produces);
}

std::optional<Op> Store::getOp(const MicroversionId &id) const {
  const auto *const node = opsSpool.get(id);
  if (nullptr == node) {
    return std::nullopt;
  }
  // The node names its parent and source by spool index; an Op names them the
  // way a person writes them, so both come back through idOf().
  return node->toOp(opsSpool.idOf(node->parentIndex),
                    opsSpool.idOf(node->sourceOpIndex));
}

std::vector<MicroversionId> Store::opsFor(const MicroversionId &version) const {
  std::vector<MicroversionId> needed;
  for (const auto &step : version.path()) {
    if (opsSpool.contains(step)) {
      needed.push_back(step);
    }
  }
  return needed;
}

void Store::replay(const CompactOpNode &node, Version &onto) const {
  switch (node.kind) {
  case OpKind::Insert: {
    onto.insert(node.at, node.span());
    break;
  }
  case OpKind::Delete: {
    onto.remove(node.at, node.length);
    break;
  }
  case OpKind::Rearrange: {
    onto.rearrange(node.at, node.length, node.to);
    break;
  }
  case OpKind::Transclude: {
    if (!node.span().empty()) {
      // Named directly by a content address, so there is no source document to
      // go through: the reference is already global.
      onto.insert(node.at, node.span());
      break;
    }
    // Resolved against the source version as it stands, which is what makes
    // this a virtual copy: the spans it yields are the source's own addresses,
    // so both versions end up pointing at one copy of the content.
    const auto from = rebuildFromIndex(node.sourceOpIndex);
    onto.insertSpans(node.at, from.spansFor(node.sourceAt, node.sourceLength));
    break;
  }
  case OpKind::Link: {
    // A link changes no text. It is recorded as an operation so that making
    // one is a point in hypertime like any other edit, which is what lets a
    // reader go back to before it was made.
    break;
  }
  case OpKind::PageBreak: {
    onto.insertBreak(node.at);
    break;
  }
  }
}

Version Store::rebuildFromIndex(const std::uint32_t index) const {
  Version built;
  for (const auto idx : opsSpool.ancestralPath(index)) {
    if (const auto *const node = opsSpool.get(idx); nullptr != node) {
      replay(*node, built);
    }
  }
  return built;
}

Version Store::rebuild(const MicroversionId &version) const {
  if (const auto targetIdx = opsSpool.indexOf(version); targetIdx > 0) {
    return rebuildFromIndex(targetIdx);
  }
  // Nothing is filed under this name. Replaying the longest recorded prefix of
  // it is still the right answer -- asking for a state one edit past the end
  // gets the end -- and the ancestral walk above cannot give it, having no
  // node to start from.
  Version built;
  for (const auto &step : version.path()) {
    if (const auto *const node = opsSpool.get(step); nullptr != node) {
      replay(*node, built);
    }
  }
  return built;
}

bool Store::advance(Version &document, const MicroversionId &known,
                    const MicroversionId &version) const {
  // One step on means the op filed under `version` names `known` as its
  // parent -- which is what putOp() checks when it is recorded, so asking
  // MicroversionId is asking the same question the spool already answered.
  if (version.isZero() || version.parent() != known) {
    return false;
  }
  const auto *const node = opsSpool.get(version);
  if (nullptr == node) {
    return false;
  }
  replay(*node, document);
  return true;
}

std::string Store::textOf(const MicroversionId &version) const {
  return rebuild(version).materialize(*this);
}

ScrollId Store::addScroll(const Scroll &scroll) {
  // Identified by the scroll it names, not by how it was written down: two
  // references to one scroll must share an id, or a transclusion between them
  // would be invisible to the address comparison that finds one.
  for (std::size_t i = 0; i < externals.size(); i++) {
    if (externals[i].sameContentAs(scroll)) {
      // Already known. Anything it says about where the bytes are is folded
      // in, since a second reference may have learned of a seal the first had
      // not -- but the identity, and every span using it, stays put.
      for (const auto &segment : scroll.segments) {
        externals[i].addSegment(segment);
      }
      return static_cast<ScrollId>(i + 1);
    }
  }
  externals.push_back(scroll);
  return static_cast<ScrollId>(externals.size());
}

void Store::addSegment(const ScrollId id, const ScrollSegment &segment) {
  if (localScroll == id || id > externals.size()) {
    return;
  }
  externals[id - 1].addSegment(segment);
}

const Scroll *Store::scroll(const ScrollId id) const {
  if (localScroll == id || id > externals.size()) {
    return nullptr;
  }
  return &externals[id - 1];
}

std::string Store::read(const PrimediaSpan &span) const {
  if (span.isLocal()) {
    return spool.read(span);
  }
  if (const auto vocab = readVocabulary(span)) {
    return *vocab;
  }
  const auto *const which = scroll(span.scroll);
  if (nullptr == which) {
    return {};
  }
  // Verified inside the resolver. Content that cannot be reached, or that does
  // not hash to what the reference named, comes back empty -- so a document
  // quoting a torrent nobody is seeding still opens, with the quotation blank
  // rather than with something invented in its place.
  return resolver.read(*which, span);
}

MicroversionId Store::transcludeExternal(const MicroversionId &parent,
                                         const std::uint32_t at,
                                         const Scroll &from,
                                         const std::uint64_t scrollOffset,
                                         const std::uint64_t length) {
  Op op;
  op.kind = OpKind::Transclude;
  op.at   = at;
  // No source version: the content is named directly by a content address, so
  // there is no other document to resolve it through. This is the case Xanadu
  // wants and the local spool cannot express.
  op.span = PrimediaSpan{addScroll(from), scrollOffset, length};
  return apply(parent, op);
}

MicroversionId Store::insertBreak(const MicroversionId &parent,
                                  const std::uint32_t at) {
  Op op;
  op.kind = OpKind::PageBreak;
  op.at   = at;
  return apply(parent, op);
}

MicroversionId Store::apply(const MicroversionId &parent, Op op) {
  op.parent = parent;

  // Straight on, when nothing has followed this state yet.
  const auto onward = parent.next();
  if (!opsSpool.contains(onward)) {
    putOp(onward, op);
    return onward;
  }

  // Something already follows it, so this is a second future for the same
  // state and gets a branch of its own. Nothing that already existed moves.
  // Ordinals rather than 'a'..'z': a state can have more than twenty-six
  // futures now, and the search just keeps counting past z into aa, ab, ...
  // rather than giving up there.
  for (std::uint32_t ordinal = 1; ordinal < branchSearchCeiling; ordinal++) {
    const auto branched = parent.branch(ordinal);
    if (!opsSpool.contains(branched)) {
      putOp(branched, op);
      return branched;
    }
  }
  throw std::runtime_error("microversion " + parent.str() +
                           " already has every branch a name can hold");
}

MicroversionId Store::insert(const MicroversionId &parent,
                             const std::uint32_t at,
                             const std::string_view text) {
  Op op;
  op.kind = OpKind::Insert;
  op.at   = at;
  // Into the spool first: the op records where the content went, never the
  // content, so the content has to have gone somewhere before there is an op.
  op.span = spool.append(text);
  return apply(parent, op);
}

MicroversionId Store::erase(const MicroversionId &parent,
                            const std::uint32_t at,
                            const std::uint32_t length) {
  Op op;
  op.kind   = OpKind::Delete;
  op.at     = at;
  op.length = length;
  return apply(parent, op);
}

MicroversionId Store::rearrange(const MicroversionId &parent,
                                const std::uint32_t at,
                                const std::uint32_t length,
                                const std::uint32_t to) {
  Op op;
  op.kind   = OpKind::Rearrange;
  op.at     = at;
  op.length = length;
  op.to     = to;
  return apply(parent, op);
}

MicroversionId Store::transclude(const MicroversionId &parent,
                                 const std::uint32_t at,
                                 const MicroversionId &source,
                                 const std::uint32_t sourceAt,
                                 const std::uint32_t sourceLength) {
  Op op;
  op.kind         = OpKind::Transclude;
  op.at           = at;
  op.source       = source;
  op.sourceAt     = sourceAt;
  op.sourceLength = sourceLength;
  return apply(parent, op);
}

MicroversionId Store::addLink(const MicroversionId &parent, Link link) {
  link.id = nextLinkId++;
  linkTable.emplace(link.id, std::move(link));

  Op op;
  op.kind = OpKind::Link;
  op.link = nextLinkId - 1;
  return apply(parent, op);
}

std::vector<const Link *> Store::linksTouching(const PrimediaSpan &span) const {
  std::vector<const Link *> found;
  for (const auto &[id, link] : linkTable) {
    if (link.touches(span)) {
      found.push_back(&link);
    }
  }
  return found;
}

std::optional<FormatAttribute>
Store::formatAttributeOf(const Link &link) const {
  if (LinkType::Format != link.type || link.right.empty()) {
    return std::nullopt;
  }
  const auto &named = link.right.front();
  for (const auto attribute : allFormatAttributes) {
    if (named == vocabularySpanFor(attribute)) {
      return attribute;
    }
  }
  return std::nullopt;
}

std::vector<MicroversionId> Store::children(const MicroversionId &id) const {
  std::vector<MicroversionId> found;
  if (opsSpool.contains(id.next())) {
    found.push_back(id.next());
  }
  // apply() always hands out the first free ordinal, and the spool is
  // append-only, so branches off one state are never sparse -- ordinal 3
  // existing means 1 and 2 were already taken when it was assigned. The
  // first ordinal not found is therefore where the real ones stop, not a
  // gap with more waiting past it.
  for (std::uint32_t ordinal = 1; ordinal < branchSearchCeiling; ordinal++) {
    const auto branched = id.branch(ordinal);
    if (!opsSpool.contains(branched)) {
      break;
    }
    found.push_back(branched);
  }
  return found;
}

std::vector<MicroversionId> Store::allVersions() const {
  // Sorted rather than in the order the spool holds them: a std::map used to
  // hold these and answered in this order, which is the one a person reading a
  // list of states expects -- 1, 1a1, 2, 3 -- and not the order they happened
  // to be typed in.
  std::vector<MicroversionId> found;
  found.reserve(opsSpool.size());
  for (std::uint32_t idx = 1; idx <= opsSpool.size(); idx++) {
    found.push_back(opsSpool.idOf(idx));
  }
  std::sort(found.begin(), found.end());
  return found;
}

MicroversionId Store::latest() const {
  // The last in replay order, which for a document edited straight through is
  // the newest. A store whose most recent work was on an earlier branch has no
  // single answer to "the latest", and this at least names a real state.
  //
  // The greatest name, deliberately, and not the last operation the spool
  // holds: the spool is in the order operations arrived, which is the order
  // they were typed while a store is open and the order they were written
  // while one is being read back, and those are not the same order once a
  // branch exists. Picking by name is the one answer that does not change
  // across a save and a load.
  MicroversionId newest;
  for (std::uint32_t idx = 1; idx <= opsSpool.size(); idx++) {
    if (const auto id = opsSpool.idOf(idx); newest < id) {
      newest = id;
    }
  }
  return newest;
}

std::vector<OpRecord> Store::opRecords() const {
  std::vector<OpRecord> records;
  records.reserve(opsSpool.size());
  for (std::uint32_t idx = 1; idx <= opsSpool.size(); idx++) {
    const auto *const node = opsSpool.get(idx);
    if (nullptr == node) {
      continue;
    }
    records.push_back(OpRecord{opsSpool.idOf(idx),
                               node->toOp(opsSpool.idOf(node->parentIndex),
                                          opsSpool.idOf(node->sourceOpIndex))});
  }
  std::sort(records.begin(), records.end(),
            [](const OpRecord &lhs, const OpRecord &rhs) {
              return lhs.produces < rhs.produces;
            });
  return records;
}

void Store::adoptOpRecords(const std::vector<OpRecord> &records) {
  for (const auto &record : records) {
    // Sorted by name, so a parent is always read before the state it produced
    // -- which putOp() needs, since it resolves the parent to a spool index.
    // A repeated name is dropped rather than thrown over: the std::map these
    // used to be read into kept the first of a duplicate and said nothing,
    // and a store that opened before must not stop opening now.
    if (!opsSpool.contains(record.produces)) {
      putOp(record.produces, record.op);
    }
  }
}

void Store::save(const std::string &directory) const {
  const std::filesystem::path dir(directory);
  std::filesystem::create_directories(dir);

  {
    // Appended to rather than rewritten once the file already holds a prefix
    // of what is in memory. The primedia spool only ever grows, so what is
    // already on disk is still correct and only the tail is new -- which
    // makes a save cost what was typed since the last one instead of the
    // whole document every time.
    //
    // The operations spool below cannot do this, and it is worth saying why
    // rather than leaving the asymmetry looking like an oversight: its
    // records are delta-coded against the record before them in name order
    // (see FLAG_SEQUENTIAL in binary_ops.cpp), and a branch off an early
    // state sorts into the middle of that order, not the end. There is no
    // tail to append. SegmentedOpsSpool is what carries the operations
    // incrementally; ops.spool is a compact whole-file export of them.
    const auto &bytes = spool.bytes();
    const bool fresh  = flushedPrimediaDirectory != directory;
    std::ofstream out(dir / primediaFile,
                      std::ios::binary |
                          (fresh ? std::ios::trunc : std::ios::app));
    const auto from =
        fresh ? std::size_t{0} : static_cast<std::size_t>(primediaFlushed);
    out.write(bytes.data() + from,
              static_cast<std::streamsize>(bytes.size() - from));
    flushedPrimediaDirectory = directory;
    primediaFlushed          = bytes.size();
  }
  {
    // The operations, as the array-backed tree they are held as: the file is
    // what the spool has in memory, so reading it back is a read rather than
    // a parse and a replay. The state-zero slot at index 0 is the arena's, not
    // the file's -- see SegmentedOpsSpool::addSealedSegment().
    //
    // Costs about sixteen times what the compact binary encoding did, which
    // for a file that never leaves the machine buys back the whole of the
    // decode. Writing it is a copy rather than an encode, so saving got
    // quicker as well as loading.
    //
    // It also settles what the emission order should be, by removing the
    // question: FLAG_SEQUENTIAL dropped a record's name when it continued the
    // one before it, which made the order records were written in worth
    // arguing about. Fixed-size nodes carry no names at all -- they are worked
    // out from the tree -- so there is no ordering left to choose.
    std::ofstream out(dir / opsNodesFile, std::ios::binary | std::ios::trunc);
    if (0 != opsSpool.size()) {
      out.write(reinterpret_cast<const char *>(opsSpool.rawOps() + 1),
                static_cast<std::streamsize>(opsSpool.size() *
                                             sizeof(CompactOpNode)));
    }
  }
  // A store written before the node array is superseded once it has been
  // written out in the new shape. Leaving it would leave two answers to what
  // the operations are, and load() would quietly prefer this one.
  if (std::filesystem::exists(dir / opsNodesFile)) {
    std::error_code ignored;
    std::filesystem::remove(dir / opsFile, ignored);
  }
  {
    // The scroll table: what a span's ScrollId means. Without it an id is a
    // number with no content behind it, so this is as much a part of the store
    // as the spans that refer to it.
    //
    // A scroll line names the scroll; the segment lines after it say which
    // torrent carries which stretch. They are separate lines because they are
    // separate kinds of fact with separate lifetimes: the first never changes,
    // and the second is rewritten every time something is sealed.
    auto out = openTextSpoolForWrite(dir / scrollsFile);
    for (std::size_t i = 0; i < externals.size(); i++) {
      const auto &scroll = externals[i];
      out << "scroll " << (i + 1) << ' '
          << (scroll.isNamed() ? scroll.publisher.hex() : "-") << ' '
          << (scroll.salt.empty() ? "-" : toHex(scroll.salt)) << '\n';
      for (const auto &segment : scroll.segments) {
        out << "segment " << (i + 1) << ' ' << segment.at << ' '
            << segment.length << ' ' << segment.torrent.hex() << ' '
            << segment.streamOffset << ' ' << segment.fileIndex << ' '
            << (segment.path.empty() ? "-" : segment.path) << '\n';
      }
    }
  }
  {
    auto out = openTextSpoolForWrite(dir / linksFile);
    for (const auto &[id, link] : linkTable) {
      out << id << ' ' << linkTypeName(link.type) << ' '
          << (link.owner.empty() ? "-" : link.owner) << ' ' << link.left.size()
          << ' ' << link.right.size();
      for (const auto &span : link.left) {
        out << ' ' << span.scroll << ' ' << span.start << ' ' << span.length;
      }
      for (const auto &span : link.right) {
        out << ' ' << span.scroll << ' ' << span.start << ' ' << span.length;
      }
      out << '\n';
    }
  }
}

void Store::saveOsmicText(const std::string &directory) const {
  const std::filesystem::path dir(directory);
  std::filesystem::create_directories(dir);

  {
    std::ofstream out(dir / primediaFile, std::ios::binary | std::ios::trunc);
    out << spool.bytes();
  }
  {
    // Canonical line-by-line OSMIC text format.
    auto out = openTextSpoolForWrite(dir / opsFile);
    writeOsmicTextOpsSpool(out, opRecords());
  }
  {
    auto out = openTextSpoolForWrite(dir / scrollsFile);
    for (std::size_t i = 0; i < externals.size(); i++) {
      const auto &scroll = externals[i];
      out << "scroll " << (i + 1) << ' '
          << (scroll.isNamed() ? scroll.publisher.hex() : "-") << ' '
          << (scroll.salt.empty() ? "-" : toHex(scroll.salt)) << '\n';
      for (const auto &segment : scroll.segments) {
        out << "segment " << (i + 1) << ' ' << segment.at << ' '
            << segment.length << ' ' << segment.torrent.hex() << ' '
            << segment.streamOffset << ' ' << segment.fileIndex << ' '
            << (segment.path.empty() ? "-" : segment.path) << '\n';
      }
    }
  }
  {
    auto out = openTextSpoolForWrite(dir / linksFile);
    for (const auto &[id, link] : linkTable) {
      out << id << ' ' << linkTypeName(link.type) << ' '
          << (link.owner.empty() ? "-" : link.owner) << ' ' << link.left.size()
          << ' ' << link.right.size();
      for (const auto &span : link.left) {
        out << ' ' << span.scroll << ' ' << span.start << ' ' << span.length;
      }
      for (const auto &span : link.right) {
        out << ' ' << span.scroll << ' ' << span.start << ' ' << span.length;
      }
      out << '\n';
    }
  }
}

std::string Store::exportOsmicText() const {
  std::ostringstream out;
  writeOsmicText(out);
  return out.str();
}

std::string Store::exportBinaryOps() const {
  std::ostringstream out(std::ios::binary);
  writeBinaryOpsSpool(out, opRecords());
  return out.str();
}

void Store::writeOsmicText(std::ostream &out) const {
  writeOsmicTextOpsSpool(out, opRecords());
}

void Store::load(const std::string &directory) {
  const std::filesystem::path dir(directory);
  spool.adopt(readWholeFile(dir / primediaFile));
  // What was just read is already on disk here, so the next save to this
  // directory can append to it rather than write it out again.
  flushedPrimediaDirectory = directory;
  primediaFlushed          = spool.size();
  opsSpool.clear();
  linkTable.clear();
  externals.clear();
  nextLinkId = 1;

  if (std::filesystem::exists(dir / scrollsFile)) {
    auto in = openTextSpoolForRead(dir / scrollsFile);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) {
        continue;
      }
      std::istringstream fields(line);
      std::string what;
      std::size_t which{};
      fields >> what >> which;
      if (!fields || 0 == which) {
        throw std::runtime_error("malformed scroll table in " +
                                 (dir / scrollsFile).string() + ": " + line);
      }
      // Appended rather than interned, because the ids already written into
      // the operations spool are positions in this list.
      while (externals.size() < which) {
        externals.emplace_back();
      }
      auto &scroll = externals[which - 1];

      if ("scroll" == what) {
        std::string key;
        std::string salt;
        fields >> key >> salt;
        if (!fields) {
          throw std::runtime_error("malformed scroll in " +
                                   (dir / scrollsFile).string() + ": " + line);
        }
        if ("-" != key) {
          scroll.publisher = PublicKey::fromHex(key);
        }
        if ("-" != salt) {
          scroll.salt = fromHex(salt);
        }
      } else if ("segment" == what) {
        ScrollSegment segment;
        std::string hash;
        fields >> segment.at >> segment.length >> hash >>
            segment.streamOffset >> segment.fileIndex >> segment.path;
        if (!fields) {
          throw std::runtime_error("malformed segment in " +
                                   (dir / scrollsFile).string() + ": " + line);
        }
        segment.torrent = InfoHash::fromHex(hash);
        if ("-" == segment.path) {
          segment.path.clear();
        }
        scroll.addSegment(segment);
      } else {
        throw std::runtime_error("unknown line in " +
                                 (dir / scrollsFile).string() + ": " + line);
      }
    }
  } else if (std::filesystem::exists(dir / originsFile)) {
    // A store written before scrolls existed. Every entry was one file of one
    // torrent, which is a scroll with a single segment covering all of it --
    // and because a one-segment scroll's offsets are that file's offsets, the
    // spans already written keep meaning exactly what they meant.
    std::ifstream in(dir / originsFile);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) {
        continue;
      }
      std::istringstream fields(line);
      std::string hash;
      std::string path;
      std::uint32_t fileIndex{};
      std::uint64_t fileOffset{};
      std::uint64_t fileLength{};
      fields >> hash >> fileIndex >> fileOffset >> fileLength >> path;
      if (!fields) {
        throw std::runtime_error("malformed origin in " +
                                 (dir / originsFile).string() + ": " + line);
      }
      externals.push_back(Scroll::ofTorrentFile(
          InfoHash::fromHex(hash), fileIndex, "-" == path ? "" : path,
          fileOffset, fileLength));
    }
  }

  if (std::filesystem::exists(dir / opsNodesFile)) {
    // Taken in whole: the nodes are already the shape they are held in, and
    // every name is worked out from the tree rather than read from the file.
    if (!opsSpool.openActiveSegment(dir / opsNodesFile)) {
      throw std::runtime_error("cannot read the operations in " +
                               (dir / opsNodesFile).string());
    }
  } else if (std::filesystem::exists(dir / opsFile)) {
    // Written before the operations were kept as nodes, in the compact binary
    // encoding or the OSMIC text one. Still read, and written back out as
    // nodes by the next save().
    std::ifstream in(dir / opsFile, std::ios::binary);
    std::vector<OpRecord> records;
    readOpsSpool(in, records);
    adoptOpRecords(records);
  }

  if (std::filesystem::exists(dir / linksFile)) {
    auto in = openTextSpoolForRead(dir / linksFile);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) {
        continue;
      }
      std::istringstream fields(line);
      Link link;
      std::string type;
      std::size_t lefts  = 0;
      std::size_t rights = 0;
      fields >> link.id >> type >> link.owner >> lefts >> rights;
      if (!fields) {
        throw std::runtime_error("malformed link in " +
                                 (dir / linksFile).string() + ": " + line);
      }
      link.type = linkTypeFromName(type);
      if ("-" == link.owner) {
        link.owner.clear();
      }
      const auto readSpans = [&fields](const std::size_t count,
                                       std::vector<PrimediaSpan> &into) {
        for (std::size_t i = 0; i < count; i++) {
          PrimediaSpan span;
          fields >> span.scroll >> span.start >> span.length;
          into.push_back(span);
        }
      };
      readSpans(lefts, link.left);
      readSpans(rights, link.right);
      nextLinkId = std::max(nextLinkId, link.id + 1);
      linkTable.emplace(link.id, std::move(link));
    }
  }
}

} // namespace xudu
