#include "store.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "binary_ops.hpp"
#include "common/xanadu/osmic_walker.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"
#include "publication.hpp"
#include "store_tables.hpp"
#include "user_permascroll.hpp"
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

Store::Store()
    : userPermascroll_(std::make_shared<UserPermascroll>()),
      chronofilade_(std::make_unique<enfilade::Chronofilade>()) {}

Store::Store(std::shared_ptr<UserPermascroll> userPermascroll)
    : userPermascroll_(userPermascroll ? std::move(userPermascroll)
                                       : std::make_shared<UserPermascroll>()),
      chronofilade_(std::make_unique<enfilade::Chronofilade>()) {}

Store::~Store() { zigzag::DimensionRegistry::instance().unregisterStore(this); }

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
  if (!op.parent.isZero() && 0 == parentIdx) {
    throw std::invalid_argument("operation filed under " + produces.str() +
                                " names unknown parent " + op.parent.str());
  }

  const auto sourceIdx = op.source.isZero() ? 0U : opsSpool.indexOf(op.source);
  if (!op.source.isZero() && 0 == sourceIdx && OpKind::Structure == op.kind) {
    throw std::invalid_argument("operation filed under " + produces.str() +
                                " names unknown source " + op.source.str());
  }
  // The ordinal is what makes the name recoverable from the tree, which is
  // how a sealed segment -- a file of nodes and nothing else -- gets indexed.
  const auto node = CompactOpNode::fromOp(
      op, parentIdx, sourceIdx, CompactOpNode::branchOrdinalFor(produces));
  const auto index = opsSpool.append(node, produces);

  if (chronofilade_ && index > 0) {
    chronofilade_->recordOp(index, node, produces, *this);
  }

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
  if (chronofilade_) {
    return chronofilade_->rebuildVersion(index, *this);
  }
  Version built;
  OsmicWalker::walkAncestral(
      opsSpool, index,
      [this, &built](std::uint32_t, const CompactOpNode &node) {
        replay(node, built);
      });
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
  folded.setStore(const_cast<Store *>(this));
  OsmicWalker::walkAncestral(
      opsSpool, index, [&folded](std::uint32_t idx, const CompactOpNode &node) {
        // A cold fold keeps going past a refusal: the manifold counts it in
        // refusedOps(), which is where a caller checking honesty looks.
        static_cast<void>(folded.applyStructure(idx, node));
      });
  // A cold fold ends tight, which is what makes the per-cell cost R12 quotes
  // the cost of a manifold that was just loaded rather than a best case.
  folded.compact();
  const_cast<Store *>(this)->syncCurrentVersionsFromRank(folded);
  return folded;
}

zigzag::Manifold Store::rebuildManifold(const MicroversionId &version) const {
  if (const auto targetIdx = opsSpool.indexOf(version); targetIdx > 0) {
    return rebuildManifoldFromIndex(targetIdx);
  }
  // Nothing filed under this name: fold the longest recorded prefix of it, for
  // the same reason rebuild() replays one.
  zigzag::Manifold folded;
  folded.setStore(const_cast<Store *>(this));
  for (const auto &step : version.path()) {
    if (const auto *const node = opsSpool.get(step); nullptr != node) {
      // As above: refusals are counted, not fatal to the fold.
      static_cast<void>(folded.applyStructure(opsSpool.indexOf(step), *node));
    }
  }
  folded.compact();
  return folded;
}

std::uint32_t Store::lastOpOnCell(const MicroversionId &parent,
                                  const zigzag::CellRef cell,
                                  const zigzag::Manifold *const known) const {
  if (nullptr != known) {
    if (const auto slot = known->slot(cell); slot.has_value()) {
      return slot->lastOp;
    }
  }
  auto head = cell;
  OsmicWalker::walkAncestral(
      opsSpool, opsSpool.indexOf(parent),
      [&head, cell](std::uint32_t idx, const CompactOpNode &node) {
        if (idx > cell && OpKind::Structure == node.kind &&
            node.sourceOpIndex == head) {
          head = idx;
        }
      });
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

MicroversionId Store::makeOpHandle(const MicroversionId &parent,
                                   const std::uint32_t target,
                                   const std::string_view text) {
  if (zigzag::isEphemeral(target)) {
    throw std::invalid_argument(
        "a derived cell has no operation behind it, so a handle naming one "
        "cannot be recorded -- see design R8 and R12");
  }
  const auto *const node = opsSpool.get(target);
  if (nullptr == node) {
    throw std::invalid_argument("target operation " + std::to_string(target) +
                                " does not exist in store");
  }
  const auto span =
      text.empty() ? PrimediaSpan{} : userPermascroll_->append(text);
  Op op;
  op.kind = OpKind::Structure;
  op.flags =
      structureFlags(StructureVerb::MakeCell, false, ValueKind::OpHandle);
  op.span  = span;
  op.value = target;
  return apply(parent, op);
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

MicroversionId
Store::setLink(const MicroversionId &parent, const zigzag::CellRef from,
               const zigzag::DimRef dim, const zigzag::DimVector dir,
               const zigzag::CellRef to, const zigzag::Manifold *const known) {
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
  op.flags = structureFlags(StructureVerb::SetLink, dir);
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
  const auto res = setLink(withDims, homeCell_, dimsDimension_,
                           zigzag::DimVector::POS, dimsDimension_);
  zigzag::DimensionRegistry::instance().registerDim(*this, "d.dims",
                                                    dimsDimension_);
  return res;
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
    const auto next =
        known->linked(tail, dimsDimension_, zigzag::DimVector::POS);
    if (zigzag::noCell == next || next == ref || next == homeCell_) {
      break;
    }
    tail = next;
  }
  const auto version =
      setLink(minted, tail, dimsDimension_, zigzag::DimVector::POS, ref, known);
  zigzag::DimensionRegistry::instance().registerDim(*this, name, ref);
  return MintedDimension{.version = version, .dim = ref};
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

bool Store::advanceTo(Version &document, const MicroversionId &known,
                      const MicroversionId &version) const {
  if (known == version) {
    return true;
  }
  const auto fromIdx = opsSpool.indexOf(known);
  const auto toIdx   = opsSpool.indexOf(version);
  if (chronofilade_ && toIdx > 0) {
    return chronofilade_->advance(document, fromIdx, toIdx, *this);
  }
  document = rebuild(version);
  return true;
}

bool Store::verifyAgainstFullRebuild(const std::uint32_t index) const {
  Version raw;
  OsmicWalker::walkAncestral(
      opsSpool, index, [this, &raw](std::uint32_t, const CompactOpNode &node) {
        replay(node, raw);
      });
  if (chronofilade_) {
    return chronofilade_->verifyAgainstFullRebuild(index, *this, raw);
  }
  return true;
}

bool Store::verifyAgainstFullRebuild(const MicroversionId &version) const {
  const auto idx = opsSpool.indexOf(version);
  return verifyAgainstFullRebuild(idx);
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
        const GlobalAddr ga{.scroll = piece.scroll, .address = piece.start + i};
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
      const auto it =
          addressPresence.find(GlobalAddr{.scroll = scroll, .address = addr});
      const std::size_t shareCount =
          (it != addressPresence.end()) ? it->second.size() : 1;
      charSharings[c] = shareCount;

      if (K <= 1 || shareCount == K) {
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
  if (localScroll == id) {
    localSegments.addSegment(segment);
    return;
  }
  if (id > externals.size()) {
    return;
  }
  externals[id - 1].addSegment(segment);
}

void Store::setContentSource(const ContentSource *source) {
  resolver.setSource(source);
  if (source) {
    for (auto &sc : externals) {
      hydrateExternalScroll(sc);
    }
  }
}

void Store::hydrateExternalScroll(Scroll &sc) const {
  const auto *source = resolver.contentSource();
  if (!source) {
    return;
  }
  for (auto &seg : sc.segments) {
    if (seg.length == 0 || seg.path.empty()) {
      if (const auto meta = source->metainfo(seg.torrent)) {
        if (seg.fileIndex < meta->files().size()) {
          const auto &f = meta->files()[seg.fileIndex];
          if (seg.path.empty()) {
            seg.path = f.path;
          }
          if (seg.streamOffset == 0) {
            seg.streamOffset = f.offset;
          }
          if (seg.length == 0) {
            seg.at     = 0;
            seg.length = f.length;
          }
        }
      }
    }
  }
}

gleditor::cpp26::optional<const Scroll &>
Store::scroll(const ScrollId id) const {
  if (localScroll == id || id > externals.size()) {
    return gleditor::cpp26::nullopt;
  }
  auto &sc = const_cast<Scroll &>(externals[id - 1]);
  hydrateExternalScroll(sc);
  return sc;
}

gleditor::cpp26::optional<const ScrollSegment &>
Store::containerFor(const PrimediaSpan &span) const {
  if (span.isLocal()) {
    return localSegments.segmentAt(span.start);
  }
  return scroll(span.scroll).and_then([&](const Scroll &external) {
    return external.segmentAt(span.start);
  });
}

std::vector<ScrollSegment>
Store::segmentsOverlapping(const ScrollId scrollId, const std::uint64_t start,
                           const std::uint64_t length) const {
  const auto owner =
      localScroll == scrollId
          ? gleditor::cpp26::optional<const Scroll &>{localSegments}
          : scroll(scrollId);
  std::vector<ScrollSegment> found;
  if (!owner) {
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

void Store::setBootstrapPermascroll(
    std::string key,
    std::function<ResolveResult(const PrimediaSpan &)> reader) {
  bootstrapPermascrollKey_ = std::move(key);
  bootstrapReader_         = std::move(reader);
}

void Store::setProvenance(SignedProvenance prov, const SigningOptions &where) {
  if (!prov.signature.empty() || !prov.tsv.empty()) {
    const auto check = verifyProvenance(prov, where);
    if (!check.signatureValid) {
      throw std::runtime_error("unverified or tampered provenance: " +
                               check.detail);
    }
  }
  provenance_ = std::move(prov);
}

ResolveResult Store::resolve(const PrimediaSpan &span) const {
  if (span.isLocal()) {
    if (!bootstrapPermascrollKey_.empty() && userPermascroll_->size() == 0 &&
        bootstrapReader_) {
      return bootstrapReader_(span);
    }
    return ResolveResult{.status = ResolutionStatus::VerifiedBytes,
                         .text   = userPermascroll_->read(span)};
  }
  if (const auto vocab = readVocabulary(span)) {
    return ResolveResult{.status = ResolutionStatus::VerifiedBytes,
                         .text   = *vocab};
  }
  const auto which = scroll(span.scroll);
  if (!which) {
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
    if (!bootstrapPermascrollKey_.empty() && userPermascroll_->size() == 0 &&
        bootstrapReader_) {
      return bootstrapReader_(span).text;
    }
    return userPermascroll_->read(span);
  }
  if (const auto vocab = readVocabulary(span)) {
    return *vocab;
  }
  const auto which = scroll(span.scroll);
  if (!which) {
    return {};
  }
  // Check ephemeral live author buffer if not yet sealed into a torrent piece
  auto liveText = readRemoteAuthorBuffer(span);
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
    std::ranges::sort(
        chunks, [](const RemoteAuthorChunk &a, const RemoteAuthorChunk &b) {
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
  const auto which = scroll(span.scroll);
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
  op.span = PrimediaSpan{
      .scroll = addScroll(from), .start = scrollOffset, .length = length};
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
      const auto sc = scroll(localOp.span.scroll);
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
      localOp.span.start = std::max(localOp.span.start, bufferEnd);
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
  auto onward = parent.next();
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
  return InsertedMedia{.version = apply(parent, op), .span = op.span};
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

MicroversionId Store::addLink(const MicroversionId &parent, Link link,
                              const zigzag::Manifold *const known) {
  std::optional<zigzag::Manifold> folded;
  const zigzag::Manifold *currentFold = known;
  if (nullptr == currentFold) {
    folded      = rebuildManifold(parent);
    currentFold = &folded.value();
  }

  auto curHead = parent;

  if (zigzag::noCell == homeCell_) {
    curHead     = sliceGenesis(curHead);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  } else if (!currentFold->contains(homeCell_)) {
    curHead     = structureHead();
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  auto ensureDim = [&](const std::string_view name) {
    auto dim = currentFold->dimensionNamed(name, *this);
    if (!dim) {
      const auto minted = makeDimension(curHead, name, currentFold);
      curHead           = minted.version;
      folded            = rebuildManifold(curHead);
      currentFold       = &folded.value();
      return minted.dim;
    }
    return *dim;
  };

  const auto dimLinks   = ensureDim("d.links");
  const auto dimFrom    = ensureDim("d.from");
  const auto dimTo      = ensureDim("d.to");
  const auto dimType    = ensureDim("d.linktype");
  const auto dimTier    = ensureDim("d.linktier");
  const auto dimOwner   = ensureDim("d.owner");
  const auto dimCurator = ensureDim("d.curator");

  // Mint the link cell itself.
  curHead             = makeCell(curHead, PrimediaSpan{});
  const auto linkCell = cellRefOf(curHead);
  link.id             = linkCell;
  folded              = rebuildManifold(curHead);
  currentFold         = &folded.value();

  // Link onto d.links rank off homeCell_
  const auto tailLinks = zigzag::rankTail(*currentFold, homeCell_, dimLinks,
                                          zigzag::DimVector::POS);
  curHead     = setLink(curHead, tailLinks, dimLinks, zigzag::DimVector::POS,
                        linkCell, currentFold);
  folded      = rebuildManifold(curHead);
  currentFold = &folded.value();

  // From endpoint (d.from)
  if (!link.left.empty()) {
    curHead             = makeCell(curHead, link.left.front());
    const auto fromCell = cellRefOf(curHead);
    std::uint64_t at    = link.left.front().length;
    folded              = rebuildManifold(curHead);
    currentFold         = &folded.value();
    for (const auto &span : link.left | std::views::drop(1)) {
      curHead = spliceCellSpan(curHead, fromCell, at, 0, span, currentFold);
      at += span.length;
      folded      = rebuildManifold(curHead);
      currentFold = &folded.value();
    }
    curHead     = setLink(curHead, linkCell, dimFrom, zigzag::DimVector::POS,
                          fromCell, currentFold);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  // To endpoint (d.to)
  if (!link.right.empty()) {
    curHead           = makeCell(curHead, link.right.front());
    const auto toCell = cellRefOf(curHead);
    std::uint64_t at  = link.right.front().length;
    folded            = rebuildManifold(curHead);
    currentFold       = &folded.value();
    for (const auto &span : link.right | std::views::drop(1)) {
      curHead = spliceCellSpan(curHead, toCell, at, 0, span, currentFold);
      at += span.length;
      folded      = rebuildManifold(curHead);
      currentFold = &folded.value();
    }
    curHead = setLink(curHead, linkCell, dimTo, zigzag::DimVector::POS, toCell,
                      currentFold);
    folded  = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  // Link properties (d.linktype, d.linktier, d.owner, d.curator)
  auto linkProperty = [&](const std::string_view prop,
                          const zigzag::DimRef dim) {
    if (prop.empty()) {
      return;
    }
    curHead             = makeCell(curHead, prop);
    const auto propCell = cellRefOf(curHead);
    folded              = rebuildManifold(curHead);
    currentFold         = &folded.value();
    curHead = setLink(curHead, linkCell, dim, zigzag::DimVector::POS, propCell,
                      currentFold);
    folded  = rebuildManifold(curHead);
    currentFold = &folded.value();
  };

  linkProperty(linkTypeName(link.type), dimType);
  linkProperty(prominenceTierName(link.tier), dimTier);
  linkProperty(link.owner, dimOwner);
  linkProperty(link.curator, dimCurator);

  syncLinksFromRank(*currentFold);
  return curHead;
}

std::optional<FormatAttribute> Store::formatAttributeOf(const Link &link) {
  if (LinkType::Format != link.type || link.right.empty()) {
    return std::nullopt;
  }
  const auto &named = link.right.front();
  return firstOf(allFormatAttributes |
                 std::views::filter([&named](const FormatAttribute attribute) {
                   return named == vocabularySpanFor(attribute);
                 }));
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
  std::ranges::sort(found);
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
  if (opsSpool.empty()) {
    return MicroversionId{};
  }
  const auto ids =
      std::views::iota(1U, static_cast<std::uint32_t>(opsSpool.size() + 1)) |
      std::views::transform(
          [this](const std::uint32_t idx) { return opsSpool.idOf(idx); });
  return std::ranges::max(ids);
}

std::vector<MicroversionId> Store::structureHeads() const {
  if (zigzag::noCell == homeCell_ || opsSpool.empty()) {
    return {};
  }
  const auto hasHomeInAncestry = [this](const std::uint32_t idx) {
    auto curr = idx;
    while (curr > 0 && curr <= opsSpool.size()) {
      if (curr == homeCell_) {
        return true;
      }
      const auto *const node = opsSpool.get(curr);
      if (nullptr == node || 0 == node->parentIndex) {
        break;
      }
      curr = node->parentIndex;
    }
    return false;
  };

  auto leafHeads =
      std::views::iota(1U, static_cast<std::uint32_t>(opsSpool.size() + 1)) |
      std::views::filter([this](const std::uint32_t idx) {
        return opsSpool.childrenOf(idx).empty();
      }) |
      std::views::filter(hasHomeInAncestry) |
      std::views::transform(
          [this](const std::uint32_t idx) { return opsSpool.idOf(idx); });

  return std::ranges::to<std::vector<MicroversionId>>(leafHeads);
}

MicroversionId Store::structureHead() const {
  const auto heads = structureHeads();
  return heads.empty() ? MicroversionId{} : std::ranges::max(heads);
}

const std::vector<MicroversionId> &Store::currentVersions() const {
  if (currentVersions_.empty()) {
    const auto sHead  = structureHead();
    const auto target = !sHead.isZero() ? sHead : latest();
    if (!target.isZero()) {
      const auto manifold = rebuildManifold(target);
      const_cast<Store *>(this)->syncCurrentVersionsFromRank(manifold);
      if (currentVersions_.empty()) {
        currentVersionsFallback_ = {target};
        return currentVersionsFallback_;
      }
    }
  }
  return currentVersions_;
}

MicroversionId Store::primaryCurrentVersion() const {
  const auto &cur = currentVersions();
  return cur.empty() ? latest() : cur.front();
}

void Store::setCurrentVersions(std::vector<MicroversionId> versions) {
  currentVersions_            = std::move(versions);
  hasExplicitCurrentVersions_ = true;
}

void Store::repointCurrentVersion(const MicroversionId &version) {
  currentVersions_            = {version};
  hasExplicitCurrentVersions_ = true;
}

void Store::addCurrentVersion(const MicroversionId &version) {
  if (std::ranges::find(currentVersions_, version) == currentVersions_.end()) {
    currentVersions_.push_back(version);
    hasExplicitCurrentVersions_ = true;
  }
}

void Store::removeCurrentVersion(const MicroversionId &version) {
  std::erase(currentVersions_, version);
  hasExplicitCurrentVersions_ = true;
}

MicroversionId Store::designateEdition(const MicroversionId &parent,
                                       const std::string_view name,
                                       const MicroversionId &target,
                                       const zigzag::Manifold *const known,
                                       const bool allowDuplicateName) {
  const auto targetOp = opsSpool.indexOf(target);
  if (0 == targetOp) {
    throw std::invalid_argument("target version does not exist in store");
  }

  std::optional<zigzag::Manifold> folded;
  auto currentFold = known;
  if (nullptr == currentFold) {
    folded      = rebuildManifold(parent);
    currentFold = &folded.value();
  }

  auto curHead = parent;

  if (zigzag::noCell == homeCell_) {
    curHead     = sliceGenesis(curHead);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  } else if (!currentFold->contains(homeCell_)) {
    curHead     = structureHead();
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  auto ensureDim = [&](const std::string_view name) {
    auto dim = currentFold->dimensionNamed(name, *this);
    if (!dim) {
      const auto minted = makeDimension(curHead, name, currentFold);
      curHead           = minted.version;
      folded            = rebuildManifold(curHead);
      currentFold       = &folded.value();
      return minted.dim;
    }
    return *dim;
  };

  // 1. Ensure dimensions d.editions and d.edition-of exist
  const auto dimEditions  = ensureDim("d.editions");
  const auto dimEditionOf = ensureDim("d.edition-of");

  // 3. Mint or find the handle cell for targetOp
  zigzag::CellRef handleCell = zigzag::noCell;
  const auto existingHandle  = currentFold->findOpHandle(targetOp);
  if (existingHandle.has_value()) {
    handleCell = *existingHandle;
  } else {
    curHead     = makeOpHandle(curHead, targetOp);
    handleCell  = cellRefOf(curHead);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  // 4. Check if an edition cell named `name` already exists on d.editions rank
  zigzag::CellRef existingEditionCell = zigzag::noCell;
  if (!allowDuplicateName) {
    for (const auto cell :
         zigzag::rankAfter(*currentFold, homeCell_, dimEditions)) {
      if (currentFold->textOf(cell, *this) == name) {
        existingEditionCell = cell;
        break;
      }
    }
  }

  if (zigzag::noCell != existingEditionCell) {
    // Repoint existing edition cell to the new handle
    curHead = setLink(curHead, existingEditionCell, dimEditionOf,
                      zigzag::DimVector::POS, handleCell, currentFold);
  } else {
    // Mint new edition cell with name
    curHead                = makeCell(curHead, name);
    const auto editionCell = cellRefOf(curHead);
    folded                 = rebuildManifold(curHead);
    currentFold            = &folded.value();

    // Link onto tail of d.editions rank
    const auto tail = zigzag::rankTail(*currentFold, homeCell_, dimEditions,
                                       zigzag::DimVector::POS);
    curHead     = setLink(curHead, tail, dimEditions, zigzag::DimVector::POS,
                          editionCell, currentFold);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();

    // Link editionCell to handleCell on d.edition-of
    curHead = setLink(curHead, editionCell, dimEditionOf,
                      zigzag::DimVector::POS, handleCell, currentFold);
  }

  // Reconcile currentVersions_ cache and legacy aliasIndex_
  folded = rebuildManifold(curHead);
  syncCurrentVersionsFromRank(folded.value());
  if (name != "current") {
    aliasIndex_[std::string(name)]    = target;
    versionAnnotations_[target].alias = std::string(name);
  }

  return curHead;
}

void Store::syncCurrentVersionsFromRank(const zigzag::Manifold &manifold) {
  const auto eds = manifold.editions();
  if (eds.empty()) {
    return;
  }
  std::vector<MicroversionId> heads;
  heads.reserve(eds.size());
  for (const auto &ed : eds) {
    if (ed.name == "current" && ed.targetOp > 0) {
      const auto id = opsSpool.idOf(ed.targetOp);
      if (!id.isZero() && std::ranges::find(heads, id) == heads.end()) {
        heads.push_back(id);
      }
    }
  }
  if (heads.empty()) {
    for (const auto &ed : eds) {
      if (ed.targetOp > 0) {
        const auto id = opsSpool.idOf(ed.targetOp);
        if (!id.isZero() && std::ranges::find(heads, id) == heads.end()) {
          heads.push_back(id);
        }
      }
    }
  }
  if (!heads.empty()) {
    currentVersions_ = std::move(heads);
  }
}

void Store::syncAliasesFromRank(const zigzag::Manifold &manifold) {
  for (const auto &[name, targetOp] : manifold.aliases(*this)) {
    const auto id = opsSpool.idOf(targetOp);
    if (!id.isZero()) {
      aliasIndex_[name] = id;
      if (const auto ann = manifold.versionAnnotation(targetOp, *this)) {
        versionAnnotations_[id] = *ann;
      }
    }
  }
}

void Store::syncScrollsFromRank(const zigzag::Manifold &manifold) {
  scrollRegistry_ = manifold.scrollRegistry(*this);
  for (const auto &rec : scrollRegistry_.scrolls) {
    if (externals.size() < rec.id) {
      externals.resize(rec.id);
    }
    auto &sc = externals[rec.id - 1];
    if (rec.globalKey.starts_with("btpk:")) {
      const auto rest  = std::string_view(rec.globalKey).substr(5);
      const auto colon = rest.find(':');
      if (colon != std::string_view::npos) {
        try {
          sc.publisher = PublicKey::fromHex(rest.substr(0, colon));
          sc.salt      = std::string(rest.substr(colon + 1));
        } catch (...) {
        }
      }
    } else if (rec.globalKey.starts_with("file:")) {
      const auto rest  = std::string_view(rec.globalKey).substr(5);
      const auto colon = rest.find(':');
      if (colon != std::string_view::npos) {
        const auto hashHex = rest.substr(0, colon);
        const auto fidxStr = rest.substr(colon + 1);
        try {
          const auto fidx =
              static_cast<std::uint32_t>(std::stoul(std::string(fidxStr)));
          const auto hash = InfoHash::fromHex(hashHex);
          if (sc.segments.empty()) {
            bool foundInLocal = false;
            for (const auto &seg : localSegments.segments) {
              if (seg.torrent == hash && seg.fileIndex == fidx) {
                sc.segments.push_back(seg);
                foundInLocal = true;
                break;
              }
            }
            if (!foundInLocal) {
              ScrollSegment seg;
              seg.torrent   = hash;
              seg.fileIndex = fidx;
              sc.segments.push_back(std::move(seg));
            }
          }
        } catch (...) {
        }
      }
    }
    hydrateExternalScroll(sc);
  }
}

void Store::syncLinksFromRank(const zigzag::Manifold &manifold) {
  const auto mlinks = manifold.links(*this);
  linkTable.insert(mlinks.begin(), mlinks.end());
}

MicroversionId Store::registerScroll(const MicroversionId &parent,
                                     const std::string_view globalKey,
                                     const zigzag::Manifold *known) {
  if (globalKey.empty()) {
    return parent;
  }
  std::optional<zigzag::Manifold> folded;
  auto currentFold = known;
  if (nullptr == currentFold) {
    folded      = rebuildManifold(parent);
    currentFold = &folded.value();
  }

  auto curHead = parent;

  if (zigzag::noCell == homeCell_) {
    curHead     = sliceGenesis(curHead);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  } else if (!currentFold->contains(homeCell_)) {
    curHead     = structureHead();
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  auto ensureDim = [&](const std::string_view name) {
    auto dim = currentFold->dimensionNamed(name, *this);
    if (!dim) {
      const auto minted = makeDimension(curHead, name, currentFold);
      curHead           = minted.version;
      folded            = rebuildManifold(curHead);
      currentFold       = &folded.value();
      return minted.dim;
    }
    return *dim;
  };

  const auto dimScrolls = ensureDim("d.scrolls");

  for (const auto cell :
       zigzag::rankAfter(*currentFold, homeCell_, dimScrolls)) {
    if (currentFold->textOf(cell, *this) == globalKey) {
      return curHead;
    }
  }

  curHead               = makeCell(curHead, globalKey);
  const auto scrollCell = cellRefOf(curHead);
  folded                = rebuildManifold(curHead);
  currentFold           = &folded.value();

  const auto tail = zigzag::rankTail(*currentFold, homeCell_, dimScrolls,
                                     zigzag::DimVector::POS);
  curHead         = setLink(curHead, tail, dimScrolls, zigzag::DimVector::POS,
                            scrollCell, currentFold);
  folded          = rebuildManifold(curHead);
  currentFold     = &folded.value();

  syncScrollsFromRank(*currentFold);
  return curHead;
}

MicroversionId Store::linkScrollRef(const MicroversionId &parent,
                                    const zigzag::CellRef scrollCell,
                                    const zigzag::CellRef placeholderCell,
                                    const zigzag::Manifold *known) {
  if (zigzag::noCell == scrollCell || zigzag::noCell == placeholderCell) {
    return parent;
  }
  std::optional<zigzag::Manifold> folded;
  auto currentFold = known;
  if (nullptr == currentFold) {
    folded      = rebuildManifold(parent);
    currentFold = &folded.value();
  }

  auto curHead = parent;

  if (zigzag::noCell == homeCell_) {
    curHead     = sliceGenesis(curHead);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  } else if (!currentFold->contains(homeCell_)) {
    curHead     = structureHead();
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  auto ensureDim = [&](const std::string_view name) {
    auto dim = currentFold->dimensionNamed(name, *this);
    if (!dim) {
      const auto minted = makeDimension(curHead, name, currentFold);
      curHead           = minted.version;
      folded            = rebuildManifold(curHead);
      currentFold       = &folded.value();
      return minted.dim;
    }
    return *dim;
  };

  const auto dimScrollRefs = ensureDim("d.scroll-refs");
  const auto tail = zigzag::rankTail(*currentFold, scrollCell, dimScrollRefs,
                                     zigzag::DimVector::POS);

  curHead     = setLink(curHead, tail, dimScrollRefs, zigzag::DimVector::POS,
                        placeholderCell, currentFold);
  folded      = rebuildManifold(curHead);
  currentFold = &folded.value();

  syncScrollsFromRank(*currentFold);
  return curHead;
}

MicroversionId Store::makeExternRef(const MicroversionId &parent,
                                    const ExternOpRef &target,
                                    const zigzag::Manifold *const known) {
  std::optional<zigzag::Manifold> folded;
  auto currentFold = known;
  if (nullptr == currentFold) {
    folded      = rebuildManifold(parent);
    currentFold = &folded.value();
  }

  const auto registry  = currentFold->scrollRegistry(*this);
  const auto scrollRec = registry.findRecord(target.scroll);
  if (!scrollRec || zigzag::noCell == scrollRec->cell) {
    throw std::invalid_argument("scroll " + std::to_string(target.scroll) +
                                " is not registered in scroll registry");
  }

  if (const auto existing = registry.placeholderForExtern(target); existing) {
    return parent;
  }

  const auto descriptorText = target.produces.str();
  const auto span           = userPermascroll_->append(descriptorText);

  Op op;
  op.kind = OpKind::Structure;
  op.flags =
      structureFlags(StructureVerb::MakeCell, false, ValueKind::ExternRef);
  op.span  = span;
  op.value = 0;

  auto curHead           = apply(parent, op);
  const auto placeholder = cellRefOf(curHead);

  curHead = linkScrollRef(curHead, scrollRec->cell, placeholder, nullptr);
  return curHead;
}

std::optional<ExternOpRef>
Store::externTarget(const zigzag::CellRef placeholder,
                    const SpanReader &reader) const {
  if (zigzag::noCell == placeholder) {
    return std::nullopt;
  }
  const auto scrollIdOpt = scrollRegistry_.scrollIdForCell(placeholder);
  if (!scrollIdOpt.has_value()) {
    return std::nullopt;
  }

  if (const auto indexed =
          scrollRegistry_.findExternForPlaceholder(placeholder);
      indexed) {
    return *indexed;
  }

  const auto manifold = rebuildManifold(latest());
  const auto text     = manifold.textOf(placeholder, reader);
  if (text.empty()) {
    return std::nullopt;
  }

  try {
    const auto produces = MicroversionId::parse(text);
    return ExternOpRef{.scroll = *scrollIdOpt, .produces = produces};
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<ExternOpRef>
Store::externTarget(const zigzag::CellRef placeholder) const {
  return externTarget(placeholder, *this);
}

Store::AppendedPouchItem Store::appendPouchItemWithRef(
    const MicroversionId &parent, const zigzag::CellRef zone,
    const PrimediaSpan &content, const PouchOrigin &origin,
    const zigzag::Manifold *const known) {
  if (zigzag::noCell == zone) {
    throw std::invalid_argument("cannot append pouch item to noCell zone");
  }

  std::optional<zigzag::Manifold> folded;
  auto currentFold = known;
  if (nullptr == currentFold) {
    folded      = rebuildManifold(parent);
    currentFold = &folded.value();
  }

  auto curHead = parent;

  auto ensureDim = [&](const std::string_view name) -> zigzag::DimRef {
    auto dim = currentFold->dimensionNamed(name, *this);
    if (!dim) {
      const auto minted = makeDimension(curHead, name, currentFold);
      curHead           = minted.version;
      folded            = rebuildManifold(curHead);
      currentFold       = &folded.value();
      return minted.dim;
    }
    return *dim;
  };

  const auto dimItems = ensureDim("d.items");

  // Mint the item cell with content
  curHead             = makeCell(curHead, content);
  const auto itemCell = cellRefOf(curHead);
  folded              = rebuildManifold(curHead);
  currentFold         = &folded.value();

  // Append itemCell to zone's d.items rank posward
  const auto tail =
      zigzag::rankTail(*currentFold, zone, dimItems, zigzag::DimVector::POS);
  curHead = setLink(curHead, tail, dimItems, zigzag::DimVector::POS, itemCell,
                    currentFold);
  folded  = rebuildManifold(curHead);
  currentFold = &folded.value();

  // Add origin links if provided
  if (origin.cell && !origin.cell->scroll.empty()) {
    // Intern extern ref for foreign cell (§5.5)
    curHead     = registerScroll(curHead, origin.cell->scroll, currentFold);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();

    const auto scrollIdOpt =
        currentFold->scrollRegistry(*this).scrollIdForKey(origin.cell->scroll);
    if (scrollIdOpt) {
      const ExternOpRef extRef{
          .scroll   = *scrollIdOpt,
          .produces = origin.cell->produces,
      };
      curHead     = makeExternRef(curHead, extRef, currentFold);
      folded      = rebuildManifold(curHead);
      currentFold = &folded.value();

      const auto placeholder =
          currentFold->scrollRegistry(*this).placeholderForExtern(extRef);
      if (placeholder && *placeholder != zigzag::noCell) {
        const auto dimOriginCell = ensureDim("d.origin-cell");
        curHead = setLink(curHead, itemCell, dimOriginCell,
                          zigzag::DimVector::POS, *placeholder, currentFold);
        folded  = rebuildManifold(curHead);
        currentFold = &folded.value();
      }
    }
  }

  if (origin.document && (!origin.document->scroll.empty() ||
                          !origin.document->version.isZero())) {
    const auto descText = writeGlobalDocumentState(*origin.document);
    curHead             = makeCell(curHead, descText);
    const auto descCell = cellRefOf(curHead);
    folded              = rebuildManifold(curHead);
    currentFold         = &folded.value();

    const auto dimOriginState = ensureDim("d.origin-state");
    curHead = setLink(curHead, itemCell, dimOriginState, zigzag::DimVector::POS,
                      descCell, currentFold);
    folded  = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  return AppendedPouchItem{
      .version  = curHead,
      .itemCell = itemCell,
  };
}

MicroversionId Store::appendPouchItem(const MicroversionId &parent,
                                      const zigzag::CellRef zone,
                                      const PrimediaSpan &content,
                                      const PouchOrigin &origin,
                                      const zigzag::Manifold *const known) {
  return appendPouchItemWithRef(parent, zone, content, origin, known).version;
}

MicroversionId Store::dismissPouchItem(const MicroversionId &parent,
                                       const zigzag::CellRef zone,
                                       const zigzag::CellRef item,
                                       const zigzag::Manifold *const known) {
  if (zigzag::noCell == item) {
    return parent;
  }

  std::optional<zigzag::Manifold> folded;
  auto currentFold = known;
  if (nullptr == currentFold) {
    folded      = rebuildManifold(parent);
    currentFold = &folded.value();
  }

  const auto dimItemsOpt = currentFold->dimensionNamed("d.items", *this);
  if (!dimItemsOpt) {
    return parent;
  }
  const auto dimItems = *dimItemsOpt;

  auto curHead = parent;

  // Unlink item from d.items
  const auto prev = currentFold->linked(item, dimItems, zigzag::DimVector::NEG);
  const auto next = currentFold->linked(item, dimItems, zigzag::DimVector::POS);

  if (zigzag::noCell != prev) {
    curHead     = setLink(curHead, prev, dimItems, zigzag::DimVector::POS, next,
                          currentFold);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  } else if (zigzag::noCell != next) {
    curHead     = setLink(curHead, next, dimItems, zigzag::DimVector::NEG,
                          zigzag::noCell, currentFold);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  // Append item to d.dismissed rank
  auto ensureDim = [&](const std::string_view name) -> zigzag::DimRef {
    auto dim = currentFold->dimensionNamed(name, *this);
    if (!dim) {
      const auto minted = makeDimension(curHead, name, currentFold);
      curHead           = minted.version;
      folded            = rebuildManifold(curHead);
      currentFold       = &folded.value();
      return minted.dim;
    }
    return *dim;
  };

  const auto dimDismissed = ensureDim("d.dismissed");
  const auto anchor       = (zigzag::noCell != zone) ? zone : homeCell_;
  if (zigzag::noCell != anchor) {
    const auto tail = zigzag::rankTail(*currentFold, anchor, dimDismissed,
                                       zigzag::DimVector::POS);
    curHead = setLink(curHead, tail, dimDismissed, zigzag::DimVector::POS, item,
                      currentFold);
  }

  return curHead;
}

Store::AppendedAnthologyEntry Store::appendAnthologyEntry(
    const MicroversionId &parent, const zigzag::CellRef root,
    const ExternOpRef &memberRef, const GlobalDocumentState &pinnedState,
    const std::string_view label, const zigzag::Manifold *const known) {
  if (zigzag::noCell == root) {
    throw std::invalid_argument("cannot append anthology entry to noCell root");
  }

  // Validate ancestry (§5.9 §6.1, §3)
  if (memberRef.produces != pinnedState.version &&
      !memberRef.produces.isAncestorOf(pinnedState.version)) {
    throw std::invalid_argument("anthology member birth (" +
                                memberRef.produces.str() +
                                ") is not an ancestor of pinned state (" +
                                pinnedState.version.str() + ")");
  }

  std::optional<zigzag::Manifold> folded;
  auto currentFold = known;
  if (nullptr == currentFold) {
    folded      = rebuildManifold(parent);
    currentFold = &folded.value();
  }

  const auto registry  = currentFold->scrollRegistry(*this);
  const auto scrollRec = registry.findRecord(memberRef.scroll);
  if (!scrollRec || zigzag::noCell == scrollRec->cell) {
    throw std::invalid_argument("scroll " + std::to_string(memberRef.scroll) +
                                " is not registered in scroll registry");
  }

  GlobalDocumentState effectiveState = pinnedState;
  if (effectiveState.scroll.empty()) {
    effectiveState.scroll = scrollRec->globalKey;
  } else if (!scrollRec->globalKey.empty() &&
             effectiveState.scroll != scrollRec->globalKey) {
    throw std::invalid_argument("pinnedState scroll (" + effectiveState.scroll +
                                ") does not match registered scroll (" +
                                scrollRec->globalKey + ")");
  }

  auto curHead = parent;

  auto ensureDim = [&](const std::string_view name) -> zigzag::DimRef {
    auto dim = currentFold->dimensionNamed(name, *this);
    if (!dim) {
      const auto minted = makeDimension(curHead, name, currentFold);
      curHead           = minted.version;
      folded            = rebuildManifold(curHead);
      currentFold       = &folded.value();
      return minted.dim;
    }
    return *dim;
  };

  const auto dimAnthology   = ensureDim("d.anthology");
  const auto dimMember      = ensureDim("d.member");
  const auto dimMemberState = ensureDim("d.member-state");

  // 1. Intern placeholder for foreign cell (§5.5)
  curHead     = makeExternRef(curHead, memberRef, currentFold);
  folded      = rebuildManifold(curHead);
  currentFold = &folded.value();

  const auto placeholder =
      currentFold->scrollRegistry(*this).placeholderForExtern(memberRef);
  if (!placeholder || *placeholder == zigzag::noCell) {
    throw std::runtime_error("failed to intern placeholder for extern ref");
  }

  // 2. Mint the descriptor cell for pinnedState
  const auto descText  = writeGlobalDocumentState(effectiveState);
  curHead              = makeCell(curHead, descText);
  const auto stateCell = cellRefOf(curHead);
  folded               = rebuildManifold(curHead);
  currentFold          = &folded.value();

  // 3. Mint the entry cell
  curHead              = makeCell(curHead, label);
  const auto entryCell = cellRefOf(curHead);
  folded               = rebuildManifold(curHead);
  currentFold          = &folded.value();

  // 4. Link entryCell on d.member towards placeholder (or tail of placeholder's
  // negward rank)
  const auto memberTail = zigzag::rankTail(*currentFold, *placeholder,
                                           dimMember, zigzag::DimVector::NEG);
  curHead     = setLink(curHead, entryCell, dimMember, zigzag::DimVector::POS,
                        memberTail, currentFold);
  folded      = rebuildManifold(curHead);
  currentFold = &folded.value();

  // 5. Link entryCell on d.member-state to stateCell
  curHead = setLink(curHead, entryCell, dimMemberState, zigzag::DimVector::POS,
                    stateCell, currentFold);
  folded  = rebuildManifold(curHead);
  currentFold = &folded.value();

  // 6. Link entryCell onto root's d.anthology rank posward
  const auto tail = zigzag::rankTail(*currentFold, root, dimAnthology,
                                     zigzag::DimVector::POS);
  curHead         = setLink(curHead, tail, dimAnthology, zigzag::DimVector::POS,
                            entryCell, currentFold);
  folded          = rebuildManifold(curHead);
  currentFold     = &folded.value();

  return AppendedAnthologyEntry{
      .version         = curHead,
      .entryCell       = entryCell,
      .placeholderCell = *placeholder,
      .stateCell       = stateCell,
  };
}

MicroversionId Store::appendAnthologyLocalMember(
    const MicroversionId &parent, const zigzag::CellRef root,
    const zigzag::CellRef localCell, const zigzag::Manifold *const known) {
  if (zigzag::noCell == root) {
    throw std::invalid_argument("cannot append local member to noCell root");
  }
  if (zigzag::noCell == localCell) {
    throw std::invalid_argument("cannot append noCell as local member");
  }

  std::optional<zigzag::Manifold> folded;
  auto currentFold = known;
  if (nullptr == currentFold) {
    folded      = rebuildManifold(parent);
    currentFold = &folded.value();
  }

  auto curHead = parent;

  auto ensureDim = [&](const std::string_view name) -> zigzag::DimRef {
    auto dim = currentFold->dimensionNamed(name, *this);
    if (!dim) {
      const auto minted = makeDimension(curHead, name, currentFold);
      curHead           = minted.version;
      folded            = rebuildManifold(curHead);
      currentFold       = &folded.value();
      return minted.dim;
    }
    return *dim;
  };

  const auto dimAnthology = ensureDim("d.anthology");

  const auto tail = zigzag::rankTail(*currentFold, root, dimAnthology,
                                     zigzag::DimVector::POS);
  curHead         = setLink(curHead, tail, dimAnthology, zigzag::DimVector::POS,
                            localCell, currentFold);
  return curHead;
}

MicroversionId
Store::refreshAnthologyEntry(const MicroversionId &parent,
                             const zigzag::CellRef entryCell,
                             const GlobalDocumentState &newPinnedState,
                             const std::optional<ExternOpRef> optNewMemberRef,
                             const zigzag::Manifold *const known) {
  if (zigzag::noCell == entryCell) {
    throw std::invalid_argument("cannot refresh noCell entry");
  }

  std::optional<zigzag::Manifold> folded;
  auto currentFold = known;
  if (nullptr == currentFold) {
    folded      = rebuildManifold(parent);
    currentFold = &folded.value();
  }

  auto curHead = parent;

  auto ensureDim = [&](const std::string_view name) -> zigzag::DimRef {
    auto dim = currentFold->dimensionNamed(name, *this);
    if (!dim) {
      const auto minted = makeDimension(curHead, name, currentFold);
      curHead           = minted.version;
      folded            = rebuildManifold(curHead);
      currentFold       = &folded.value();
      return minted.dim;
    }
    return *dim;
  };

  const auto dimMember      = ensureDim("d.member");
  const auto dimMemberState = ensureDim("d.member-state");

  if (optNewMemberRef.has_value()) {
    const auto &memberRef = *optNewMemberRef;
    if (memberRef.produces != newPinnedState.version &&
        !memberRef.produces.isAncestorOf(newPinnedState.version)) {
      throw std::invalid_argument(
          "refreshed member birth is not an ancestor of new pinned state");
    }

    curHead     = makeExternRef(curHead, memberRef, currentFold);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();

    const auto placeholder =
        currentFold->scrollRegistry(*this).placeholderForExtern(memberRef);
    if (!placeholder || *placeholder == zigzag::noCell) {
      throw std::runtime_error(
          "failed to intern placeholder for refreshed extern ref");
    }

    curHead     = setLink(curHead, entryCell, dimMember, zigzag::DimVector::POS,
                          *placeholder, currentFold);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  } else {
    const auto placeholderOpt =
        zigzag::step(*currentFold, entryCell, dimMember);
    if (placeholderOpt.has_value()) {
      const auto extTarget = externTarget(*placeholderOpt);
      if (extTarget.has_value()) {
        if (extTarget->produces != newPinnedState.version &&
            !extTarget->produces.isAncestorOf(newPinnedState.version)) {
          throw std::invalid_argument(
              "existing member birth is not an ancestor of new pinned state");
        }
      }
    }
  }

  const auto descText     = writeGlobalDocumentState(newPinnedState);
  curHead                 = makeCell(curHead, descText);
  const auto newStateCell = cellRefOf(curHead);
  folded                  = rebuildManifold(curHead);
  currentFold             = &folded.value();

  curHead = setLink(curHead, entryCell, dimMemberState, zigzag::DimVector::POS,
                    newStateCell, currentFold);
  return curHead;
}

std::vector<Store::EditionInfo>
Store::editions(const MicroversionId &version) const {
  const auto manifold = rebuildManifold(version);
  const auto eds      = manifold.editions();
  std::vector<EditionInfo> result;
  result.reserve(eds.size());
  for (const auto &ed : eds) {
    result.push_back(EditionInfo{
        .cell     = ed.cell,
        .name     = ed.name,
        .handle   = ed.handle,
        .targetOp = ed.targetOp,
        .targetVersion =
            ed.targetOp > 0 ? opsSpool.idOf(ed.targetOp) : MicroversionId{},
    });
  }
  return result;
}

std::optional<Store::EditionInfo>
Store::editionNamed(const MicroversionId &version,
                    const std::string_view name) const {
  for (const auto &ed : editions(version)) {
    if (ed.name == name) {
      return ed;
    }
  }
  return std::nullopt;
}

MicroversionId Store::annotateVersion(const MicroversionId &parent,
                                      const MicroversionId &target,
                                      VersionAnnotation annotation,
                                      const zigzag::Manifold *const known) {
  const auto targetOp = opsSpool.indexOf(target);
  if (0 == targetOp) {
    throw std::invalid_argument("target version does not exist in store");
  }

  std::optional<zigzag::Manifold> folded;
  auto currentFold = known;
  if (nullptr == currentFold) {
    folded      = rebuildManifold(parent);
    currentFold = &folded.value();
  }

  auto curHead = parent;

  if (zigzag::noCell == homeCell_) {
    curHead     = sliceGenesis(curHead);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  } else if (!currentFold->contains(homeCell_)) {
    curHead     = structureHead();
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  auto ensureDim = [&](const std::string_view name) {
    auto dim = currentFold->dimensionNamed(name, *this);
    if (!dim) {
      const auto minted = makeDimension(curHead, name, currentFold);
      curHead           = minted.version;
      folded            = rebuildManifold(curHead);
      currentFold       = &folded.value();
      return minted.dim;
    }
    return *dim;
  };

  const auto dimNotes   = ensureDim("d.notes");
  const auto dimTag     = ensureDim("d.tag");
  const auto dimAlias   = ensureDim("d.alias");
  const auto dimCreated = ensureDim("d.created");

  // Mint or find OpHandle for targetOp
  zigzag::CellRef handleCell = zigzag::noCell;
  const auto existingHandle  = currentFold->findOpHandle(targetOp);
  if (existingHandle.has_value()) {
    handleCell = *existingHandle;
  } else {
    curHead     = makeOpHandle(curHead, targetOp);
    handleCell  = cellRefOf(curHead);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  if (!annotation.description.empty()) {
    curHead             = makeCell(curHead, annotation.description);
    const auto descCell = cellRefOf(curHead);
    folded              = rebuildManifold(curHead);
    currentFold         = &folded.value();
    curHead     = setLink(curHead, handleCell, dimNotes, zigzag::DimVector::POS,
                          descCell, currentFold);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  if (!annotation.tag.empty()) {
    curHead            = makeCell(curHead, annotation.tag);
    const auto tagCell = cellRefOf(curHead);
    folded             = rebuildManifold(curHead);
    currentFold        = &folded.value();
    curHead     = setLink(curHead, handleCell, dimTag, zigzag::DimVector::POS,
                          tagCell, currentFold);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  if (!annotation.alias.empty()) {
    curHead              = makeCell(curHead, annotation.alias);
    const auto aliasCell = cellRefOf(curHead);
    folded               = rebuildManifold(curHead);
    currentFold          = &folded.value();
    curHead     = setLink(curHead, handleCell, dimAlias, zigzag::DimVector::POS,
                          aliasCell, currentFold);
    folded      = rebuildManifold(curHead);
    currentFold = &folded.value();
    aliasIndex_[annotation.alias] = target;
  }

  if (!annotation.timestamp.empty()) {
    curHead             = makeCell(curHead, annotation.timestamp);
    const auto timeCell = cellRefOf(curHead);
    folded              = rebuildManifold(curHead);
    currentFold         = &folded.value();
    curHead = setLink(curHead, handleCell, dimCreated, zigzag::DimVector::POS,
                      timeCell, currentFold);
    folded  = rebuildManifold(curHead);
    currentFold = &folded.value();
  }

  versionAnnotations_[target] = annotation;
  return curHead;
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
  const auto targetOp = opsSpool.indexOf(id);
  if (0 == targetOp) {
    return std::nullopt;
  }
  const auto lat = latest();
  if (lat.isZero()) {
    return std::nullopt;
  }
  const auto manifold = rebuildManifold(lat);
  const auto ann      = manifold.versionAnnotation(targetOp, *this);
  if (ann.has_value()) {
    versionAnnotations_[id] = *ann;
    if (!ann->alias.empty()) {
      aliasIndex_[ann->alias] = id;
    }
  }
  return ann;
}

std::optional<MicroversionId>
Store::resolveAlias(const std::string_view alias) const {
  const auto it = aliasIndex_.find(std::string(alias));
  if (it != aliasIndex_.end()) {
    return it->second;
  }
  const auto lat = latest();
  if (!lat.isZero()) {
    const auto manifold = rebuildManifold(lat);
    const_cast<Store *>(this)->syncAliasesFromRank(manifold);
    const auto it2 = aliasIndex_.find(std::string(alias));
    if (it2 != aliasIndex_.end()) {
      return it2->second;
    }
  }
  if (!currentVersions_.empty()) {
    if (const auto ed = editionNamed(currentVersions_.front(), alias); ed) {
      return ed->targetVersion;
    }
  }
  return std::nullopt;
}

std::string Store::displayName(const MicroversionId &id) const {
  if (const auto ann = versionAnnotation(id); ann && !ann->alias.empty()) {
    return ann->alias;
  }
  const auto lat = latest();
  if (!lat.isZero()) {
    for (const auto &ed : editions(lat)) {
      if (ed.targetVersion == id && !ed.name.empty() && ed.name != "current") {
        return ed.name;
      }
    }
  }
  if (!currentVersions_.empty()) {
    for (const auto &ed : editions(currentVersions_.front())) {
      if (ed.targetVersion == id && !ed.name.empty() && ed.name != "current") {
        return ed.name;
      }
    }
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
    MicroversionId dimId{};
    MicroversionId targetId{};
    MicroversionId valTargetId{};
    if (OpKind::Structure == node->kind) {
      const auto verb = structureVerbOf(node->flags);
      if (StructureVerb::SetLink == verb) {
        if (node->linkId > 0) {
          dimId = opsSpool.idOf(node->linkId);
        }
        if (node->to > 0) {
          targetId = opsSpool.idOf(node->to);
        }
      }
      if (ValueKind::OpHandle == valueKindOf(node->flags)) {
        if (node->value > 0) {
          valTargetId = opsSpool.idOf(static_cast<std::uint32_t>(node->value));
        }
      }
    }
    records.push_back(
        OpRecord{.produces = opsSpool.idOf(idx),
                 .op       = node->toOp(opsSpool.idOf(node->parentIndex),
                                        opsSpool.idOf(node->sourceOpIndex)),
                 .structureDimension   = dimId,
                 .structureTarget      = targetId,
                 .structureValueTarget = valTargetId});
  }
  std::ranges::sort(records, [](const OpRecord &lhs, const OpRecord &rhs) {
    return lhs.produces < rhs.produces;
  });
  return records;
}

void Store::adoptOpRecords(const std::vector<OpRecord> &records) {
  std::vector<OpRecord> pending;
  std::set<MicroversionId> seen;
  for (const auto &record : records) {
    if (record.produces.isZero() || opsSpool.contains(record.produces)) {
      continue;
    }
    if (seen.insert(record.produces).second) {
      pending.push_back(record);
    }
  }

  if (!pending.empty()) {
    auto getDeps = [](const OpRecord &rec) {
      std::vector<MicroversionId> deps;
      if (!rec.op.parent.isZero()) {
        deps.push_back(rec.op.parent);
      }
      if (!rec.op.source.isZero()) {
        deps.push_back(rec.op.source);
      }
      if (OpKind::Structure == rec.op.kind) {
        const auto verb = structureVerbOf(rec.op.flags);
        if (StructureVerb::SetLink == verb) {
          if (!rec.structureDimension.isZero()) {
            deps.push_back(rec.structureDimension);
          }
          if (!rec.structureTarget.isZero()) {
            deps.push_back(rec.structureTarget);
          }
        }
        if (ValueKind::OpHandle == valueKindOf(rec.op.flags)) {
          if (!rec.structureValueTarget.isZero()) {
            deps.push_back(rec.structureValueTarget);
          }
        }
      }
      return deps;
    };

    std::map<MicroversionId, std::size_t> pendingIndices;
    for (std::size_t i = 0; i < pending.size(); ++i) {
      pendingIndices.emplace(pending[i].produces, i);
    }

    std::vector<std::size_t> inDegree(pending.size(), 0);
    std::vector<std::vector<std::size_t>> dependents(pending.size());

    for (std::size_t i = 0; i < pending.size(); ++i) {
      auto deps = getDeps(pending[i]);
      std::ranges::sort(deps);
      const auto [first, last] = std::ranges::unique(deps);
      deps.erase(first, last);

      for (const auto &dep : deps) {
        if (opsSpool.contains(dep)) {
          continue;
        }
        const auto it = pendingIndices.find(dep);
        if (it == pendingIndices.end()) {
          throw std::invalid_argument(
              "operation filed under " + pending[i].produces.str() +
              " has unresolved dependency " + dep.str());
        }
        dependents[it->second].push_back(i);
        inDegree[i]++;
      }
    }

    auto cmp = [&](std::size_t lhs, std::size_t rhs) {
      return pending[lhs].produces > pending[rhs].produces;
    };
    std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(cmp)>
        ready(cmp);

    for (std::size_t i = 0; i < pending.size(); ++i) {
      if (0 == inDegree[i]) {
        ready.push(i);
      }
    }

    std::vector<std::size_t> scheduled;
    scheduled.reserve(pending.size());

    while (!ready.empty()) {
      const auto u = ready.top();
      ready.pop();
      scheduled.push_back(u);

      for (const auto v : dependents[u]) {
        if (--inDegree[v] == 0) {
          ready.push(v);
        }
      }
    }

    if (scheduled.size() < pending.size()) {
      throw std::invalid_argument("cyclic dependency among operations");
    }

    for (const auto idx : scheduled) {
      const auto &record = pending[idx];
      Op op              = record.op;

      if (OpKind::Structure == op.kind) {
        const auto verb = structureVerbOf(op.flags);
        if (StructureVerb::SetLink == verb) {
          if (!record.structureDimension.isZero()) {
            const auto dimIdx = opsSpool.indexOf(record.structureDimension);
            if (0 == dimIdx) {
              throw std::invalid_argument("unresolved SetLink dimension " +
                                          record.structureDimension.str());
            }
            op.link = dimIdx;
          }
          if (!record.structureTarget.isZero()) {
            const auto targetIdx = opsSpool.indexOf(record.structureTarget);
            if (0 == targetIdx) {
              throw std::invalid_argument("unresolved SetLink target " +
                                          record.structureTarget.str());
            }
            op.to = targetIdx;
          } else if (!record.structureDimension.isZero()) {
            op.to = 0;
          }
        }

        if (ValueKind::OpHandle == valueKindOf(op.flags)) {
          if (!record.structureValueTarget.isZero()) {
            const auto valTargetIdx =
                opsSpool.indexOf(record.structureValueTarget);
            if (0 == valTargetIdx) {
              throw std::invalid_argument("unresolved OpHandle target " +
                                          record.structureValueTarget.str());
            }
            op.value = valTargetIdx;
          }
        }
      }

      putOp(record.produces, op);
    }
  }

  indexGenesisCells();
  const auto sHeads = structureHeads();
  if (!sHeads.empty()) {
    for (const auto &sh : sHeads) {
      const auto manifold = rebuildManifold(sh);
      syncCurrentVersionsFromRank(manifold);
      syncAliasesFromRank(manifold);
      syncScrollsFromRank(manifold);
      syncLinksFromRank(manifold);
    }
  } else if (!latest().isZero()) {
    const auto manifold = rebuildManifold(latest());
    syncCurrentVersionsFromRank(manifold);
    syncAliasesFromRank(manifold);
    syncScrollsFromRank(manifold);
    syncLinksFromRank(manifold);
  }
}

void Store::sealPendingMetadataAsCells() const {
  if (latest().isZero()) {
    return;
  }
  if (versionAnnotations_.empty() && !hasExplicitCurrentVersions_ &&
      (zigzag::noCell == homeCell_ || externals.empty())) {
    return;
  }
  auto *mutableStore = const_cast<Store *>(this);
  auto curHead = (zigzag::noCell != homeCell_) ? structureHead() : latest();
  if (curHead.isZero()) {
    curHead = latest();
  }
  auto manifold = rebuildManifold(curHead);

  const auto pendingAnnotations = versionAnnotations_;
  for (const auto &[id, ann] : pendingAnnotations) {
    const auto targetOp = opsSpool.indexOf(id);
    if (targetOp > 0) {
      const auto existing = manifold.versionAnnotation(targetOp, *this);
      if (!existing.has_value() || existing->description != ann.description ||
          existing->tag != ann.tag || existing->alias != ann.alias ||
          existing->timestamp != ann.timestamp) {
        curHead  = mutableStore->annotateVersion(curHead, id, ann, &manifold);
        manifold = rebuildManifold(curHead);
      }
    }
  }

  // Only seal currentVersions_ as "current" editions if explicitly set
  // (§5.3 / §5.4).
  if (hasExplicitCurrentVersions_ && !currentVersions_.empty()) {
    const auto existingEds = manifold.editions();
    if (existingEds.empty()) {
      const auto headsToSeal = currentVersions_;
      for (std::size_t i = 0; i < headsToSeal.size(); ++i) {
        if (opsSpool.indexOf(headsToSeal[i]) > 0) {
          curHead = mutableStore->designateEdition(
              curHead, "current", headsToSeal[i], &manifold,
              /*allowDuplicateName=*/(i > 0));
          manifold = rebuildManifold(curHead);
        }
      }
    }
    mutableStore->hasExplicitCurrentVersions_ = false;
  }

  // Only seal externals on d.scrolls if this store is a slice (homeCell_ !=
  // noCell)
  if (zigzag::noCell != homeCell_ && !externals.empty()) {
    const auto reg = manifold.scrollRegistry(*this);
    for (const auto &sc : externals) {
      const auto key = scrollKey(sc);
      if (!key.empty() && !reg.byKey.contains(key)) {
        curHead  = mutableStore->registerScroll(curHead, key, &manifold);
        manifold = rebuildManifold(curHead);
      }
    }
  }
}

void Store::save(const std::string &directory) const {
  sealPendingMetadataAsCells();
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
    std::vector<ScrollSegment> deploymentSegments = localSegments.segments;
    for (const auto &sc : externals) {
      for (const auto &seg : sc.segments) {
        if (!seg.path.empty() || !seg.torrent.isZero()) {
          bool already = false;
          for (const auto &existing : deploymentSegments) {
            if (existing == seg) {
              already = true;
              break;
            }
          }
          if (!already) {
            deploymentSegments.push_back(seg);
          }
        }
      }
    }
    writeStoreTables(dir / storeTablesName,
                     StoreTables{.documentId    = documentId_,
                                 .localSegments = deploymentSegments});
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
  sealPendingMetadataAsCells();
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
    std::vector<ScrollSegment> deploymentSegments = localSegments.segments;
    for (const auto &sc : externals) {
      for (const auto &seg : sc.segments) {
        if (!seg.path.empty() || !seg.torrent.isZero()) {
          bool already = false;
          for (const auto &existing : deploymentSegments) {
            if (existing == seg) {
              already = true;
              break;
            }
          }
          if (!already) {
            deploymentSegments.push_back(seg);
          }
        }
      }
    }
    writeStoreTables(dir / storeTablesName,
                     StoreTables{.documentId    = documentId_,
                                 .localSegments = deploymentSegments});
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
  scrollRegistry_ = {};
  localSegments   = Scroll{};
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
    documentId_            = tables.documentId;
    localSegments          = Scroll{};
    localSegments.segments = std::move(tables.localSegments);
  }

  // The nodes above arrived as a mapped segment rather than through putOp(),
  // so which operations minted the genesis cells has to be read back out of
  // them. Cheap next to the load itself, and it is the whole reason the two
  // refs are derived rather than written into the side tables: a store's own
  // operations already say what they are.
  indexGenesisCells();
  const auto sHeads = structureHeads();
  if (!sHeads.empty()) {
    for (const auto &sh : sHeads) {
      const auto manifold = rebuildManifold(sh);
      syncCurrentVersionsFromRank(manifold);
      syncAliasesFromRank(manifold);
      syncScrollsFromRank(manifold);
      syncLinksFromRank(manifold);
    }
  } else if (!latest().isZero()) {
    const auto manifold = rebuildManifold(latest());
    syncCurrentVersionsFromRank(manifold);
    syncAliasesFromRank(manifold);
    syncScrollsFromRank(manifold);
    syncLinksFromRank(manifold);
  }
  if (zigzag::noCell == homeCell_) {
    for (const auto &seg : localSegments.segments) {
      if (!seg.torrent.isZero()) {
        bool found = false;
        for (auto &sc : externals) {
          for (const auto &s : sc.segments) {
            if (s.torrent == seg.torrent && s.fileIndex == seg.fileIndex) {
              found = true;
              break;
            }
          }
          if (found) {
            break;
          }
        }
        if (!found) {
          Scroll sc;
          sc.segments.push_back(seg);
          externals.push_back(std::move(sc));
        }
      }
    }
  }
  if (chronofilade_) {
    chronofilade_->clear();
    chronofilade_->indexSpool(*this);
  }
}

} // namespace xanadu
