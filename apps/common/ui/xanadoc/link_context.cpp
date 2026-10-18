/**
 * @file link_context.cpp
 * @brief Carrying out the link navigator's effects in a running session.
 */
#include "link_context.hpp"

#include <gleditor/render_state.hpp>

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

bool LinkContext::readerStore(const std::size_t index) const {
  return (manifold && manifoldStoreIndex == index) ||
         std::ranges::any_of(session.views(), [index](const auto &view) {
           return view.storeIndex == index;
         });
}

void LinkContext::setCandidates(const std::vector<zigzag::CellRef> &linkIds) {
  std::vector<xanadu::LinkKey> keys;
  keys.reserve(linkIds.size());
  for (const auto id : linkIds) {
    if (!readerStore(0)) break;
    const auto key    = keyOf(id);
    const auto *owner = nativeStoreOf(key);
    if (owner) {
      const auto found = owner->links().find(key.id);
      if (found != owner->links().end() &&
          found->second.type == LinkType::Format)
        continue;
    }
    keys.push_back(key);
  }
  for (std::size_t i = 1; i < session.storeCount(); ++i) {
    if (!readerStore(i)) continue;
    const auto &store = session.store(i);
    for (const auto &[id, link] : store.links())
      if (link.type != LinkType::Format)
        keys.push_back({.authority = store.documentId(), .id = id});
  }
  const auto packages = session.readerLinkPackages().links();
  keys.insert(keys.end(), packages.begin(), packages.end());
  std::vector<LinkKey> unique;
  for (const auto &key : keys)
    if (std::ranges::find(unique, key) == unique.end()) unique.push_back(key);
  navigator.setCandidates(std::move(unique));
}

xanadu::LinkKey LinkContext::keyOf(const std::uint64_t id) const {
  return session.presentationLinkKey(id);
}

const Store *LinkContext::nativeStoreOf(const LinkKey &key) const {
  for (std::size_t i = 0; i < session.storeCount(); ++i)
    if (session.store(i).documentId() == key.authority)
      return &session.store(i);
  return nullptr;
}

void LinkContext::packageVisibilityChanged() {
  const auto selected = navigator.selection();
  if (selected && !nativeStoreOf(selected->key) &&
      !session.readerLinkPackages().find(selected->key))
    std::ignore = execute(nav::Dismiss{});
}

std::optional<Link> LinkContext::recordOf(const std::uint64_t id) const {
  const auto key = keyOf(id);
  if (const auto *pkg = session.readerLinkPackages().package(key))
    return resolvePackageLink(key, *pkg, {}, {}).link;
  const auto *store = nativeStoreOf(key);
  if (!store) return std::nullopt;
  const auto found = store->links().find(key.id);
  if (found == store->links().end()) return std::nullopt;
  return found->second;
}

std::expected<xanadu::LinkOccurrences, xanadu::LinkQueryError>
LinkContext::resolve(const xanadu::LinkKey &key) const {
  return resolveForPresentation(key);
}

std::expected<xanadu::LinkOccurrences, xanadu::LinkQueryError>
LinkContext::resolveForPresentation(const xanadu::LinkKey &key,
                                    const std::optional<int> cellRadius) const {
  const auto *owner = nativeStoreOf(key);
  for (std::size_t i = 0; i < session.storeCount(); ++i)
    if (session.store(i).documentId() == key.authority &&
        session.readerLinkPackages().containsAuthority(key.authority))
      return std::unexpected(xanadu::LinkQueryError::LinkNotFound);
  const auto *package = session.readerLinkPackages().package(key);
  if (!package && !owner) {
    return std::unexpected(xanadu::LinkQueryError::LinkNotFound);
  }
  if (package) return packageIndex(cellRadius).resolve(key, *package);
  return packageIndex(cellRadius).resolve(*owner, key.id);
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
LinkContext::presentationLinks(const int cellRadius) const {
  const auto keys = session.readerLinkPackages().links();
  std::vector<LinkOccurrences> result;
  const auto index = packageIndex(cellRadius);
  for (std::size_t i = 0; i < session.storeCount(); ++i) {
    if (!readerStore(i)) continue;
    const auto &store = session.store(i);
    for (const auto &[id, link] : store.links())
      if (link.type != LinkType::Format)
        if (auto resolved = index.resolve(store, id))
          result.push_back(std::move(*resolved));
  }
  if (keys.empty()) return result;
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
  const auto current = navigator.currentVisit();
  const auto visit   = current
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

std::string LinkContext::previewVisit(const VisitId id) const {
  const auto visit = activity.find(id);
  if (!visit) return "Unavailable visit";
  return std::visit(
      [this](const auto &site) -> std::string {
        const auto index =
            session.storeIndexForAuthority(site.store.str(), site.version);
        if (!index) return "Unavailable: open the saved source store";
        const auto &store = session.store(*index);
        if (!store.segmentedOps().contains(site.version))
          return "Unavailable saved version";
        Version text;
        if constexpr (std::is_same_v<std::decay_t<decltype(site)>, CellSite>) {
          const auto slice = store.rebuildManifold(site.version);
          if (!slice.contains(site.cell)) return "Unavailable saved cell";
          const auto spans = slice.contentOf(site.cell);
          text.insertSpans(
              0, std::vector<PrimediaSpan>(spans.begin(), spans.end()));
        } else {
          const auto view = viewIndexOf(site);
          text = store.rebuild(site.version,
                               view ? session.views()[*view].focusedBirth : 0);
        }
        if (site.range.end < site.range.start || site.range.end > text.length())
          return "Unavailable saved range";
        std::string excerpt;
        for (const auto &span :
             text.spansFor(site.range.start, site.range.end - site.range.start))
          excerpt += store.read(span);
        return describe(OccurrenceSite{site}) + ": " + excerpt;
      },
      visit->target);
}

xanadu::NavigationResult
LinkContext::execute(const xanadu::NavigationCommand &command) {
  if (pendingEntry) {
    pendingEntry.reset();
    ++changes;
  }
  if (std::holds_alternative<nav::CancelEntry>(command)) {
    entryUnavailable.clear();
    ++changes;
    return NavigationEffect{};
  }
  const auto named    = keyNamed(command);
  const auto selected = navigator.selection();
  if (named && !nativeStoreOf(*named) &&
      !session.readerLinkPackages().find(*named))
    return std::unexpected(NavigationError::LinkNotFound);
  entryUnavailable.clear();
  std::optional<OccurrenceSite> target;
  if (const auto *restore = std::get_if<nav::EnterVisit>(&command)) {
    if (const auto visit = activity.find(restore->visit))
      target = visit->target;
  } else if (const auto *forward =
                 std::get_if<nav::ActivityForward>(&command)) {
    if (const auto visit = activity.find(forward->child))
      target = visit->target;
  } else if (std::holds_alternative<nav::ActivityBack>(command)) {
    if (const auto current = navigator.currentVisit())
      if (const auto visit = activity.find(*current); visit && visit->parent)
        if (const auto parent = activity.find(*visit->parent))
          target = parent->target;
  } else if (std::holds_alternative<nav::ReturnToOrigin>(command)) {
    target = originSite();
  } else if (selected && selected->occurrences) {
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
      if (*cursor.occurrence < member.occurrences.size())
        target = member.occurrences[*cursor.occurrence].site;
    }
  }
  if (target) {
    const bool focusable = std::visit(
        [this](const auto &site) {
          using Site = std::decay_t<decltype(site)>;
          if constexpr (std::is_same_v<Site, DocumentSite>) {
            return (bool(focusDocument) || bool(documentEntry)) &&
                   viewIndexOf(site).has_value();
          } else {
            const auto index =
                session.storeIndexForAuthority(site.store.str(), site.version);
            if (!index ||
                !session.store(*index).segmentedOps().contains(site.version))
              return false;
            const auto slice =
                session.store(*index).rebuildManifold(site.version);
            if (!slice.contains(site.cell)) return false;
            std::uint64_t length = 0;
            for (const auto &span : slice.contentOf(site.cell))
              length += span.length;
            return site.range.start <= site.range.end &&
                   site.range.end <= length &&
                   ((bool(cellEntry) && renderState_) ||
                    (bool(focusCell) && manifold &&
                     site.version == manifoldVersion &&
                     site.store ==
                         session.store(manifoldStoreIndex).documentId()));
          }
        },
        *target);
    if (!focusable) {
      entryUnavailable = "Unavailable: open the exact saved document or cell "
                         "version. Cancel to keep reading.";
      ++changes;
      return std::unexpected(NavigationError::MemberNotInView);
    }
  }
  if (selected && selected->occurrences && renderState_) {
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
        const auto *site = std::get_if<DocumentSite>(
            &member.occurrences[*cursor.occurrence].site);
        const auto view = site ? viewIndexOf(*site) : std::nullopt;
        if (view && *view < renderState_->docs.size() &&
            renderState_->docs[*view]) {
          auto &doc = *renderState_->docs[*view];
          const std::array offsets{site->range.start,
                                   site->range.end > site->range.start
                                       ? site->range.end - 1
                                       : site->range.start};
          if (!doc.anchorFor(offsets[0]) || !doc.anchorFor(offsets[1])) {
            doc.setPriorityOffsets(offsets);
            pendingEntry =
                PendingEntry{command, session.generation(), *view, *site};
            ++changes;
            return NavigationEffect{};
          }
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
    GLEDITOR_LOG_DEBUG("xuzz.links", "link command '{}' refused: {}",
                       xanadu::name(command), xanadu::name(result.error()));
    return result;
  }
  apply(*result);
  return result;
}

void LinkContext::advanceFrame(RenderState &state) {
  renderState_ = &state;
  if (!pendingEntry) return;
  const auto &request = *pendingEntry;
  if (request.views != session.generation() ||
      request.view >= state.docs.size() || !state.docs[request.view]) {
    pendingEntry.reset();
    ++changes;
    return;
  }
  auto &doc = *state.docs[request.view];
  const std::array offsets{request.site.range.start,
                           request.site.range.end > request.site.range.start
                               ? request.site.range.end - 1
                               : request.site.range.start};
  if (!doc.anchorFor(offsets[0]) || !doc.anchorFor(offsets[1])) {
    doc.setPriorityOffsets(offsets);
    return;
  }
  const auto command = request.command;
  pendingEntry.reset();
  std::ignore = execute(command);
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
      GLEDITOR_LOG_DEBUG("xuzz.links", "link {} did not resolve: {}",
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

void LinkContext::restoreSelection(const LinkVisitContext &saved) {
  if (auto effect = navigator.restoreSelection(saved)) apply(*effect);
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
        if constexpr (std::is_same_v<Site, xanadu::DocumentSite>) {
          if (const auto view = viewIndexOf(at)) {
            const auto &opened = session.views()[*view];
            auto label = std::filesystem::path(session.path(opened.storeIndex))
                             .filename()
                             .string();
            if (renderState_ && *view < renderState_->docs.size() &&
                renderState_->docs[*view]) {
              const auto &doc = *renderState_->docs[*view];
              if (const auto page = doc.pageIndexForOffset(at.range.start)) {
                const auto last = doc.pageIndexForOffset(
                    at.range.end > at.range.start ? at.range.end - 1
                                                  : at.range.start);
                const bool crosses = last && *last != *page;
                label += crosses ? " · pages " : " · page ";
                label += std::to_string(*page + 1);
                if (crosses) label += "–" + std::to_string(*last + 1);
                label += "/" + std::to_string(doc.numPages()) +
                         (doc.isFullyLoaded() ? "" : "+");
                if (crosses)
                  label += " · continues across page breaks";
                else if (!last)
                  label += " · end layout pending";
              } else
                label += " · layout pending";
            }
            return label;
          }
          return std::string{"a closed version"};
        } else {
          return "cell " + std::to_string(at.cell);
        }
      },
      site);
}

std::optional<LinkKey> LinkContext::enteredLink() const {
  if (const auto current = navigator.currentVisit())
    if (const auto visit = activity.find(*current);
        visit && visit->arrival == Arrival::EnteredEndpoint && visit->link)
      return visit->link->key;
  return std::nullopt;
}

void LinkContext::focus(const xanadu::OccurrenceSite &site) {
  std::visit(
      [this]<typename Site>(const Site &at) {
        if constexpr (std::is_same_v<Site, xanadu::DocumentSite>) {
          if (const auto view = viewIndexOf(at)) {
            if (documentEntry && renderState_) {
              documentEntry(*view, at.range, *renderState_);
            } else if (focusDocument) {
              focusDocument(*view, at.range);
            }
            return;
          }
          // The version is no longer open. Opening it is the pending-target
          // work of a later stage; landing somewhere else would be worse.
          GLEDITOR_LOG_DEBUG("xuzz.links", "entered {} is not open",
                             at.version.str());
        } else {
          if (cellEntry && renderState_) {
            if (const auto index = session.storeIndexForAuthority(
                    at.store.str(), at.version)) {
              cellEntry(*renderState_, *index, at);
              return;
            }
          }
          if (!manifold || at.version != manifoldVersion ||
              at.store != session.store(manifoldStoreIndex).documentId() ||
              !manifold->contains(at.cell)) {
            GLEDITOR_LOG_DEBUG("xuzz.links",
                               "saved cell {} is outside the bound slice",
                               at.cell);
            return;
          }
          if (focusCell) {
            focusCell(at.cell, at.range);
          } else {
            GLEDITOR_LOG_DEBUG("xuzz.links", "no ZigZag view to enter cell {}",
                               at.cell);
          }
        }
      },
      site);
}

} // namespace xanadu
