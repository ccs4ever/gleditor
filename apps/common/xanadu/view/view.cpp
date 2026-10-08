#include "common/xanadu/view/view.hpp"

#include <algorithm>
#include <ranges>
#include <variant>

#include <gleditor/logging.hpp>

namespace xanadu::view {
namespace {

constexpr const char *kRegistryCategory = "view.registry";

using ChordKey = std::pair<std::string, std::string>;

ChordKey chordKey(const std::string_view chord, const std::string_view scope) {
  return {std::string(scope), canonicalChord(chord)};
}

} // namespace

ViewRegistry::ViewRegistry() {
  for (const auto &spec : defaultSettingSpecs(SystemDocKind::Keymap)) {
    if (spec.schemas.empty() || spec.schemas.front().defaultValues.empty()) {
      continue;
    }
    const auto *chord =
        std::get_if<std::string>(&spec.schemas.front().defaultValues.front());
    if (nullptr == chord || chord->empty()) {
      continue;
    }
    chords_.try_emplace(chordKey(*chord, keymapScope(spec.name)), spec.name);
  }
}

std::expected<ViewRegistry *, ViewError>
ViewRegistry::add(ViewDescriptor descriptor) {
  if (find(descriptor.kind)) {
    GLEDITOR_LOG_WARN(kRegistryCategory, "view kind '{}' is already installed",
                      descriptor.kind);
    return std::unexpected(ViewError::DuplicateViewKind);
  }
  // Every chord is checked, against the registry and against the
  // descriptor's own earlier chords, before any is taken, so a refusal
  // reserves nothing.
  std::vector<ChordKey> claimed;
  claimed.reserve(descriptor.chords.size());
  for (const auto &chord : descriptor.chords) {
    auto key = chordKey(chord.chord, chord.context);
    if (chords_.contains(key) || std::ranges::contains(claimed, key)) {
      GLEDITOR_LOG_WARN(kRegistryCategory,
                        "view '{}' wants {} for {}, which is already taken",
                        descriptor.kind, chord.chord, chord.call);
      return std::unexpected(ViewError::ChordCollision);
    }
    claimed.push_back(std::move(key));
  }
  for (std::size_t i = 0; i < claimed.size(); ++i) {
    chords_.emplace(std::move(claimed[i]), descriptor.chords[i].call);
  }
  GLEDITOR_LOG_DEBUG(kRegistryCategory, "installed view '{}' with {} chords",
                     descriptor.kind, descriptor.chords.size());
  views_.push_back(std::move(descriptor));
  return this;
}

gleditor::cpp26::optional<const ViewDescriptor &>
ViewRegistry::find(const std::string_view kind) const noexcept {
  const auto found = std::ranges::find(views_, kind, &ViewDescriptor::kind);
  if (found == views_.end()) {
    return gleditor::cpp26::nullopt;
  }
  return *found;
}

std::optional<std::string_view>
ViewRegistry::chordHolder(const std::string_view chord,
                          const std::string_view context) const {
  const auto found = chords_.find(chordKey(chord, context));
  if (found == chords_.end()) {
    return std::nullopt;
  }
  return found->second;
}

} // namespace xanadu::view
