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
#include <unordered_map>
#include <utility>
#include <vector>

#include "binary_ops.hpp"
#include "store_tables.hpp"
#include "windows_quoting.hpp"

namespace xanadu {

namespace {

/// Refuse a span that names the arena's scratch scroll.
///
/// The address-side twin of setLink()'s isEphemeral() check: scratch bytes are
/// what an ArenaManifold constructed during an evaluation, and they have no
/// permanent address, so an operation quoting one would be a transclusion into
/// a scroll that does not exist. zigzag::promote() is what gives those bytes an
/// address; every other route in is refused here as well as in the fold, since
/// one span is structurally identical to another and nothing downstream could
/// tell. See spool.hpp's scratchScroll.
void requireAddressable(const PrimediaSpan &span, const std::string_view what) {
  if (scratchScroll == span.scroll) {
    throw std::invalid_argument(
        std::string(what) +
        " names the arena scratch scroll, which has no permanent address -- "
        "promote() is what spools those bytes for real; see design R8 and "
        "design/vlog-logic-extension.md section 5.5");
  }
}

/// Names of the files a store is written as.
constexpr const char *opsNodesFile = "ops.nodes";
/// An export of the operations, in either encoding: canonical OSMIC text as
/// --export-osmic writes it, or the compact binary wire format. readOpsSpool()
/// tells them apart by their magic, so one name covers both honestly.
///
/// Not `ops.spool`. That name meant the operations spool back when the spool
/// *was* the file; since ops.nodes it names the in-memory segmented structure
/// instead, so a file called that is a second meaning for a live name -- and
/// a directory holding one is a store from before nodes, which load() refuses
/// by that name rather than reading as this.
constexpr const char *opsExportFile = "ops.export";

/// Files a store no longer has, refused by name if one is found. Each was the
/// only copy of something, so opening a directory holding one as though it
/// were a modern store would lose exactly what it holds.
constexpr const char *primediaFile        = "primedia.spool";
constexpr const char *legacyOpsFile       = "ops.spool";
constexpr const char *currentVersionsFile = "current.yaml";
constexpr const char *versionsFile        = "versions.yaml";

/// Not a protocol limit -- a branch ordinal is a plain std::uint32_t and the
/// compact binary format can encode any value that fits in one, past 254,
/// by escaping to a varint -- only a backstop so a search for a free branch
/// terminates instead of looping forever if every one of four billion
/// somehow were taken.
constexpr std::uint32_t branchSearchCeiling =
    std::numeric_limits<std::uint32_t>::max();

/// Open @p path for writing the OSMIC text operations export, imbued with the
/// classic "C" locale rather than whatever gleditor::initLocale() has set the
/// process to.
///
/// It is whitespace-delimited plain digits, meant to be read back by splitting
/// on spaces -- not by a locale-aware parser. Most locales' numpunct groups
/// large integers with a thousands separator, which a document long enough to
/// need a four-digit byte offset or length would put in the middle of a field
/// the reader expects to be one token, producing a "malformed" error over a
/// store that was never actually corrupt, only written under the wrong locale.
///
/// The scroll and link tables used to be written this way too, and are now a
/// binary container instead -- so this is the last plain-text spool, and the
/// hazard above now applies only to it.
std::ofstream openTextSpoolForWrite(const std::filesystem::path &path) {
  std::ofstream out(path, std::ios::trunc);
  out.imbue(std::locale::classic());
  return out;
}

} // namespace

Store::Store() : userPermascroll_(std::make_shared<UserPermascroll>()) {}

Store::Store(std::shared_ptr<UserPermascroll> userPermascroll)
    : userPermascroll_(userPermascroll ? std::move(userPermascroll)
                                       : std::make_shared<UserPermascroll>()) {}

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
  const auto index = opsSpool.append(node, produces);

  // The two cells genesis mints are the first two minted, so noticing them
  // here costs a comparison per operation and saves a scan per question. See
  // homeCell() for why they are not simply indices 1 and 2.
  if (OpKind::Structure == op.kind &&
      StructureVerb::MakeCell == structureVerbOf(op.flags)) {
    if (zigzag::noCell == homeCell_) {
      homeCell_ = index;
    } else if (zigzag::noCell == dimsDimension_) {
      dimsDimension_ = index;
    }
  }
}

void Store::indexGenesisCells() {
  homeCell_      = zigzag::noCell;
  dimsDimension_ = zigzag::noCell;
  for (std::uint32_t idx = 1; idx <= opsSpool.size(); idx++) {
    const auto *const node = opsSpool.get(idx);
    if (nullptr == node || OpKind::Structure != node->kind ||
        StructureVerb::MakeCell != structureVerbOf(node->flags)) {
      continue;
    }
    if (zigzag::noCell == homeCell_) {
      homeCell_ = idx;
      continue;
    }
    dimsDimension_ = idx;
    return;
  }
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
  case OpKind::Structure: {
    // Changes no text. A slice's structure is a second replay product of this
    // same spool -- see rebuildManifold() and zigzag::Manifold -- so folding it
    // here would be building the wrong one of the two. Recorded as an operation
    // so that structural editing is a point in hypertime like any other edit,
    // exactly as OpKind::Link is.
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

zigzag::Manifold
Store::rebuildManifoldFromIndex(const std::uint32_t index) const {
  zigzag::Manifold folded;
  for (const auto idx : opsSpool.ancestralPath(index)) {
    if (const auto *const node = opsSpool.get(idx); nullptr != node) {
      // Every node rather than the Structure ones: applyStructure() ignores
      // the other kinds, and filtering here would be a second place that has
      // to know which kinds fold.
      folded.applyStructure(idx, *node);
    }
  }
  // A cold fold ends tight, which is what makes the per-cell cost R12 quotes
  // the cost of a manifold that was just loaded rather than a best case.
  folded.compact();
  return folded;
}

zigzag::Manifold Store::rebuildManifold(const MicroversionId &version) const {
  if (const auto targetIdx = opsSpool.indexOf(version); targetIdx > 0) {
    return rebuildManifoldFromIndex(targetIdx);
  }
  // Nothing filed under this name: fold the longest recorded prefix of it, for
  // the same reason rebuild() replays one.
  zigzag::Manifold folded;
  for (const auto &step : version.path()) {
    if (const auto *const node = opsSpool.get(step); nullptr != node) {
      folded.applyStructure(opsSpool.indexOf(step), *node);
    }
  }
  folded.compact();
  return folded;
}

std::uint32_t Store::lastOpOnCell(const MicroversionId &parent,
                                  const zigzag::CellRef cell,
                                  const zigzag::Manifold *const known) const {
  if (nullptr != known) {
    if (const auto *const slot = known->slot(cell); nullptr != slot) {
      return slot->lastOp;
    }
  }
  auto head = cell;
  for (const auto idx : opsSpool.ancestralPath(opsSpool.indexOf(parent))) {
    if (idx <= cell) {
      continue;
    }
    const auto *const node = opsSpool.get(idx);
    if (nullptr != node && OpKind::Structure == node->kind &&
        node->sourceOpIndex == head) {
      head = idx;
    }
  }
  return head;
}

void Store::requireCellOp(const zigzag::CellRef ref, const char *what) const {
  // Only that the reference names a Structure operation, which is what a
  // CellRef is: the index of the operation that minted the cell, or of one
  // that has touched it since. Whether it is on *this* state's ancestral path
  // is a question only a fold can answer -- the manifold refuses it there --
  // but naming a text insert or an index no operation sits at is a mistake
  // cheap enough to catch here, and silently recording an operation the fold
  // will drop is the failure mode this codebase keeps ruling out.
  const auto *const node = opsSpool.get(ref);
  if (nullptr == node || OpKind::Structure != node->kind) {
    throw std::invalid_argument(
        std::string(what) + " is operation " + std::to_string(ref) +
        ", which is not one that touches a cell; a CellRef is the index of a "
        "Structure operation");
  }
}

MicroversionId Store::makeCell(const MicroversionId &parent,
                               const PrimediaSpan &content) {
  requireAddressable(content, "a cell's content");
  Op op;
  op.kind  = OpKind::Structure;
  op.flags = structureFlags(StructureVerb::MakeCell);
  op.span  = content;
  return apply(parent, op);
}

MicroversionId Store::makeCell(const MicroversionId &parent,
                               const std::string_view text) {
  // Into the permascroll first, exactly as insert() does it: a cell's content
  // is ordinary spooled primedia, which is what makes it a link endpoint and a
  // transclusion source rather than a payload of its own kind. See R6.
  return makeCell(parent, userPermascroll_->append(text));
}

MicroversionId Store::applyScalar(const MicroversionId &parent,
                                  const zigzag::CellRef cell,
                                  const ScalarValue &value,
                                  const zigzag::Manifold *const known) {
  // The rendering is spooled whether this mints or restates, because a cell's
  // content is primedia at an address and an address is what a link, a
  // transclusion and a diff all attach to. R6 accepts the permascroll now
  // holding bytes a program wrote rather than bytes a person typed -- worst
  // case 24 for a double, typically fewer -- as the price of a scalar being a
  // first-class Xanadu object rather than a payload of its own kind.
  const auto span = userPermascroll_->append(value.text);
  if (zigzag::noCell == cell) {
    Op op;
    op.kind  = OpKind::Structure;
    op.flags = structureFlags(StructureVerb::MakeCell, false, value.kind);
    op.span  = span;
    op.value = value.bits;
    return apply(parent, op);
  }
  return setValue(parent, cell, span, value.kind, value.bits, known);
}

MicroversionId Store::makeScalarCell(const MicroversionId &parent,
                                     const double value) {
  return applyScalar(parent, zigzag::noCell, scalarValue(value), nullptr);
}

MicroversionId Store::makeScalarCell(const MicroversionId &parent,
                                     const bool value) {
  return applyScalar(parent, zigzag::noCell, scalarValue(value), nullptr);
}

MicroversionId Store::makeScalarCell(const MicroversionId &parent,
                                     const std::int64_t value) {
  return applyScalar(parent, zigzag::noCell, scalarValue(value), nullptr);
}

MicroversionId Store::setScalar(const MicroversionId &parent,
                                const zigzag::CellRef cell, const double value,
                                const zigzag::Manifold *const known) {
  return applyScalar(parent, cell, scalarValue(value), known);
}

MicroversionId Store::setScalar(const MicroversionId &parent,
                                const zigzag::CellRef cell, const bool value,
                                const zigzag::Manifold *const known) {
  return applyScalar(parent, cell, scalarValue(value), known);
}

MicroversionId Store::setScalar(const MicroversionId &parent,
                                const zigzag::CellRef cell,
                                const std::int64_t value,
                                const zigzag::Manifold *const known) {
  return applyScalar(parent, cell, scalarValue(value), known);
}

MicroversionId Store::spliceCellSpan(const MicroversionId &parent,
                                     const zigzag::CellRef cell,
                                     const std::uint64_t at,
                                     const std::uint64_t removing,
                                     const PrimediaSpan &quoted,
                                     const zigzag::Manifold *const known) {
  if (zigzag::noCell == cell || zigzag::isEphemeral(cell)) {
    throw std::invalid_argument("spliceCell needs a cell an operation minted");
  }
  requireCellOp(cell, "the cell being edited");
  requireAddressable(quoted, "the span being spliced in");

  Op op;
  op.kind  = OpKind::Structure;
  op.flags = structureFlags(StructureVerb::Splice);
  // The one Structure verb whose `at` is not zero: it is an offset inside the
  // cell's own content, which is the frame this operation edits within.
  op.at     = static_cast<std::uint32_t>(at);
  op.length = static_cast<std::uint32_t>(removing);
  op.span   = quoted;
  if (const auto previous = lastOpOnCell(parent, cell, known);
      zigzag::noCell != previous) {
    op.source = opsSpool.idOf(previous);
  }
  return apply(parent, op);
}

MicroversionId Store::spliceCell(const MicroversionId &parent,
                                 const zigzag::CellRef cell,
                                 const std::uint64_t at,
                                 const std::uint64_t removing,
                                 const std::string_view text,
                                 const zigzag::Manifold *const known) {
  // Into the permascroll first, and only what was actually typed -- which is
  // the difference this verb exists to make. setCellText() appends the whole
  // cell however little of it changed.
  const auto span =
      text.empty() ? PrimediaSpan{} : userPermascroll_->append(text);
  return spliceCellSpan(parent, cell, at, removing, span, known);
}

MicroversionId Store::setCellText(const MicroversionId &parent,
                                  const zigzag::CellRef cell,
                                  const std::string_view text,
                                  const zigzag::Manifold *const known) {
  const auto span = userPermascroll_->append(text);
  return setValue(parent, cell, span, ValueKind::None, 0, known);
}

MicroversionId Store::setLink(const MicroversionId &parent,
                              const zigzag::CellRef from,
                              const zigzag::DimRef dim, const bool negward,
                              const zigzag::CellRef to,
                              const zigzag::Manifold *const known) {
  if (zigzag::noCell == from) {
    throw std::invalid_argument("a link has to be from some cell");
  }
  if (zigzag::isEphemeral(from) || zigzag::isEphemeral(dim) ||
      zigzag::isEphemeral(to)) {
    throw std::invalid_argument(
        "a derived cell has no operation behind it, so a link naming one "
        "cannot be recorded -- see design R8 and R12");
  }
  requireCellOp(from, "the cell a link is from");
  requireCellOp(dim, "the dimension a link is along");
  if (zigzag::noCell != to) {
    requireCellOp(to, "the cell a link is to");
  }

  Op op;
  op.kind  = OpKind::Structure;
  op.flags = structureFlags(StructureVerb::SetLink, negward);
  op.to    = to;
  op.link  = dim;
  // The chain, which is also how the fold knows whose link this is: there is
  // no separate subject field, and the chain's far end is the MakeCell whose
  // index is the cell. See R7.
  if (const auto previous = lastOpOnCell(parent, from, known);
      zigzag::noCell != previous) {
    op.source = opsSpool.idOf(previous);
  }
  return apply(parent, op);
}

MicroversionId Store::setValue(const MicroversionId &parent,
                               const zigzag::CellRef cell,
                               const PrimediaSpan &content,
                               const ValueKind kind, const std::uint64_t bits,
                               const zigzag::Manifold *const known) {
  if (zigzag::noCell == cell || zigzag::isEphemeral(cell)) {
    throw std::invalid_argument("setValue needs a cell an operation minted");
  }
  requireCellOp(cell, "the cell a value is set on");
  requireAddressable(content, "the content a value restates");
  Op op;
  op.kind  = OpKind::Structure;
  op.flags = structureFlags(StructureVerb::SetValue, false, kind);
  op.span  = content;
  op.value = bits;
  if (const auto previous = lastOpOnCell(parent, cell, known);
      zigzag::noCell != previous) {
    op.source = opsSpool.idOf(previous);
  }
  return apply(parent, op);
}

MicroversionId Store::sliceGenesis(const MicroversionId &parent) {
  if (zigzag::noCell != homeCell_) {
    throw std::invalid_argument(
        "this store already has a home cell at operation " +
        std::to_string(homeCell_) +
        "; genesis mints the two cells that cannot be deleted, once");
  }
  const auto withHome = makeCell(parent, std::string_view{"home"});
  const auto withDims = makeCell(withHome, std::string_view{"d.dims"});
  // d.dims is a dimension like any other, so it belongs on its own rank --
  // which is what makes dimensions() report it alongside everything minted
  // afterwards instead of it being the one dimension that is invisible.
  return setLink(withDims, homeCell_, dimsDimension_, false, dimsDimension_);
}

Store::MintedDimension Store::makeDimension(const MicroversionId &parent,
                                            const std::string_view name,
                                            const zigzag::Manifold *known) {
  if (zigzag::noCell == dimsDimension_) {
    throw std::invalid_argument(
        "a dimension goes on the d.dims rank, and this store has no d.dims "
        "cell yet -- call sliceGenesis() first");
  }
  const auto minted = makeCell(parent, name);
  const auto ref    = cellRefOf(minted);

  std::optional<zigzag::Manifold> folded;
  if (nullptr == known) {
    folded = rebuildManifold(minted);
    known  = &folded.value();
  }
  // The rank's tail, so dimensions come back in the order they were minted.
  auto tail = homeCell_;
  for (auto step = known->cellCount() + 1; step > 0; step--) {
    const auto next = known->linked(tail, dimsDimension_, false);
    if (zigzag::noCell == next || next == ref || next == homeCell_) {
      break;
    }
    tail = next;
  }
  return MintedDimension{
      setLink(minted, tail, dimsDimension_, false, ref, known), ref};
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

MultiVersionDiffResult
Store::diffVersions(const std::vector<MicroversionId> &versions) const {
  MultiVersionDiffResult result;
  result.totalComparedVersions = versions.size();
  if (versions.empty()) {
    return result;
  }

  struct VerData {
    MicroversionId id;
    Version doc;
    std::string text;
    std::vector<std::pair<ScrollId, std::uint64_t>> charAddresses;
  };

  std::vector<VerData> built;
  built.reserve(versions.size());

  struct GlobalAddr {
    ScrollId scroll{0};
    std::uint64_t address{0};
    bool operator==(const GlobalAddr &) const = default;
  };
  struct GlobalAddrHash {
    std::size_t operator()(const GlobalAddr &g) const noexcept {
      return std::hash<std::uint64_t>{}(
          (static_cast<std::uint64_t>(g.scroll) << 32) ^ g.address);
    }
  };

  std::unordered_map<GlobalAddr, std::vector<std::size_t>, GlobalAddrHash>
      addressPresence;

  for (std::size_t vIdx = 0; vIdx < versions.size(); ++vIdx) {
    VerData vd;
    vd.id   = versions[vIdx];
    vd.doc  = rebuild(vd.id);
    vd.text = vd.doc.materialize(*this);

    for (const auto &piece : vd.doc.pieces()) {
      if (piece.scroll == breakMarkerScroll) {
        continue;
      }
      for (std::uint64_t i = 0; i < piece.length; ++i) {
        const GlobalAddr ga{piece.scroll, piece.start + i};
        vd.charAddresses.emplace_back(piece.scroll, piece.start + i);

        auto &vec = addressPresence[ga];
        if (vec.empty() || vec.back() != vIdx) {
          vec.push_back(vIdx);
        }
      }
    }
    built.push_back(std::move(vd));
  }

  const std::size_t K = versions.size();

  for (std::size_t vIdx = 0; vIdx < built.size(); ++vIdx) {
    const auto &vd = built[vIdx];
    SingleVersionDiff sv;
    sv.version = vd.id;
    sv.text    = vd.text;

    const std::size_t nChars = vd.charAddresses.size();
    if (nChars == 0) {
      if (K > 1) {
        for (const auto &[addr, vList] : addressPresence) {
          if (std::ranges::find(vList, vIdx) == vList.end()) {
            sv.deletedChars++;
          }
        }
      }
      result.versions.push_back(std::move(sv));
      continue;
    }

    std::vector<DiffKind> charKinds(nChars);
    std::vector<std::size_t> charSharings(nChars);

    for (std::size_t c = 0; c < nChars; ++c) {
      const auto &[scroll, addr] = vd.charAddresses[c];
      const auto it = addressPresence.find(GlobalAddr{scroll, addr});
      const std::size_t shareCount =
          (it != addressPresence.end()) ? it->second.size() : 1;
      charSharings[c] = shareCount;

      if (K <= 1) {
        charKinds[c] = DiffKind::Universal;
        sv.universalChars++;
      } else if (shareCount == K) {
        charKinds[c] = DiffKind::Universal;
        sv.universalChars++;
      } else if (shareCount > 1) {
        charKinds[c] = DiffKind::Shared;
        sv.sharedChars++;
      } else {
        charKinds[c] = DiffKind::Unique;
        sv.uniqueChars++;
      }
    }

    std::uint32_t spanStart = 0;
    for (std::size_t c = 1; c <= nChars; ++c) {
      if (c == nChars || charKinds[c] != charKinds[spanStart] ||
          charSharings[c] != charSharings[spanStart]) {
        DiffSpan ds;
        ds.kind         = charKinds[spanStart];
        ds.offset       = spanStart;
        ds.length       = static_cast<std::uint32_t>(c - spanStart);
        ds.sharingCount = charSharings[spanStart];
        sv.spans.push_back(ds);
        spanStart = static_cast<std::uint32_t>(c);
      }
    }

    if (K > 1) {
      for (const auto &[addr, vList] : addressPresence) {
        if (std::ranges::find(vList, vIdx) == vList.end()) {
          sv.deletedChars++;
        }
      }
    }

    result.versions.push_back(std::move(sv));
  }

  return result;
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

const ScrollSegment *Store::containerFor(const PrimediaSpan &span) const {
  if (span.isLocal()) {
    return localSegments.segmentAt(span.start);
  }
  const auto *external = scroll(span.scroll);
  return nullptr == external ? nullptr : external->segmentAt(span.start);
}

std::vector<ScrollSegment>
Store::segmentsOverlapping(const ScrollId scrollId, const std::uint64_t start,
                           const std::uint64_t length) const {
  const auto *const owner =
      localScroll == scrollId ? &localSegments : scroll(scrollId);
  std::vector<ScrollSegment> found;
  if (nullptr == owner) {
    return found;
  }
  const auto rangeEnd = start + length;
  for (const auto &segment : owner->segments) {
    if (segment.at < rangeEnd && segment.end() > start) {
      found.push_back(segment);
    }
  }
  return found;
}

ResolveResult Store::resolve(const PrimediaSpan &span) const {
  if (span.isLocal()) {
    return ResolveResult{.status = ResolutionStatus::VerifiedBytes,
                         .text   = userPermascroll_->read(span)};
  }
  if (const auto vocab = readVocabulary(span)) {
    return ResolveResult{.status = ResolutionStatus::VerifiedBytes,
                         .text   = *vocab};
  }
  const auto *const which = scroll(span.scroll);
  if (nullptr == which) {
    return ResolveResult{
        .status     = ResolutionStatus::WithheldRedacted,
        .holeRecord = PublishedHoleRecord{.at     = span.start,
                                          .length = span.length,
                                          .reason = HoleReason::Unsealed}};
  }
  // Check ephemeral live author buffer if not yet sealed into a torrent piece
  const auto liveText = readRemoteAuthorBuffer(span);
  if (!liveText.empty()) {
    return ResolveResult{.status = ResolutionStatus::VerifiedBytes,
                         .text   = liveText};
  }
  auto res = resolver.resolve(*which, span);
  if (res.status == ResolutionStatus::MissingPieces) {
    res.status     = ResolutionStatus::WithheldRedacted;
    res.holeRecord = PublishedHoleRecord{.at     = span.start,
                                         .length = span.length,
                                         .reason = HoleReason::Unsealed};
  }
  return res;
}

std::string Store::read(const PrimediaSpan &span) const {
  if (span.isLocal()) {
    return userPermascroll_->read(span);
  }
  if (const auto vocab = readVocabulary(span)) {
    return *vocab;
  }
  const auto *const which = scroll(span.scroll);
  if (nullptr == which) {
    return {};
  }
  // Check ephemeral live author buffer if not yet sealed into a torrent piece
  const auto liveText = readRemoteAuthorBuffer(span);
  if (!liveText.empty()) {
    return liveText;
  }
  // Verified inside the resolver. Content that cannot be reached, or that does
  // not hash to what the reference named, comes back empty -- so a document
  // quoting a torrent nobody is seeding still opens, with the quotation blank
  // rather than with something invented in its place.
  return resolver.read(*which, span);
}

void Store::setExternalLiveBytes(const std::string_view authorScrollKey,
                                 const std::uint64_t start,
                                 const std::string_view text) {
  if (authorScrollKey.empty() || text.empty()) {
    return;
  }
  auto &chunks      = remoteAuthorBuffers_[std::string(authorScrollKey)];
  const auto newEnd = start + text.size();
  bool merged       = false;

  for (auto &chunk : chunks) {
    const auto chunkEnd = chunk.start + chunk.text.size();
    if (chunkEnd == start) {
      chunk.text.append(text);
      merged = true;
      break;
    }
    if (newEnd == chunk.start) {
      chunk.text.insert(0, text);
      chunk.start = start;
      merged      = true;
      break;
    }
    if (start >= chunk.start && newEnd <= chunkEnd) {
      merged = true;
      break;
    }
  }

  if (!merged) {
    chunks.push_back(
        RemoteAuthorChunk{.start = start, .text = std::string(text)});
  }

  if (chunks.size() > 1) {
    std::sort(chunks.begin(), chunks.end(),
              [](const RemoteAuthorChunk &a, const RemoteAuthorChunk &b) {
                return a.start < b.start;
              });
    std::vector<RemoteAuthorChunk> consolidated;
    consolidated.reserve(chunks.size());
    for (auto &c : chunks) {
      if (consolidated.empty()) {
        consolidated.push_back(std::move(c));
      } else {
        auto &last         = consolidated.back();
        const auto lastEnd = last.start + last.text.size();
        if (c.start <= lastEnd) {
          if (c.start + c.text.size() > lastEnd) {
            const auto extraOffset = lastEnd - c.start;
            last.text.append(c.text.substr(extraOffset));
          }
        } else {
          consolidated.push_back(std::move(c));
        }
      }
    }
    chunks = std::move(consolidated);
  }
}

std::string Store::readRemoteAuthorBuffer(const PrimediaSpan &span) const {
  if (span.scroll == 0) {
    return {};
  }
  const auto *const which = scroll(span.scroll);
  if (!which) {
    return {};
  }
  const std::string key = "btpk:" + which->publisher.hex() + ":" + which->salt;
  const auto it         = remoteAuthorBuffers_.find(key);
  if (it == remoteAuthorBuffers_.end()) {
    return {};
  }

  const auto reqStart = span.start;
  const auto reqEnd   = span.start + span.length;
  for (const auto &chunk : it->second) {
    const auto chunkStart = chunk.start;
    const auto chunkEnd   = chunk.start + chunk.text.size();
    if (reqStart >= chunkStart && reqEnd <= chunkEnd) {
      const auto relOffset = reqStart - chunkStart;
      return chunk.text.substr(static_cast<std::size_t>(relOffset),
                               static_cast<std::size_t>(span.length));
    }
  }
  return {};
}

void Store::clearRemoteAuthorBuffer(const std::string_view authorScrollKey) {
  remoteAuthorBuffers_.erase(std::string(authorScrollKey));
}

void Store::trimRemoteAuthorBuffer(const std::string_view authorScrollKey,
                                   const std::uint64_t sealedUpTo) {
  if (sealedUpTo == 0) {
    clearRemoteAuthorBuffer(authorScrollKey);
    return;
  }
  const auto it = remoteAuthorBuffers_.find(std::string(authorScrollKey));
  if (it == remoteAuthorBuffers_.end()) {
    return;
  }
  auto &chunks = it->second;
  for (auto chunkIt = chunks.begin(); chunkIt != chunks.end();) {
    const auto chunkEnd = chunkIt->start + chunkIt->text.size();
    if (chunkEnd <= sealedUpTo) {
      chunkIt = chunks.erase(chunkIt);
    } else if (chunkIt->start < sealedUpTo) {
      const auto trimBytes =
          static_cast<std::size_t>(sealedUpTo - chunkIt->start);
      chunkIt->text  = chunkIt->text.substr(trimBytes);
      chunkIt->start = sealedUpTo;
      ++chunkIt;
    } else {
      ++chunkIt;
    }
  }
  if (chunks.empty()) {
    remoteAuthorBuffers_.erase(it);
  }
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

MicroversionId
Store::applyRemoteLiveOp(const Op &op, const std::string_view primediaText,
                         const std::string_view authorScrollKey) {
  Op localOp               = op;
  std::string effectiveKey = std::string(authorScrollKey);

  if (effectiveKey.empty()) {
    if (localOp.span.scroll > 0 && localOp.span.scroll <= externals.size()) {
      const auto *sc = scroll(localOp.span.scroll);
      if (sc) {
        effectiveKey = "btpk:" + sc->publisher.hex() + ":" + sc->salt;
      }
    }
  }
  if (effectiveKey.empty()) {
    effectiveKey =
        "btpk:0000000000000000000000000000000000000000000000000000000000000000:"
        "remote_author";
  }

  if (localOp.kind == OpKind::Insert) {
    Scroll targetScroll;
    if (effectiveKey.starts_with("btpk:")) {
      const auto rest  = std::string_view(effectiveKey).substr(5);
      const auto colon = rest.find(':');
      if (colon != std::string_view::npos) {
        targetScroll.publisher = PublicKey::fromHex(rest.substr(0, colon));
        targetScroll.salt      = std::string(rest.substr(colon + 1));
      }
    }
    const auto scrollId = addScroll(targetScroll);
    localOp.span.scroll = scrollId;

    if (!primediaText.empty()) {
      const auto it = remoteAuthorBuffers_.find(effectiveKey);
      const std::uint64_t bufferEnd =
          (it != remoteAuthorBuffers_.end() && !it->second.empty())
              ? (it->second.back().start + it->second.back().text.size())
              : 0;

      // Newly typed primedia is strictly append-only on the author's scroll;
      // if the op's span.start is before bufferEnd or was unassigned, anchor to
      // bufferEnd.
      if (localOp.span.start < bufferEnd) {
        localOp.span.start = bufferEnd;
      }
      if (localOp.span.length == 0) {
        localOp.span.length = static_cast<std::uint64_t>(primediaText.size());
      }
      setExternalLiveBytes(effectiveKey, localOp.span.start, primediaText);
    }
  }

  // NOTE: userPermascroll_->append() is NEVER called. Remote operations NEVER
  // pollute Slot 0!
  return apply(localOp.parent, localOp);
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
  op.span = userPermascroll_->append(text);
  return apply(parent, op);
}

Store::InsertedMedia Store::insertMedia(const MicroversionId &parent,
                                        const std::uint32_t at,
                                        const std::string_view bytes,
                                        std::string mimeType) {
  Op op;
  op.kind = OpKind::Insert;
  op.at   = at;
  op.span = userPermascroll_->append(bytes);
  localSegments.addSegment(ScrollSegment{
      .at       = op.span.start,
      .length   = op.span.length,
      .mimeType = std::move(mimeType),
  });
  return InsertedMedia{apply(parent, op), op.span};
}

MicroversionId Store::insertSpan(const MicroversionId &parent,
                                 const std::uint32_t at,
                                 const PrimediaSpan &span) {
  Op op;
  op.kind = OpKind::Insert;
  op.at   = at;
  op.span = span;
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

const std::vector<MicroversionId> &Store::currentVersions() const {
  if (currentVersions_.empty()) {
    const auto lat = latest();
    if (!lat.isZero()) {
      currentVersions_.push_back(lat);
    }
  }
  return currentVersions_;
}

MicroversionId Store::primaryCurrentVersion() const {
  const auto &cur = currentVersions();
  return cur.empty() ? latest() : cur.front();
}

void Store::setCurrentVersions(std::vector<MicroversionId> versions) {
  currentVersions_ = std::move(versions);
}

void Store::repointCurrentVersion(const MicroversionId &version) {
  currentVersions_ = {version};
}

void Store::addCurrentVersion(const MicroversionId &version) {
  if (std::ranges::find(currentVersions_, version) == currentVersions_.end()) {
    currentVersions_.push_back(version);
  }
}

void Store::removeCurrentVersion(const MicroversionId &version) {
  std::erase(currentVersions_, version);
}

void Store::setVersionAnnotation(const MicroversionId &id,
                                 VersionAnnotation annotation) {
  if (!annotation.alias.empty()) {
    aliasIndex_[annotation.alias] = id;
  }
  versionAnnotations_[id] = std::move(annotation);
}

std::optional<VersionAnnotation>
Store::versionAnnotation(const MicroversionId &id) const {
  const auto it = versionAnnotations_.find(id);
  if (it != versionAnnotations_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<MicroversionId>
Store::resolveAlias(std::string_view alias) const {
  const auto it = aliasIndex_.find(std::string(alias));
  if (it != aliasIndex_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::string Store::displayName(const MicroversionId &id) const {
  if (const auto ann = versionAnnotation(id); ann && !ann->alias.empty()) {
    return ann->alias;
  }
  return id.str();
}

std::vector<OpRecord>
Store::opRecords(const std::uint32_t sinceExclusive) const {
  std::vector<OpRecord> records;
  records.reserve(opsSpool.size());
  for (std::uint32_t idx = sinceExclusive + 1; idx <= opsSpool.size(); idx++) {
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

  // The content the operations below name lives in the author's permascroll,
  // not here, so saving a document means making sure what was typed into it is
  // durable -- not copying it. A store that wrote its own copy made every open
  // document a second place the author's text lived, and the addresses in it
  // only agreed with the permascroll's because every store rewrote the whole
  // thing every time.
  userPermascroll_->flush();
  {
    // The operations, as the array-backed tree they are held as: the file is
    // what the spool has in memory, so reading it back is a read rather than
    // a parse and a replay.
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
    //
    // Written by the spool rather than here. This used to be an ofstream of
    // its own, which meant the shape of a segment file was agreed between two
    // pieces of code instead of known by one -- and that is the arrangement
    // that let migration step 1 change the shape and go unnoticed.
    if (!opsSpool.writeSegmentFile(dir / opsNodesFile)) {
      throw std::runtime_error("cannot write the operations to " +
                               (dir / opsNodesFile).string());
    }
  }
  {
    // The scroll table and the link table: what a span's ScrollId means, and
    // what connects one span to another. Without the first an id is a number
    // with no content behind it, so both are as much a part of the store as
    // the spans that refer to them.
    //
    // One container rather than two plaintext files. There was never a reason
    // for them to be separate artefacts other than that they were written at
    // different times -- both are per-store side tables replayed at load --
    // and being plaintext was buying only that they could be read with `less`,
    // which tools/xudu-dump buys back. See R11 and store_tables.hpp.
    writeStoreTables(dir / storeTablesName,
                     StoreTables{.scrolls            = externals,
                                 .localSegments      = localSegments.segments,
                                 .links              = linkTable,
                                 .currentVersions    = currentVersions(),
                                 .versionAnnotations = versionAnnotations_});
    // The files this container replaced, taken with it. Leaving them would
    // leave two answers to what the scrolls are, and load() refuses a
    // directory holding both rather than choosing.
    std::error_code ignored;
    for (const auto *const superseded :
         {"scrolls.spool", "links.spool", "origins.spool", currentVersionsFile,
          versionsFile}) {
      std::filesystem::remove(dir / superseded, ignored);
    }
  }
}

void Store::saveOsmicText(const std::string &directory) const {
  const std::filesystem::path dir(directory);
  std::filesystem::create_directories(dir);

  userPermascroll_->flush();
  {
    // Canonical line-by-line OSMIC text format.
    auto out = openTextSpoolForWrite(dir / opsExportFile);
    writeOsmicTextOpsSpool(out, opRecords());
  }
  {
    // The side tables in the store's own container, not in text. What this
    // export exists for is the *operations* in canonical OSMIC text -- that is
    // what the format is named after and what exportOsmicText() renders. The
    // scroll and link tables being plaintext alongside them was incidental,
    // and writing them that way now would produce a directory that load()
    // reads the operations out of and silently finds no scrolls in.
    writeStoreTables(dir / storeTablesName,
                     StoreTables{.scrolls            = externals,
                                 .localSegments      = localSegments.segments,
                                 .links              = linkTable,
                                 .currentVersions    = currentVersions(),
                                 .versionAnnotations = versionAnnotations_});
  }
}

std::string Store::exportOsmicText() const {
  std::ostringstream out;
  writeOsmicText(out);
  return out.str();
}

std::string Store::exportBinaryOps(const std::uint32_t sinceExclusive) const {
  std::ostringstream out(std::ios::binary);
  writeBinaryOpsSpool(out, opRecords(sinceExclusive));
  return out.str();
}

void Store::writeOsmicText(std::ostream &out) const {
  writeOsmicTextOpsSpool(out, opRecords());
}

void Store::load(const std::string &directory) {
  const std::filesystem::path dir(directory);

  // A store from when a document carried a copy of the author's whole
  // permascroll. Refused rather than opened, and refused *before* anything is
  // read: the operations in it name addresses in that copy, and this build
  // resolves local spans against the permascroll the caller supplied. Opening
  // it would rebuild every version against the wrong scroll and show text that
  // is not the document -- worse than failing, because it looks like it worked.
  //
  // The bytes are not lost and this says where they are: point the permascroll
  // at the file and the addresses line up again, because the copy *was* the
  // permascroll.
  if (std::filesystem::exists(dir / primediaFile)) {
    throw StoreTablesUnreadable(
        (dir / primediaFile).string() +
        " is a document's own copy of the author's permascroll, from before "
        "primedia stopped being siloed per document. This build reads local "
        "spans from the permascroll the store was opened with; open this one "
        "with that file as the permascroll instead. See design R11.");
  }
  if (std::filesystem::exists(dir / legacyOpsFile)) {
    throw OpsSegmentUnreadable(
        (dir / legacyOpsFile).string() +
        " is an operations spool from before operations were kept as nodes, "
        "and this build does not read one. See design R11.");
  }

  opsSpool.clear();
  linkTable.clear();
  externals.clear();
  localSegments = Scroll{};
  nextLinkId    = 1;
  currentVersions_.clear();
  versionAnnotations_.clear();
  aliasIndex_.clear();

  if (std::filesystem::exists(dir / opsNodesFile)) {
    // Taken in whole: the nodes are already the shape they are held in, and
    // every name is worked out from the tree rather than read from the file.
    if (!opsSpool.openActiveSegment(dir / opsNodesFile)) {
      throw std::runtime_error("cannot read the operations in " +
                               (dir / opsNodesFile).string());
    }
  } else if (std::filesystem::exists(dir / opsExportFile)) {
    // What saveOsmicText() wrote: the operations in canonical OSMIC text.
    // Read back and written out as nodes by the next save().
    std::ifstream in(dir / opsExportFile, std::ios::binary);
    std::vector<OpRecord> records;
    readOpsSpool(in, records);
    adoptOpRecords(records);
  }

  // A store whose side tables were still two plaintext files. Refused rather
  // than opened: the operations would load and every span into an external
  // scroll would resolve to nothing, so the document would come back looking
  // like it had lost its quotations rather than looking broken. That is the
  // failure R14 exists to stop, and deleting a reader under R11 does not
  // license reintroducing it.
  if (!std::filesystem::exists(dir / storeTablesName)) {
    for (const auto *const superseded :
         {"scrolls.spool", "links.spool", "origins.spool"}) {
      if (std::filesystem::exists(dir / superseded)) {
        throw StoreTablesUnreadable(
            (dir / superseded).string() +
            " is a store's side tables from before they were one container, "
            "and this build does not read them. See design R11.");
      }
    }
  }

  if (std::filesystem::exists(dir / storeTablesName)) {
    // Both side tables at once, from one container. The plaintext scroll and
    // link parsers that were here -- a getline loop each, with a throw per
    // malformed field and a locale-imbued stream apiece so that a thousands
    // separator could not turn a byte offset into two tokens -- are gone with
    // the format they were reading. So is the origins.spool reader, which
    // existed only to open stores written before there was a scroll table at
    // all: keeping a reader so that an old file still parses is exactly the
    // tax R11 refuses.
    auto tables            = readStoreTables(dir / storeTablesName);
    externals              = std::move(tables.scrolls);
    localSegments          = Scroll{};
    localSegments.segments = std::move(tables.localSegments);
    linkTable              = std::move(tables.links);
    for (const auto &[id, link] : linkTable) {
      nextLinkId = std::max(nextLinkId, id + 1);
    }
    currentVersions_ = std::move(tables.currentVersions);
    // Through setVersionAnnotation() rather than assigned, so that the alias
    // index is built from the annotations rather than being a third thing that
    // has to be kept in step with them.
    for (auto &[id, annotation] : tables.versionAnnotations) {
      setVersionAnnotation(id, std::move(annotation));
    }
  }

  // The nodes above arrived as a mapped segment rather than through putOp(),
  // so which operations minted the genesis cells has to be read back out of
  // them. Cheap next to the load itself, and it is the whole reason the two
  // refs are derived rather than written into the side tables: a store's own
  // operations already say what they are.
  indexGenesisCells();
}

} // namespace xanadu
