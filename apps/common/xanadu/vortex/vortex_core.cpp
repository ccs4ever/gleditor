#include "common/xanadu/vortex/vortex_core.hpp"

#include <bit>
#include <cctype>
#include <iostream>
#include <ranges>
#include <utility>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace zigzag::vortex {

VortexCore::VortexCore(ArenaManifold &arena) : arena_(arena) { initGenesis(); }

CellRef VortexCore::mintNamedDimension(std::string_view name,
                                       CellRef &lastDimCell) {
  if (const auto existing = baseDimensionNamed(name)) {
    lastDimCell = *existing;
    return *existing;
  }
  CellRef dimCell = arena_.makeCell(name);
  if (lastDimCell != noCell && dims_.dims != noCell) {
    zigzag::expectWritten(
        arena_.link(lastDimCell, dims_.dims, DimVector::POS, dimCell));
  }
  lastDimCell = dimCell;
  return dimCell;
}

void VortexCore::initGenesis() {
  if (arena_.base() && arena_.base()->home() != noCell) {
    home_ = arena_.base()->home();
  } else {
    home_ = arena_.makeCell("home");
  }
  CellRef lastDim = noCell;

  // System Dimension Genesis off home_
  if (arena_.base() && arena_.base()->dimsDimension() != noCell) {
    dims_.dims = arena_.base()->dimsDimension();
    lastDim    = dims_.dims;
  } else {
    dims_.dims = mintNamedDimension("d.dims", lastDim);
    zigzag::expectWritten(
        arena_.link(home_, dims_.dims, DimVector::POS, dims_.dims));
  }

  dims_.grab           = mintNamedDimension("d.grab", lastDim);
  dims_.step           = mintNamedDimension("d.step", lastDim);
  dims_.spin           = mintNamedDimension("d.spin", lastDim);
  dims_.stack          = mintNamedDimension("d.stack", lastDim);
  dims_.contract       = mintNamedDimension("d.contract", lastDim);
  dims_.clone          = mintNamedDimension("d.clone", lastDim);
  dims_.cursors        = mintNamedDimension("d.cursors", lastDim);
  dims_.cache          = mintNamedDimension("d.cache", lastDim);
  dims_.vars           = mintNamedDimension("d.vars", lastDim);
  dims_.values         = mintNamedDimension("d.values", lastDim);
  dims_.pinningCursors = mintNamedDimension("d.pinning-cursors", lastDim);
  dims_.name           = mintNamedDimension("d.name", lastDim);
  dims_.stdlib         = mintNamedDimension("d.stdlib", lastDim);
  dims_.clause         = mintNamedDimension("d.clause", lastDim);
  dims_.stores         = mintNamedDimension("d.stores", lastDim);
  dims_.role           = mintNamedDimension("d.role", lastDim);
}

CellRef VortexCore::mintDimension(std::string_view name) {
  if (const auto existing = baseDimensionNamed(name)) {
    return *existing;
  }
  auto named = zigzag::rankAfter(arena_, home_, dims_.dims) |
               std::views::filter([&](const DimRef dim) {
                 return arena_.textOf(dim) == name;
               });
  if (const auto found = zigzag::firstOf(named)) {
    return *found;
  }
  CellRef tail = zigzag::rankTail(arena_, home_, dims_.dims);
  return mintNamedDimension(name, tail);
}

DimRef VortexCore::findOrMintDimension(const std::string_view name) {
  const auto minted =
      zigzag::firstOf(zigzag::rank(arena_, dims_.dims, dims_.dims) |
                      std::views::filter([&](const DimRef dim) {
                        return arena_.textOf(dim) == name;
                      }));
  return minted ? *minted : mintDimension(name);
}

std::optional<CellRef>
VortexCore::findModule(const std::string_view modulePath) const {
  return zigzag::firstOf(zigzag::rankAfter(arena_, home_, dims_.stdlib) |
                         std::views::filter([&](const CellRef mod) {
                           return arena_.textOf(mod) == modulePath;
                         }));
}

CellRef VortexCore::getOrCreateModule(const std::string_view modulePath) {
  if (const auto existing = findModule(modulePath)) {
    return *existing;
  }
  const CellRef modCell = arena_.makeCell(modulePath);
  zigzag::expectWritten(
      arena_.link(zigzag::rankTail(arena_, home_, dims_.stdlib), dims_.stdlib,
                  Posward, modCell));
  return modCell;
}

void VortexCore::exportSymbol(const CellRef moduleCell,
                              const std::string_view symbolName,
                              const CellRef entryOp) {
  const CellRef symCell = arena_.makeCell(symbolName);
  zigzag::expectWritten(arena_.link(symCell, dims_.values, Posward, entryOp));
  zigzag::expectWritten(
      arena_.link(zigzag::rankTail(arena_, moduleCell, dims_.vars), dims_.vars,
                  Posward, symCell));
}

std::optional<CellRef> VortexCore::link(CellRef cell, DimRef dim, DimVector dir,
                                        std::optional<CellRef> target) {
  if (!arena_.contains(cell)) {
    return std::nullopt;
  }

  // 1. Read form (target omitted)
  if (!target.has_value()) {
    return zigzag::step(arena_, cell, dim, dir);
  }

  const CellRef raw_target = *target;

  // 2. Allocation form (target == -1)
  // raw_target is already CellRef (unsigned); std::cmp_equal(raw_target, -1)
  // -- this check's own -fix -- compares against a *signed* -1 instead and
  // can never be true for an unsigned raw_target, silently disabling this
  // allocation form. See the modernize-use-integer-sign-comparison note in
  // .clang-tidy.
  // NOLINTNEXTLINE(modernize-use-integer-sign-comparison)
  if (raw_target == static_cast<CellRef>(-1)) {
    CellRef created = arena_.makeCell();
    zigzag::expectWritten(arena_.link(cell, dim, dir, created));
    return created;
  }

  // 3. Isolation form (target == 0 / noCell)
  if (raw_target == 0 || raw_target == noCell) {
    CellRef old = arena_.linked(cell, dim, dir);
    if (old == noCell) {
      return std::nullopt;
    }
    zigzag::expectWritten(arena_.link(cell, dim, dir, noCell));
    return old;
  }

  // 4. Literal target
  zigzag::expectWritten(arena_.link(cell, dim, dir, raw_target));
  return raw_target;
}

std::optional<CellRef> VortexCore::value(CellRef cell, std::int64_t offset,
                                         std::int64_t length,
                                         std::optional<CellValue> replacement) {
  if (!arena_.contains(cell)) {
    return std::nullopt;
  }

  // All reads and writes resolve through the clone master
  const CellRef master = masterOf(cell);

  // Write Branch
  if (replacement.has_value()) {
    const auto &repl = *replacement;
    if (std::holds_alternative<double>(repl)) {
      zigzag::expectWritten(arena_.setValueBits(
          master, xanadu::ValueKind::Double,
          xanadu::canonicalDoubleBits(std::get<double>(repl))));
      zigzag::expectWritten(arena_.setContent(master, {}));
    } else if (std::holds_alternative<std::int64_t>(repl)) {
      zigzag::expectWritten(arena_.setValueBits(
          master, xanadu::ValueKind::Int64,
          std::bit_cast<std::uint64_t>(std::get<std::int64_t>(repl))));
      zigzag::expectWritten(arena_.setContent(master, {}));
    } else if (std::holds_alternative<bool>(repl)) {
      zigzag::expectWritten(arena_.setValueBits(master, xanadu::ValueKind::Bool,
                                                std::get<bool>(repl) ? 1 : 0));
      zigzag::expectWritten(arena_.setContent(master, {}));
    } else {
      // String Splice
      const auto &replStr      = std::get<std::string>(repl);
      std::string current      = arena_.textOf(master);
      const auto [from, count] = resolve_range(current.size(), offset, length);
      current.replace(from, count, replStr);
      const auto span = arena_.intern(current);
      zigzag::expectWritten(
          arena_.setValueBits(master, xanadu::ValueKind::None, 0));
      zigzag::expectWritten(arena_.setContent(
          master, std::span<const xanadu::PrimediaSpan>{&span, 1}));
    }
    return cell; // Return cell so path expressions carry through
  }

  // Read Whole Content
  if (offset == 0 && length < 0) {
    return cell;
  }

  // Read Slice (ephemeral transclusion)
  std::string current      = arena_.textOf(master);
  const auto [from, count] = resolve_range(current.size(), offset, length);
  std::string sliceText    = current.substr(from, count);
  CellRef sliceCell        = arena_.makeCell(sliceText);
  return sliceCell;
}

std::optional<CellRef> VortexCore::newCell(CellRef cell, DimRef dim,
                                           DimVector dir) {
  return link(cell, dim, dir, static_cast<CellRef>(-1));
}

std::optional<CellRef> VortexCore::newCell(CellRef cell, DimRef dim,
                                           DimVector dir,
                                           const CellValue &val) {
  auto created = newCell(cell, dim, dir);
  if (created) {
    value(*created, 0, -1, val);
  }
  return created;
}

std::optional<CellRef> VortexCore::breakLink(CellRef cell, DimRef dim,
                                             DimVector dir) {
  return link(cell, dim, dir, noCell);
}

std::optional<CellRef> VortexCore::splice(CellRef cell, std::int64_t offset,
                                          std::int64_t length,
                                          const CellValue &val) {
  return value(cell, offset, length, val);
}

std::optional<CellRef> VortexCore::insert(CellRef cell, std::int64_t offset,
                                          const CellValue &val) {
  return value(cell, offset, 0, val);
}

std::optional<CellRef> VortexCore::append(CellRef cell, const CellValue &val) {
  const auto cur   = render(cell);
  std::int64_t end = 0;
  if (std::holds_alternative<std::string>(cur)) {
    end = static_cast<std::int64_t>(std::get<std::string>(cur).size());
  }
  return value(cell, end, 0, val);
}

std::optional<CellRef> VortexCore::erase(CellRef cell, std::int64_t offset,
                                         std::int64_t length) {
  return value(cell, offset, length, CellValue(std::string{}));
}

CellValue VortexCore::get(CellRef cell, std::int64_t offset,
                          std::int64_t length) {
  if (offset == 0 && length < 0) {
    return render(cell);
  }
  const auto slice = value(cell, offset, length);
  if (!slice) {
    return false;
  }
  return render(*slice);
}

CellValue VortexCore::render(CellRef cell) const {
  if (!arena_.contains(cell)) {
    return false;
  }
  const CellRef master = masterOf(cell);
  const auto kind      = arena_.valueKindOf(master);
  if (kind == xanadu::ValueKind::Double) {
    auto d = arena_.asDouble(master);
    if (d) return *d;
  } else if (kind == xanadu::ValueKind::Int64) {
    auto i = arena_.asInt64(master);
    if (i) return *i;
  } else if (kind == xanadu::ValueKind::Bool) {
    auto b = arena_.asBool(master);
    if (b) return *b;
  }
  return arena_.textOf(master);
}

bool VortexCore::isTruthy(CellRef cell) const {
  return evaluateTruthiness(render(cell));
}

bool VortexCore::evaluateTruthiness(const CellValue &val) {
  if (std::holds_alternative<bool>(val)) {
    return std::get<bool>(val);
  }
  if (std::holds_alternative<double>(val)) {
    return std::get<double>(val) != 0.0;
  }
  if (std::holds_alternative<std::int64_t>(val)) {
    return std::get<std::int64_t>(val) != 0;
  }
  const auto &s     = std::get<std::string>(val);
  std::string lower = s;
  for (char &c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
    return true;
  }
  if (lower.empty() || lower == "0" || lower == "false" || lower == "no" ||
      lower == "off") {
    return false;
  }
  return true;
}

std::function<CellRef()> VortexCore::cloneGenerator(CellRef source) {
  return [this, source]() -> CellRef {
    const CellRef fresh = arena_.makeCell();
    zigzag::expectWritten(
        arena_.link(zigzag::rankTail(arena_, source, dims_.clone), dims_.clone,
                    DimVector::POS, fresh));
    return fresh;
  };
}

CellRef VortexCore::masterOf(const CellRef cell) const noexcept {
  return arena_.cloneMaster(cell, dims_.clone).value_or(cell);
}

std::optional<DimRef>
VortexCore::baseDimensionNamed(const std::string_view name) const {
  return nullptr == arena_.base() ? std::nullopt
                                  : arena_.base()->dimensionNamed(name);
}

std::vector<CellRef> VortexCore::wingOf(const CellRef opcode,
                                        const DimVector side) const {
  // A wing slot holds either an operand reference -- an Int64 naming a cell
  // the arena can reach -- or is itself the operand, read through its master.
  const auto operand = [this](const CellRef slot) {
    return arena_.asInt64(slot)
        .transform([](const std::int64_t v) { return static_cast<CellRef>(v); })
        .and_then([this](const CellRef ref) {
          return arena_.contains(ref) ? std::optional{ref} : std::nullopt;
        })
        .value_or(masterOf(slot));
  };
  return zigzag::rank(arena_, arena_.linked(opcode, dims_.grab, side),
                      dims_.step) |
         std::views::transform(operand) | std::ranges::to<std::vector>();
}

std::vector<CellRef> VortexCore::inputsOf(CellRef opcode) const {
  return wingOf(opcode, DimVector::POS);
}

std::vector<CellRef> VortexCore::outputsOf(CellRef opcode) const {
  return wingOf(opcode, DimVector::NEG);
}

void VortexCore::bindInput(CellRef opcode, CellRef operand) {
  if (!arena_.contains(opcode) || !arena_.contains(operand)) {
    return;
  }
  CellRef slot = arena_.makeScalarCell(static_cast<std::int64_t>(operand));

  // Link slot into opcode's input wing (+d.grab, then chain on +d.step)
  CellRef first = arena_.linked(opcode, dims_.grab, DimVector::POS);
  if (first == noCell) {
    zigzag::expectWritten(
        arena_.link(opcode, dims_.grab, DimVector::POS, slot));
    return;
  }
  zigzag::expectWritten(arena_.link(zigzag::rankTail(arena_, first, dims_.step),
                                    dims_.step, DimVector::POS, slot));
}

void VortexCore::bindOutput(CellRef opcode, CellRef target) {
  if (!arena_.contains(opcode) || !arena_.contains(target)) {
    return;
  }
  CellRef slot = arena_.makeScalarCell(static_cast<std::int64_t>(target));

  // Link slot into opcode's output wing (-d.grab, then chain on +d.step)
  CellRef first = arena_.linked(opcode, dims_.grab, DimVector::NEG);
  if (first == noCell) {
    zigzag::expectWritten(
        arena_.link(opcode, dims_.grab, DimVector::NEG, slot));
    return;
  }
  zigzag::expectWritten(arena_.link(zigzag::rankTail(arena_, first, dims_.step),
                                    dims_.step, DimVector::POS, slot));
}

bool VortexCore::hasPipeline(CellRef paramCell) const {
  if (paramCell == home_ || paramCell == noCell) {
    return false;
  }
  // A cell inside an instruction stream has a predecessor along -d.spin;
  // a parameter cell heading a pipeline has only an outgoing +d.spin.
  if (arena_.linked(paramCell, dims_.spin, DimVector::NEG) != noCell) {
    return false;
  }
  return arena_.linked(paramCell, dims_.spin, DimVector::POS) != noCell;
}

CellRef VortexCore::getPipelineHead(CellRef paramCell) const {
  if (paramCell == home_ || paramCell == noCell) {
    return noCell;
  }
  if (arena_.linked(paramCell, dims_.spin, DimVector::NEG) != noCell) {
    return noCell;
  }
  return arena_.linked(paramCell, dims_.spin, DimVector::POS);
}

void VortexCore::attachPipeline(CellRef paramCell, CellRef firstOp) {
  zigzag::expectWritten(
      arena_.link(paramCell, dims_.spin, DimVector::POS, firstOp));
}

std::vector<CellRef> VortexCore::contractOf(const CellRef opcode,
                                            const DimVector side) const {
  return zigzag::rank(arena_, arena_.linked(opcode, dims_.contract, side),
                      dims_.step) |
         std::ranges::to<std::vector>();
}

void VortexCore::attachContract(const CellRef opcode, const DimVector side,
                                const CellRef conditionOp) {
  const CellRef first = arena_.linked(opcode, dims_.contract, side);
  if (first == noCell) {
    zigzag::expectWritten(
        arena_.link(opcode, dims_.contract, side, conditionOp));
    return;
  }
  zigzag::expectWritten(arena_.link(zigzag::rankTail(arena_, first, dims_.step),
                                    dims_.step, DimVector::POS, conditionOp));
}

std::vector<CellRef> VortexCore::preconditionsOf(CellRef opcode) const {
  return contractOf(opcode, DimVector::NEG);
}

std::vector<CellRef> VortexCore::postconditionsOf(CellRef opcode) const {
  return contractOf(opcode, DimVector::POS);
}

void VortexCore::attachPrecondition(CellRef opcode, CellRef conditionOp) {
  attachContract(opcode, DimVector::NEG, conditionOp);
}

void VortexCore::attachPostcondition(CellRef opcode, CellRef conditionOp) {
  attachContract(opcode, DimVector::POS, conditionOp);
}

CellRef VortexCore::pin(std::string_view name, CellRef targetNode) {
  CellRef pinCursor = arena_.makeCell();
  CellRef nameCell  = arena_.makeCell(name);
  zigzag::expectWritten(
      arena_.link(pinCursor, dims_.name, DimVector::POS, nameCell));
  if (targetNode != noCell) {
    zigzag::expectWritten(
        arena_.link(pinCursor, dims_.cache, DimVector::POS, targetNode));
  }

  // Onto the tail of home_'s +d.pinning-cursors rank.
  zigzag::expectWritten(
      arena_.link(zigzag::rankTail(arena_, home_, dims_.pinningCursors),
                  dims_.pinningCursors, DimVector::POS, pinCursor));
  return pinCursor;
}

std::optional<CellRef> VortexCore::findPin(std::string_view name) const {
  const auto named = [&](const CellRef pin) {
    return zigzag::step(arena_, pin, dims_.name)
        .transform(
            [&](const CellRef label) { return arena_.textOf(label) == name; })
        .value_or(false);
  };
  return zigzag::firstOf(
      zigzag::rankAfter(arena_, home_, dims_.pinningCursors) |
      std::views::filter(named));
}

bool VortexCore::flushPin(std::string_view name) {
  auto pinOpt = findPin(name);
  if (!pinOpt) {
    return false;
  }
  zigzag::expectWritten(
      arena_.link(*pinOpt, dims_.cache, DimVector::POS, noCell));
  return true;
}

bool VortexCore::retirePin(std::string_view name) {
  const auto pin = findPin(name);
  if (!pin) {
    return false;
  }
  // A link keeps both of its ends (ArenaManifold::link), so the pin's negward
  // neighbour is the cell that points at it -- home_ for the first pin.
  const CellRef prev = arena_.linked(*pin, dims_.pinningCursors, Negward);
  const CellRef next = arena_.linked(*pin, dims_.pinningCursors, Posward);
  zigzag::expectWritten(arena_.link(prev, dims_.pinningCursors, Posward, next));
  zigzag::expectWritten(
      arena_.link(*pin, dims_.pinningCursors, Posward, noCell));
  zigzag::expectWritten(arena_.link(*pin, dims_.cache, Posward, noCell));
  return true;
}

std::size_t VortexCore::collectGarbage() {
  std::size_t deadBefore = arena_.deadLinks();
  zigzag::expectWritten(arena_.compact());
  return deadBefore;
}

CellRef VortexCore::getOrCreateMemoPin(std::string_view opName) {
  auto pinOpt = findPin(opName);
  if (pinOpt) {
    return *pinOpt;
  }
  return pin(opName, noCell);
}

std::optional<std::vector<CellValue>>
VortexCore::lookupMemo(CellRef memoPin, const std::vector<CellValue> &inputs) {
  const auto rendered = std::views::transform(
      [this](const CellRef cell) { return render(cell); });
  const auto matches = [&](const CellRef entry) {
    return std::ranges::equal(inputsOf(entry) | rendered, inputs);
  };
  return zigzag::firstOf(zigzag::rankAfter(arena_, memoPin, dims_.cache) |
                         std::views::filter(matches))
      .transform([&](const CellRef entry) {
        return outputsOf(entry) | rendered | std::ranges::to<std::vector>();
      });
}

void VortexCore::recordMemo(CellRef memoPin,
                            const std::vector<CellValue> &inputs,
                            const std::vector<CellValue> &outputs) {
  CellRef entry = arena_.makeCell();
  for (const auto &inVal : inputs) {
    CellRef inCell = arena_.makeCell();
    value(inCell, 0, -1, inVal);
    bindInput(entry, inCell);
  }
  for (const auto &outVal : outputs) {
    CellRef outCell = arena_.makeCell();
    value(outCell, 0, -1, outVal);
    bindOutput(entry, outCell);
  }

  // Insert entry at head of memoPin's +d.cache rank
  CellRef oldHead = arena_.linked(memoPin, dims_.cache, DimVector::POS);
  zigzag::expectWritten(
      arena_.link(memoPin, dims_.cache, DimVector::POS, entry));
  if (oldHead != noCell) {
    zigzag::expectWritten(
        arena_.link(entry, dims_.cache, DimVector::POS, oldHead));
  }
}

bool VortexCore::flushMemo(std::string_view opName) { return flushPin(opName); }

bool VortexCore::retireMemo(std::string_view opName) {
  return retirePin(opName);
}

} // namespace zigzag::vortex
