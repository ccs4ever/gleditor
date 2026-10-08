/**
 * @file link_context.cpp
 * @brief Carrying out the link navigator's effects in a running session.
 */
#include "link_context.hpp"

#include <type_traits>
#include <utility>
#include <variant>

#include <gleditor/logging.hpp>

#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {

namespace {

/// The key a command names, if it names one.
std::optional<xanadu::LinkKey> keyNamed(const xanadu::NavigationCommand &c) {
  return std::visit(
      []<typename Command>(
          const Command &command) -> std::optional<xanadu::LinkKey> {
        if constexpr (requires { command.key; }) {
          return command.key;
        } else {
          return std::nullopt;
        }
      },
      c);
}

} // namespace

void LinkContext::setCandidates(const std::vector<zigzag::CellRef> &linkIds) {
  std::vector<xanadu::LinkKey> keys;
  keys.reserve(linkIds.size());
  for (const auto id : linkIds) {
    keys.push_back(keyOf(id));
  }
  const auto packages = session.readerLinkPackages().links();
  keys.insert(keys.end(), packages.begin(), packages.end());
  navigator.setCandidates(std::move(keys));
}

xanadu::LinkKey LinkContext::keyOf(const std::uint64_t id) const {
  return session.presentationLinkKey(id);
}

void LinkContext::packageVisibilityChanged() {
  const auto selected = navigator.selection();
  if (selected && selected->key.authority != session.store().documentId() &&
      !session.readerLinkPackages().find(selected->key))
    std::ignore = execute(nav::Dismiss{});
}

std::optional<Link> LinkContext::recordOf(const std::uint64_t id) const {
  const auto key = keyOf(id);
  if (const auto *pkg = session.readerLinkPackages().package(key))
    return resolvePackageLink(key, *pkg, {}, {}).link;
  const auto found = session.store().links().find(id);
  if (found == session.store().links().end()) return std::nullopt;
  return found->second;
}

std::expected<xanadu::LinkOccurrences, xanadu::LinkQueryError>
LinkContext::resolve(const xanadu::LinkKey &key) const {
  return resolveForPresentation(key);
}

std::expected<xanadu::LinkOccurrences, xanadu::LinkQueryError>
LinkContext::resolveForPresentation(const xanadu::LinkKey &key,
                                    const std::optional<int> cellRadius) const {
  const auto &primary = session.store();
  for (std::size_t i = 0; i < session.storeCount(); ++i)
    if (session.store(i).documentId() == key.authority &&
        session.readerLinkPackages().containsAuthority(key.authority))
      return std::unexpected(xanadu::LinkQueryError::LinkNotFound);
  const auto *package = session.readerLinkPackages().package(key);
  if (!package && key.authority != primary.documentId()) {
    return std::unexpected(xanadu::LinkQueryError::LinkNotFound);
  }
  if (package) return packageIndex(cellRadius).resolve(key, *package);
  std::vector<xanadu::DocumentView> documents;
  documents.reserve(session.views().size());
  for (const auto &view : session.views()) {
    documents.push_back({.store   = session.store(view.storeIndex).documentId(),
                         .version = view.version,
                         .text    = view.pieces});
  }
  std::vector<zigzag::CellRef> everyCell;
  std::vector<xanadu::CellView> cells;
  if (nullptr != manifold) {
    everyCell = manifold->cellsWithinRadius(!cellRadius      ? zigzag::noCell
                                            : cellFocusQuery ? cellFocusQuery()
                                                             : zigzag::noCell,
                                            cellRadius.value_or(-1));
    cells.push_back({.store    = session.store(manifoldStoreIndex).documentId(),
                     .version  = manifoldVersion,
                     .manifold = *manifold,
                     .cells    = everyCell});
  }
  return xanadu::resolveLinkOccurrences(primary, key.id, documents, cells);
}

PackageOccurrenceIndex
LinkContext::packageIndex(const std::optional<int> cellRadius) const {
  std::vector<PackageDocumentView> documents;
  for (const auto &view : session.views())
    documents.push_back({.store   = session.store(view.storeIndex),
                         .version = view.version,
                         .text    = view.pieces});
  std::vector<zigzag::CellRef> everyCell;
  std::vector<PackageCellView> cells;
  if (manifold && focusCell) {
    everyCell = manifold->cellsWithinRadius(!cellRadius      ? zigzag::noCell
                                            : cellFocusQuery ? cellFocusQuery()
                                                             : zigzag::noCell,
                                            cellRadius.value_or(-1));
    cells.push_back({.store    = session.store(manifoldStoreIndex),
                     .version  = manifoldVersion,
                     .manifold = *manifold,
                     .cells    = everyCell});
  }
  return PackageOccurrenceIndex(documents, cells);
}
std::vector<LinkOccurrences>
LinkContext::packagePresentation(const int cellRadius) const {
  const auto keys = session.readerLinkPackages().links();
  if (keys.empty()) return {};
  const auto index = packageIndex(cellRadius);
  std::vector<LinkOccurrences> result;
  for (const auto &key : keys)
    result.push_back(
        index.resolve(key, *session.readerLinkPackages().package(key)));
  return result;
}

std::optional<xanadu::OccurrenceSite> LinkContext::caretSite() const {
  const auto caret = caretQuery ? caretQuery() : std::nullopt;
  if (!caret || caret->view >= session.views().size()) {
    return std::nullopt;
  }
  const auto &view = session.views()[caret->view];
  return xanadu::DocumentSite{
      .store   = session.store(view.storeIndex).documentId(),
      .version = view.version,
      .range   = caret->selection.value_or(
          xanadu::Extent{.start = caret->offset, .end = caret->offset})};
}

std::optional<xanadu::OccurrenceSite>
LinkContext::cellSite(const zigzag::CellRef cell) const {
  if (nullptr == manifold || zigzag::noCell == cell) {
    return std::nullopt;
  }
  std::uint32_t length = 0;
  for (const auto &span : manifold->contentOf(cell)) {
    length += static_cast<std::uint32_t>(span.length);
  }
  const auto &slice = session.store(manifoldStoreIndex);
  return xanadu::CellSite{.store   = slice.documentId(),
                          .version = manifoldVersion,
                          .cell    = cell,
                          .range   = {.start = 0, .end = length}};
}

LinkContext::ReadingStamp LinkContext::readingStamp() const {
  return {.caret = caretQuery ? caretQuery() : std::nullopt,
          .cell  = cellFocusQuery ? cellFocusQuery() : zigzag::noCell,
          .visit = navigator.currentVisit()};
}

std::vector<xanadu::Visit> LinkContext::forwardChoices() const {
  std::vector<xanadu::Visit> choices;
  const auto current = navigator.currentVisit();
  if (!current) return choices;
  for (const auto child : activity.children(*current)) {
    if (const auto visit = activity.find(child)) {
      choices.push_back(*visit);
    }
  }
  return choices;
}

xanadu::ReadingPosition LinkContext::reading() const {
  xanadu::ReadingPosition position;
  const auto current  = navigator.currentVisit();
  const auto visit    = current
                            ? activity.find(*current)
                            : gleditor::cpp26::optional<const xanadu::Visit &>{};
  const auto selected = navigator.selection();
  position.entered    = visit && selected &&
                     xanadu::Arrival::EnteredEndpoint == visit->arrival &&
                     visit->link && visit->link->key == selected->key;
  const bool inCell =
      visit && std::holds_alternative<xanadu::CellSite>(visit->target);
  position.here =
      inCell && cellFocusQuery ? cellSite(cellFocusQuery()) : caretSite();
  return position;
}

void LinkContext::recordSourceArrival(const OccurrenceSite &site) {
  navigator.recordArrival(site);
  ++changes;
}

void LinkContext::noteOrigin() {
  const auto here = caretSite();
  if (!here) {
    return;
  }
  // Only when the reader has moved since the last visit: a selection made
  // where a trip ended starts from that visit, not from a copy of it.
  if (const auto current = navigator.currentVisit()) {
    if (const auto visit = activity.find(*current);
        visit && visit->target == *here) {
      return;
    }
  }
  navigator.recordArrival(*here);
}

xanadu::NavigationResult
LinkContext::execute(const xanadu::NavigationCommand &command) {
  const auto named    = keyNamed(command);
  const auto selected = navigator.selection();
  if (named && named->authority != session.store().documentId() &&
      !session.readerLinkPackages().find(*named))
    return std::unexpected(NavigationError::LinkNotFound);
  if (selected && selected->occurrences &&
      selected->key.authority != session.store().documentId()) {
    auto side           = selected->active;
    auto cursor         = selected->cursor(side);
    const auto *at      = std::get_if<nav::EnterAt>(&command);
    const bool entering = std::holds_alternative<nav::Enter>(command) ||
                          (at && at->key == selected->key);
    if (at && at->key == selected->key) {
      side              = at->side;
      cursor.member     = at->member;
      cursor.occurrence = at->occurrence;
    }
    if (entering && cursor.member && cursor.occurrence &&
        *cursor.member < selected->occurrences->members(side).size()) {
      const auto &member = selected->occurrences->members(side)[*cursor.member];
      if (*cursor.occurrence < member.occurrences.size()) {
        const auto &site     = member.occurrences[*cursor.occurrence].site;
        const bool focusable = std::visit(
            [this](const auto &where) {
              using Site = std::decay_t<decltype(where)>;
              if constexpr (std::is_same_v<Site, DocumentSite>)
                return bool(focusDocument) && viewIndexOf(where).has_value();
              else
                return bool(focusCell) && manifold &&
                       manifold->contains(where.cell) &&
                       where.store ==
                           session.store(manifoldStoreIndex).documentId() &&
                       where.version == manifoldVersion;
            },
            site);
        if (!focusable) {
          if (unavailable)
            unavailable("The chosen package endpoint is no longer open. Open "
                        "its pinned publication and select the link again.");
          return std::unexpected(NavigationError::MemberNotInView);
        }
      }
    }
  }
  const bool starting =
      std::holds_alternative<xanadu::nav::StepLink>(command) ||
      (named && (!selected || selected->key != *named));
  if (starting) {
    noteOrigin();
  }

  auto result = navigator.dispatch(command);
  if (!result) {
    GLEDITOR_LOG_DEBUG("xudu.links", "link command '{}' refused: {}",
                       xanadu::name(command), xanadu::name(result.error()));
    return result;
  }
  apply(*result);
  return result;
}

void LinkContext::apply(xanadu::NavigationEffect effect) {
  ++changes;
  if (effect.resolve) {
    // Synchronous while every view is local; the generation is what will let
    // this move off the render thread without a late answer landing on a link
    // the reader has left.
    const auto supplied = navigator.supply(effect.resolve->generation,
                                           resolve(effect.resolve->key));
    if (!supplied) {
      GLEDITOR_LOG_DEBUG("xudu.links", "link {} did not resolve: {}",
                         effect.resolve->key.id,
                         xanadu::name(supplied.error()));
      previewing.reset();
      if (effect.resolve->key.authority != session.store().documentId()) {
        if (unavailable)
          unavailable("The saved link package is disabled or unavailable. Its "
                      "activity record has been retained.");
        if (effect.focus) focus(*effect.focus);
      }
      return;
    }
    if (!effect.preview) {
      effect.preview = supplied->preview;
    }
  }
  if (effect.dismissed) {
    previewing.reset();
  } else if (effect.preview) {
    previewing = effect.preview;
  }
  if (effect.focus) {
    focus(*effect.focus);
  }
}

void LinkContext::restoreCurrentSelection() {
  if (auto effect = navigator.restoreCurrentSelection()) apply(*effect);
}

std::optional<xanadu::OccurrenceSite> LinkContext::originSite() const {
  const auto selected = navigator.selection();
  if (!selected || !selected->origin) {
    return std::nullopt;
  }
  if (const auto visit = activity.find(*selected->origin)) {
    return visit->target;
  }
  return std::nullopt;
}

std::optional<std::size_t>
LinkContext::viewIndexOf(const xanadu::DocumentSite &site) const {
  const auto &views = session.views();
  for (std::size_t i = 0; i < views.size(); ++i) {
    if (views[i].version == site.version &&
        session.store(views[i].storeIndex).documentId() == site.store) {
      return i;
    }
  }
  return std::nullopt;
}

std::string LinkContext::describe(const xanadu::OccurrenceSite &site) const {
  return std::visit(
      [this]<typename Site>(const Site &at) {
        const auto bytes = "bytes " + std::to_string(at.range.start) + " to " +
                           std::to_string(at.range.end);
        if constexpr (std::is_same_v<Site, xanadu::DocumentSite>) {
          if (const auto view = viewIndexOf(at)) {
            return "document " + std::to_string(*view) + ", " + bytes;
          }
          return "a closed version, " + bytes;
        } else {
          return "cell " + std::to_string(at.cell) + ", " + bytes;
        }
      },
      site);
}

void LinkContext::focus(const xanadu::OccurrenceSite &site) {
  std::visit(
      [this]<typename Site>(const Site &at) {
        if constexpr (std::is_same_v<Site, xanadu::DocumentSite>) {
          if (const auto view = viewIndexOf(at)) {
            if (focusDocument) {
              focusDocument(*view, at.range);
            }
            return;
          }
          // The version is no longer open. Opening it is the pending-target
          // work of a later stage; landing somewhere else would be worse.
          GLEDITOR_LOG_DEBUG("xudu.links", "entered {} is not open",
                             at.version.str());
        } else {
          if (!manifold ||
              at.store != session.store(manifoldStoreIndex).documentId() ||
              !manifold->contains(at.cell)) {
            GLEDITOR_LOG_DEBUG("xudu.links",
                               "saved cell {} is outside the bound slice",
                               at.cell);
            return;
          }
          if (focusCell) {
            focusCell(at.cell, at.range);
          } else {
            GLEDITOR_LOG_DEBUG("xudu.links", "no ZigZag view to enter cell {}",
                               at.cell);
          }
        }
      },
      site);
}

} // namespace xanadu
