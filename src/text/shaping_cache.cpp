#include <gleditor/text/fit.hpp>
#include <gleditor/text/shaping_cache.hpp>

#include <cmath>
#include <functional>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>

namespace gleditor::text {
namespace {

template <class... Values> void requireFinite(Values... values) {
  // NaN does not equal itself and therefore cannot identify an LRU entry.
  if (!(std::isfinite(values) && ...)) {
    throw std::invalid_argument("ShapingCache constraints must be finite");
  }
}

struct ShapingKey {
  std::string text;
  FontFacePtr font;
  LayoutOptions options;
  TextFit constraints;
  bool fitting{};

  bool operator==(const ShapingKey &other) const {
    if (text != other.text || font != other.font || fitting != other.fitting) {
      return false;
    }
    if (fitting) {
      return constraints.maxWidthPx == other.constraints.maxWidthPx &&
             constraints.maxHeightPx == other.constraints.maxHeightPx &&
             constraints.maxLines == other.constraints.maxLines &&
             constraints.overflow == other.constraints.overflow &&
             constraints.at == other.constraints.at &&
             constraints.align == other.constraints.align;
    }
    return options.maxWidthPx == other.options.maxWidthPx &&
           options.maxHeightPx == other.options.maxHeightPx &&
           options.singleParagraph == other.options.singleParagraph &&
           options.ellipsize == other.options.ellipsize &&
           options.decoratedRanges == other.options.decoratedRanges &&
           options.boxes == other.options.boxes &&
           options.blockStyles == other.options.blockStyles &&
           options.page == other.options.page;
  }
};

struct ShapingKeyHash {
  std::size_t operator()(const ShapingKey &key) const noexcept {
    std::size_t hash = std::hash<std::string>{}(key.text);
    const auto mix   = [&hash](auto value) {
      hash ^= std::hash<decltype(value)>{}(value) + 0x9e3779b97f4a7c15ULL +
              (hash << 6U) + (hash >> 2U);
    };
    const auto mixEnum = [&mix](const auto value) {
      mix(static_cast<std::size_t>(value));
    };
    mix(key.font.get());
    mix(key.fitting);
    if (key.fitting) {
      mix(key.constraints.maxWidthPx);
      mix(key.constraints.maxHeightPx);
      mix(key.constraints.maxLines);
      mixEnum(key.constraints.overflow);
      mixEnum(key.constraints.at);
      mixEnum(key.constraints.align);
      return hash;
    }
    const auto &options = key.options;
    mix(options.maxWidthPx);
    mix(options.maxHeightPx);
    mix(options.singleParagraph);
    mix(options.ellipsize);
    for (const auto &range : options.decoratedRanges) {
      mix(range.start);
      mix(range.end);
      mix(range.decorations);
    }
    for (const auto &box : options.boxes) {
      mix(box.anchor);
      mix(box.widthPx);
      mix(box.heightPx);
      mix(box.marginPx);
      mixEnum(box.placement);
      mix(box.baselineOffsetPx);
      mix(box.id);
    }
    for (const auto &range : options.blockStyles) {
      mix(range.start);
      mix(range.end);
      mixEnum(range.align);
      mix(range.indentFirstPx);
      mix(range.indentLeftPx);
      mix(range.indentRightPx);
      mix(range.placement.has_value());
      if (range.placement) {
        mixEnum(*range.placement);
      }
    }
    mixEnum(options.page.mode);
    mix(options.page.widthPx);
    mix(options.page.heightPx);
    mix(options.page.marginPx);
    return hash;
  }
};

} // namespace

struct ShapingCache::Impl {
  using Result = std::variant<PageShaping, FittedText>;
  // Map node addresses survive rehashing, so recency stores only key pointers
  // rather than a second copy of every retained text and layout option.
  using Recency = std::list<const ShapingKey *>;
  struct Entry {
    Result result;
    Recency::iterator position;
  };

  explicit Impl(const std::size_t capacity) : capacity(capacity) {
    if (capacity == 0) {
      throw std::invalid_argument("ShapingCache capacity must be positive");
    }
    entries.reserve(capacity);
  }

  template <class Shape> const Result &get(ShapingKey key, Shape shape) {
    if (const auto found = entries.find(key); found != entries.end()) {
      recency.splice(recency.begin(), recency, found->second.position);
      ++counters.hits;
      return found->second.result;
    }
    ++counters.misses;
    Result result = shape();
    if (entries.size() >= capacity) {
      entries.erase(*recency.back());
      recency.pop_back();
      ++counters.evictions;
    }
    const auto [entry, inserted] = entries.emplace(
        std::move(key), Entry{.result = std::move(result), .position = {}});
    recency.push_front(&entry->first);
    entry->second.position = recency.begin();
    return entry->second.result;
  }

  std::size_t capacity;
  Stats counters;
  Recency recency;
  std::unordered_map<ShapingKey, Entry, ShapingKeyHash> entries;
};

ShapingCache::ShapingCache(const std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}
ShapingCache::~ShapingCache()                                   = default;
ShapingCache::ShapingCache(ShapingCache &&) noexcept            = default;
ShapingCache &ShapingCache::operator=(ShapingCache &&) noexcept = default;

const PageShaping &ShapingCache::page(const std::string_view text,
                                      const FontFacePtr &font,
                                      const LayoutOptions &options) {
  requireFinite(options.maxWidthPx, options.maxHeightPx, options.page.widthPx,
                options.page.heightPx, options.page.marginPx);
  for (const auto &box : options.boxes) {
    requireFinite(box.widthPx, box.heightPx, box.marginPx,
                  box.baselineOffsetPx);
  }
  for (const auto &style : options.blockStyles) {
    requireFinite(style.indentFirstPx, style.indentLeftPx, style.indentRightPx);
  }
  return std::get<PageShaping>(impl_->get(
      ShapingKey{.text = std::string(text), .font = font, .options = options},
      [&] { return TextLayout::layoutPage(text, font, options); }));
}

const FittedText &ShapingCache::fitted(const std::string_view text,
                                       const FontFacePtr &font,
                                       const TextFit &constraints) {
  requireFinite(constraints.maxWidthPx, constraints.maxHeightPx);
  return std::get<FittedText>(
      impl_->get(ShapingKey{.text        = std::string(text),
                            .font        = font,
                            .constraints = constraints,
                            .fitting     = true},
                 [&] { return fit(text, font, constraints); }));
}

ShapingCache::Stats ShapingCache::stats() const noexcept {
  auto result    = impl_->counters;
  result.entries = impl_->entries.size();
  return result;
}

void ShapingCache::clear() noexcept {
  impl_->recency.clear();
  impl_->entries.clear();
}

} // namespace gleditor::text
