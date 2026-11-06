// §6.1's fragment-object emission (design/vagent-metaprogramming-
// implementation.md:224-321): compileFragment wraps a checked code"…"
// body — emitted through Compiler::compileFragmentBody — in its
// arena-resident object: head + descriptor + the +d.refs marking
// inventory + the +d.recipe replay template meta.cpp's reify replays
// verbatim. The record taxonomy is the sibling's: literal members,
// four-member link tuples whose `dim` field is name-keyed, and
// +d.refFor ref-capture members (+d.name for dim referents, +d.values
// for cap$N capture bindings).
#include "common/xanadu/vagent/fragment.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::CellValue;
using zigzag::vortex::VortexCore;

[[noreturn]] void refuse(const Expr &literal, DiagnosticCode code,
                         std::string detail) {
  throw Diagnostic{code, std::move(detail), literal.loc};
}

// The +d.vars record shape the descriptor and the program-image
// record share: a cell whose content is the key verbatim, one
// +d.values edge to the value, appended to the owner's +d.vars rank.
CellRef varsRecord(zigzag::ArenaManifold &arena, CellRef owner,
                   zigzag::DimRef varsDim, zigzag::DimRef valuesDim,
                   std::string_view key, CellRef value) {
  const auto record = arena.makeCell(key);
  zigzag::expectWritten(
      arena.link(record, valuesDim, zigzag::DimVector::POS, value));
  const auto tail = zigzag::rankTail(arena, owner, varsDim);
  zigzag::expectWritten(
      arena.link(tail, varsDim, zigzag::DimVector::POS, record));
  return record;
}

struct Chain {
  zigzag::ArenaManifold *arena;
  CellRef first{zigzag::noCell};
  CellRef tail{zigzag::noCell};
  void push(CellRef host, DimRef firstDim, DimRef stepDim, CellRef cell) {
    if (first == zigzag::noCell)
      zigzag::expectWritten(
          arena->link(host, firstDim, zigzag::DimVector::POS, cell));
    else
      zigzag::expectWritten(
          arena->link(tail, stepDim, zigzag::DimVector::POS, cell));
    first = first == zigzag::noCell ? cell : first;
    tail  = cell;
  }
};

// What one fragment's records need in scope: the arena, the dims the
// records link on, and the set of cells the recipe covers — every
// minted literal cell, every ref'd slot and every capture slot. A
// link tuple records only when *both* endpoints are covered cells:
// an outside endpoint would twin a state-side cell into the copy
// (the recipe's coverage set IS the internal/external split the
// replay's map-membership test reads).
struct RecordEmitter {
  zigzag::ArenaManifold &arena;
  const FragmentDims &fdims;
  const zigzag::vortex::SystemDimensions &core;
  std::unordered_set<CellRef> internal;
  Chain refs;
  Chain recipe;
  CellRef head{zigzag::noCell};

  // A +d.refs record marking `marked`; `name` present makes it a
  // NAME-record (+d.name→name cell, +d.grab→role cell), absent a
  // REF-record whose marked cell also takes the relocation marker.
  void refsRecord(CellRef marked, const std::optional<std::string> &name,
                  const std::optional<std::string> &role) {
    const auto record = arena.makeCell();
    zigzag::expectWritten(
        arena.link(record, fdims.ref, DimVector::POS, marked));
    if (name) {
      zigzag::expectWritten(
          arena.link(record, core.name, DimVector::POS, arena.makeCell(*name)));
      if (role)
        zigzag::expectWritten(arena.link(record, core.grab, DimVector::POS,
                                         arena.makeCell(*role)));
    } else {
      zigzag::expectWritten(arena.link(marked, fdims.meshRelocation,
                                       DimVector::POS, arena.makeCell("cell")));
    }
    refs.push(head, fdims.refs, core.step, record);
  }
  void recipeMember(CellRef member) {
    recipe.push(head, fdims.recipe, core.step, member);
  }
  // An int64 member cell — the position convention every recipe
  // member shares.
  CellRef member(std::int64_t position) {
    return arena.makeScalarCell(position);
  }
};

} // namespace

FragmentDims mintFragmentDims(VortexCore &core) {
  FragmentDims dims;
  dims.entry          = core.findOrMintDimension("d.entry");
  dims.refs           = core.findOrMintDimension("d.refs");
  dims.ref            = core.findOrMintDimension("d.ref");
  dims.recipe         = core.findOrMintDimension("d.recipe");
  dims.refFor         = core.findOrMintDimension("d.refFor");
  dims.meshRelocation = core.findOrMintDimension("d.mesh.relocation");
  return dims;
}

std::expected<FragmentObject, Diagnostic>
compileFragment(const Expr &literal, const FragmentContext &context,
                Compiler &compiler) {
  try {
    auto &arena    = compiler.core().arena();
    const auto key = fragmentKeyOf(literal);
    if (const auto cached = context.emitted.find(key);
        cached != context.emitted.end())
      return cached->second;

    if (!context.fragments)
      refuse(literal, DiagnosticCode::InvalidDeclaration,
             "code\"…\" literal has no fragment table — the expand "
             "stage must run before compile");
    const auto record = context.fragments->find(key);
    if (record == context.fragments->end() || !record->second.body.body)
      refuse(literal, DiagnosticCode::InvalidDeclaration,
             "code\"…\" literal has no fragment-table record; the "
             "expand stage must run before compile");
    if (context.meta) {
      const auto meta = context.meta->find(key);
      if (meta != context.meta->end() && meta->second.spliceOnly)
        refuse(literal, DiagnosticCode::InvalidDeclaration,
               "a splice-only fragment (parameterized by a Code "
               "parameter) emits no head and no descriptor — it may "
               "appear only as a splice or template actual");
    }
    if (!context.info)
      refuse(literal, DiagnosticCode::InvalidDeclaration,
             "code\"…\" literal has no checked fragment info; the "
             "check stage must run before compile");
    const auto info = context.info->find(key);
    if (info == context.info->end())
      refuse(literal, DiagnosticCode::InvalidDeclaration,
             "code\"…\" literal has no checked fragment info; the "
             "check stage must run before compile");

    // Emit the body through the scalar Emitter — parameters seeded as
    // name-keyed frame-bound locals, the Return tail and published
    // result/produced cells inside the usual choice wrapper.
    FragmentBodyRequest request{
        .body       = record->second.body.body.get(),
        .parameters = record->second.body.parameters,
        .bindings   = context.bindings,
        .fragments  = &context,
        .use        = ResultUse::Value,
        .ownerFields =
            info->second.ownerFields.empty() ? nullptr
                                             : &info->second.ownerFields,
    };
    auto body = compiler.compileFragmentBody(request);
    if (!body) return std::unexpected(body.error());

    const auto fdims = mintFragmentDims(compiler.core());
    const auto &dims = compiler.core().dims();

    // ---- Head ------------------------------------------------------------
    const auto head = arena.makeCell();
    const auto label =
        fragmentLabelOf(key, record->second.body.label, context.module);
    zigzag::expectWritten(
        arena.link(head, fdims.entry, DimVector::POS, body->entry));
    zigzag::expectWritten(
        arena.link(head, dims.name, DimVector::POS, arena.makeCell(label)));

    // ---- Descriptor --------------------------------------------------------
    // head +d.values → descriptor; fields as +d.vars records whose
    // content is the key and whose +d.values edge is the value — the
    // shape the sibling's descriptorField reads. The result record's
    // own +d.name carries the result type name (§6.1's head layout).
    // Every apparatus cell and link lands in `apparatusCells`/
    // `apparatusLinks` so the recipe below twins the descriptor on
    // reified copies — a copy must be runnable, so its head needs its
    // own +d.values descriptor, not a delegation.
    std::vector<CellRef> apparatusCells;
    struct ApparatusLink {
      CellRef from;
      DimRef dim;
      bool neg;
      CellRef to;
      std::string_view dimName;
    };
    std::vector<ApparatusLink> apparatusLinks;
    const auto aCell = [&](CellRef cell) {
      apparatusCells.push_back(cell);
      return cell;
    };
    const auto aLink = [&](CellRef from, DimRef dim, CellRef to,
                           std::string_view dimName) {
      zigzag::expectWritten(arena.link(from, dim, DimVector::POS, to));
      apparatusLinks.push_back({from, dim, false, to, dimName});
    };

    const auto descriptor = aCell(arena.makeCell());
    aLink(head, dims.values, descriptor, "d.values");
    const auto field = [&](std::string_view key, CellRef value) {
      const auto rec = aCell(arena.makeCell(key));
      aLink(rec, dims.values, value, "d.values");
      // +d.vars records chain tail-to-tail — a cell's one posward edge
      // per dim makes descriptor +d.vars → the first record only.
      aLink(zigzag::rankTail(arena, descriptor, dims.vars), dims.vars, rec,
            "d.vars");
      return rec;
    };
    const auto resultRecord = field("result", body->result);
    const auto resultType =
        aCell(arena.makeCell(typeName(info->second.result)));
    aLink(resultRecord, dims.name, resultType, "d.name");
    field("produced", body->produced);
    field("pure", aCell(arena.makeScalarCell(info->second.pure)));
    field("closed", aCell(arena.makeScalarCell(info->second.closed)));
    field("arity", aCell(arena.makeScalarCell(
                       static_cast<std::int64_t>(info->second.arity))));
    for (const auto &parameter : record->second.body.parameters)
      field(parameter.name, aCell(arena.makeCell(typeName(parameter.type))));

    // Ordered parameter signature on the descriptor's +d.grab →
    // +d.step chain, each +d.name→name and +d.values→type — the shape
    // the sibling's parameterCells and §7.2 signature comparison read.
    CellRef paramTail = zigzag::noCell;
    for (const auto &parameter : record->second.body.parameters) {
      const auto cell = aCell(arena.makeCell());
      aLink(cell, dims.name, aCell(arena.makeCell(parameter.name)), "d.name");
      aLink(cell, dims.values, aCell(arena.makeCell(typeName(parameter.type))),
            "d.values");
      if (paramTail == zigzag::noCell)
        aLink(descriptor, dims.grab, cell, "d.grab");
      else
        aLink(paramTail, dims.step, cell, "d.step");
      paramTail = cell;
    }

    // ---- Coverage set ------------------------------------------------------
    // Every cell the emission minted: body literals carry every temp,
    // scalar and op; refs the int64 slots; captures the cap$N slots;
    // apparatus the head/descriptor/param apparatus cells. The union
    // is the recipe's internal set — anything else named by content
    // is external and rebinds verbatim.
    RecordEmitter records{.arena  = arena,
                          .fdims  = fdims,
                          .core   = dims,
                          .refs   = {.arena = &arena},
                          .recipe = {.arena = &arena},
                          .head   = head};
    records.internal.insert(head);
    for (const auto &entry : body->literals)
      records.internal.insert(entry.cell);
    for (const auto &entry : body->refs) records.internal.insert(entry.cell);
    for (const auto &entry : body->captures)
      records.internal.insert(entry.slot);
    for (const auto cell : apparatusCells) records.internal.insert(cell);

    // ---- Recipe members, replay order -------------------------------------
    // Literals and link tuples first — the ref-capture arm's
    // mapped() rebind needs every internal referent already twinned
    // when its member replays. Every member cell is collected for the
    // refs sweep below.
    std::vector<CellRef> memberCells;
    const auto pushMember = [&](CellRef member) {
      records.recipeMember(member);
      memberCells.push_back(member);
    };
    for (const auto &entry : body->literals)
      pushMember(records.member(static_cast<std::int64_t>(entry.cell)));
    for (const auto cell : apparatusCells)
      pushMember(records.member(static_cast<std::int64_t>(cell)));
    const auto linkTuple = [&](CellRef from, DimRef dim, bool neg, CellRef to,
                               std::string_view dimName) {
      // Reify resolves tuple dims by name only — a name-less link
      // cannot record (the dim identity must survive genesis' name
      // re-resolution, never ride a raw position).
      if (dimName.empty())
        refuse(literal, DiagnosticCode::InvalidDeclaration,
               "fragment emission linked on an unnamed dimension — the "
               "+d.recipe tuple records dims by name");
      pushMember(records.member(static_cast<std::int64_t>(from)));
      const auto anchor = records.member(static_cast<std::int64_t>(dim));
      zigzag::expectWritten(arena.link(anchor, dims.name, DimVector::POS,
                                       arena.makeCell(dimName)));
      pushMember(anchor);
      pushMember(records.member(neg ? std::int64_t{-1} : std::int64_t{1}));
      pushMember(records.member(static_cast<std::int64_t>(to)));
    };
    for (const auto &link : body->links) {
      // An uncovered endpoint is an external/state-side cell — the
      // tuple would twin it, so the link does not record (the clone-
      // rank append of a capture slot rides the capture record
      // instead).
      if (!records.internal.contains(link.from) ||
          !records.internal.contains(link.to))
        continue;
      linkTuple(link.from, link.dim, link.neg, link.to, link.dimName);
    }
    for (const auto &link : apparatusLinks)
      linkTuple(link.from, link.dim, link.neg, link.to, link.dimName);
    // The head's own entry edge: reify maps the head to the copy's
    // head, so the tuple replays as newHead +d.entry → the entry twin —
    // a self-contained graph, never a delegation. head +d.name and the
    // +d.refs/+d.recipe chains are excluded on purpose: reify owns the
    // copy's label and regenerates both chains from its map.
    linkTuple(head, fdims.entry, false, body->entry, "d.entry");

    // Ref-capture members — every EmissionRef's slot rebinds its
    // int64 referent through the old→new map (internal → twin,
    // external → verbatim, dim → name re-resolution), and every one
    // joins the refs inventory (name-records carrying +d.name and the
    // callsite role under +d.grab).
    for (const auto &entry : body->refs) {
      const auto m = records.member(static_cast<std::int64_t>(entry.cell));
      zigzag::expectWritten(
          arena.link(m, fdims.refFor, DimVector::POS, entry.cell));
      if (!entry.dimName.empty())
        zigzag::expectWritten(arena.link(m, dims.name, DimVector::POS,
                                         arena.makeCell(entry.dimName)));
      pushMember(m);
      records.refsRecord(entry.cell, entry.name,
                         entry.name ? std::optional<std::string>{entry.role}
                                    : std::nullopt);
    }
    // cap$N capture slots — the +d.values→binding arm re-appends the
    // slot twin onto the *same* binding's +d.clone tail (a captured
    // cell is shared, never copied).
    for (const auto &entry : body->captures) {
      const auto m = records.member(static_cast<std::int64_t>(entry.slot));
      zigzag::expectWritten(
          arena.link(m, fdims.refFor, DimVector::POS, entry.slot));
      zigzag::expectWritten(
          arena.link(m, dims.values, DimVector::POS, entry.binding));
      pushMember(m);
      records.refsRecord(entry.slot, std::nullopt, std::nullopt);
    }

    // ---- Recipe-member refs records ---------------------------------------
    // The refs inventory covers +d.recipe field cells beside the
    // slots — every member cell gets a REF-record, its relocation
    // marker on the member cell itself.
    for (const auto memberCell : memberCells)
      records.refsRecord(memberCell, std::nullopt, std::nullopt);

    const FragmentObject object{head, body->entry, body->result,
                                body->produced};
    context.emitted.emplace(key, object);
    return object;
  } catch (const Diagnostic &diagnostic) {
    return std::unexpected(diagnostic);
  }
}

std::optional<CellRef> finalizeProgramImage(const Compiler &compiler) {
  if (compiler.programOps().empty()) return std::nullopt;
  auto &arena      = compiler.core().arena();
  const auto &dims = compiler.core().dims();
  const auto image = arena.makeCell();
  varsRecord(
      arena, image, dims.vars, dims.values, "floor",
      arena.makeScalarCell(static_cast<std::int64_t>(compiler.imageFloor())));
  const auto opsHead = arena.makeCell();
  CellRef tail       = zigzag::noCell;
  for (const auto op : compiler.programOps()) {
    const auto cell = arena.makeScalarCell(static_cast<std::int64_t>(op));
    if (tail == zigzag::noCell)
      zigzag::expectWritten(
          arena.link(opsHead, dims.step, DimVector::POS, cell));
    else
      zigzag::expectWritten(arena.link(tail, dims.step, DimVector::POS, cell));
    tail = cell;
  }
  varsRecord(arena, image, dims.vars, dims.values, "ops", opsHead);
  return image;
}

} // namespace xanadu::vagent
