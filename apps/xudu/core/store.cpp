#include "store.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace xudu {

namespace {

/// Names of the three files a store is written as.
constexpr const char *primediaFile = "primedia.spool";
constexpr const char *opsFile      = "ops.spool";
constexpr const char *linksFile    = "links.spool";
constexpr const char *originsFile  = "origins.spool"; // pre-scroll stores
constexpr const char *scrollsFile  = "scrolls.spool";

std::string readWholeFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

void Store::putOp(const MicroversionId &produces, const Op &op) {
  if (produces.isZero()) {
    throw std::invalid_argument(
        "state zero is the null document and is not produced by an operation");
  }
  if (ops.contains(produces)) {
    throw std::invalid_argument("microversion " + produces.str() +
                                " already has an operation; the operations "
                                "spool is append-only");
  }
  if (op.parent != produces.parent()) {
    throw std::invalid_argument(
        "operation filed under " + produces.str() + " claims parent " +
        op.parent.str() + ", but that name follows " + produces.parent().str());
  }
  ops.emplace(produces, op);
}

const Op *Store::getOp(const MicroversionId &id) const {
  const auto found = ops.find(id);
  return found == ops.end() ? nullptr : &found->second;
}

std::vector<MicroversionId> Store::opsFor(const MicroversionId &version) const {
  std::vector<MicroversionId> needed;
  for (const auto &step : version.path()) {
    if (ops.contains(step)) {
      needed.push_back(step);
    }
  }
  return needed;
}

void Store::replay(const Op &op, Version &onto) const {
  switch (op.kind) {
  case OpKind::Insert: {
    onto.insert(op.at, op.span);
    break;
  }
  case OpKind::Delete: {
    onto.remove(op.at, op.length);
    break;
  }
  case OpKind::Rearrange: {
    onto.rearrange(op.at, op.length, op.to);
    break;
  }
  case OpKind::Transclude: {
    if (!op.span.empty()) {
      // Named directly by a content address, so there is no source document to
      // go through: the reference is already global.
      onto.insert(op.at, op.span);
      break;
    }
    // Resolved against the source version as it stands, which is what makes
    // this a virtual copy: the spans it yields are the source's own addresses,
    // so both versions end up pointing at one copy of the content.
    const auto from = rebuild(op.source);
    onto.insertSpans(op.at, from.spansFor(op.sourceAt, op.sourceLength));
    break;
  }
  case OpKind::Link: {
    // A link changes no text. It is recorded as an operation so that making
    // one is a point in hypertime like any other edit, which is what lets a
    // reader go back to before it was made.
    break;
  }
  }
}

Version Store::rebuild(const MicroversionId &version) const {
  Version built;
  for (const auto &step : version.path()) {
    if (const auto *const op = getOp(step); nullptr != op) {
      replay(*op, built);
    }
  }
  return built;
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

MicroversionId Store::apply(const MicroversionId &parent, Op op) {
  op.parent = parent;

  // Straight on, when nothing has followed this state yet.
  const auto onward = parent.next();
  if (!ops.contains(onward)) {
    putOp(onward, op);
    return onward;
  }

  // Something already follows it, so this is a second future for the same
  // state and gets a branch of its own. Nothing that already existed moves.
  for (char letter = 'a'; letter <= 'z'; letter++) {
    const auto branched = parent.branch(letter);
    if (!ops.contains(branched)) {
      putOp(branched, op);
      return branched;
    }
  }
  throw std::runtime_error("microversion " + parent.str() +
                           " already has twenty-six branches");
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

std::vector<MicroversionId> Store::children(const MicroversionId &id) const {
  std::vector<MicroversionId> found;
  if (ops.contains(id.next())) {
    found.push_back(id.next());
  }
  for (char letter = 'a'; letter <= 'z'; letter++) {
    const auto branched = id.branch(letter);
    if (ops.contains(branched)) {
      found.push_back(branched);
    }
  }
  return found;
}

std::vector<MicroversionId> Store::allVersions() const {
  std::vector<MicroversionId> found;
  found.reserve(ops.size());
  for (const auto &[id, op] : ops) {
    found.push_back(id);
  }
  return found;
}

MicroversionId Store::latest() const {
  // The last in replay order, which for a document edited straight through is
  // the newest. A store whose most recent work was on an earlier branch has no
  // single answer to "the latest", and this at least names a real state.
  return ops.empty() ? MicroversionId{} : ops.rbegin()->first;
}

void Store::save(const std::string &directory) const {
  const std::filesystem::path dir(directory);
  std::filesystem::create_directories(dir);

  {
    std::ofstream out(dir / primediaFile, std::ios::binary | std::ios::trunc);
    out << spool.bytes();
  }
  {
    // One line per operation, in replay order: an append-only file on disk as
    // well as in memory, and readable without this program.
    std::ofstream out(dir / opsFile, std::ios::trunc);
    for (const auto &[id, op] : ops) {
      out << id.str() << ' ' << opKindName(op.kind) << ' ' << op.at << ' '
          << op.length << ' ' << op.to << ' ' << op.span.start << ' '
          << op.span.length << ' '
          << (op.source.isZero() ? "0" : op.source.str()) << ' ' << op.sourceAt
          << ' ' << op.sourceLength << ' ' << op.link << ' ' << op.span.scroll
          << '\n';
    }
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
    std::ofstream out(dir / scrollsFile, std::ios::trunc);
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
    std::ofstream out(dir / linksFile, std::ios::trunc);
    for (const auto &[id, link] : linkTable) {
      out << id << ' ' << linkTypeName(link.type) << ' '
          << (link.owner.empty() ? "-" : link.owner) << ' ' << link.left.size()
          << ' ' << link.right.size();
      for (const auto &span : link.left) {
        out << ' ' << span.start << ' ' << span.length;
      }
      for (const auto &span : link.right) {
        out << ' ' << span.start << ' ' << span.length;
      }
      out << '\n';
    }
  }
}

void Store::load(const std::string &directory) {
  const std::filesystem::path dir(directory);
  spool.adopt(readWholeFile(dir / primediaFile));
  ops.clear();
  linkTable.clear();
  externals.clear();
  nextLinkId = 1;

  if (std::filesystem::exists(dir / scrollsFile)) {
    std::ifstream in(dir / scrollsFile);
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
  } else {
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

  {
    std::ifstream in(dir / opsFile);
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
      // Added after the first stores were written, so its absence means the
      // local spool -- which is what every span in such a store was.
      if (!(fields >> op.span.scroll)) {
        op.span.scroll = localScroll;
      }
      if (!fields) {
        throw std::runtime_error("malformed operation in " +
                                 (dir / opsFile).string() + ": " + line);
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
        throw std::runtime_error("unknown operation \"" + kind + "\" in " +
                                 (dir / opsFile).string());
      }
      ops.emplace(produces, op);
    }
  }

  {
    std::ifstream in(dir / linksFile);
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
          fields >> span.start >> span.length;
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

// vi: set sw=2 sts=2 ts=2 et:
