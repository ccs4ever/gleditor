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
  return rebuild(version).materialize(spool);
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
          << op.span.length << ' ' << (op.source.isZero() ? "0" : op.source.str())
          << ' ' << op.sourceAt << ' ' << op.sourceLength << ' ' << op.link
          << '\n';
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
  nextLinkId = 1;

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
