/**
 * @file compact_zzcell.hpp
 * @brief Cache-aligned, zero-copy ZigZag cell structure linking directly into
 *        Xudu primedia and operations spools.
 */
#ifndef ZIGZAG_COMPACT_ZZCELL_HPP
#define ZIGZAG_COMPACT_ZZCELL_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xudu/core/compact_op.hpp"
#include "xudu/core/resolver.hpp"
#include "xudu/core/scroll.hpp"
#include "xudu/core/segmented_ops_spool.hpp"
#include "xudu/core/segmented_primedia_spool.hpp"
#include "xudu/core/spool.hpp"
#include "zigzag/core/zzstructure.hpp"

namespace zigzag {

/**
 * @enum DimOrdinal
 * @brief Fixed standard dimensions mapped to fast inline array indices.
 */
enum class DimOrdinal : std::uint8_t {
  D1         = 0,  ///< d.1 (Primary spatial X)
  D2         = 1,  ///< d.2 (Primary spatial Y)
  D3         = 2,  ///< d.3 (Primary spatial Z)
  D4         = 3,  ///< d.4
  D5         = 4,  ///< d.5
  Doc        = 5,  ///< d.doc (Reading flow / linear text)
  Transclude = 6,  ///< d.transclude (Primedia span identity)
  OpsTime    = 7,  ///< d.ops_time (Sequential append log)
  OpsDag     = 8,  ///< d.ops_dag (Ancestral tree lineage)
  Version    = 9,  ///< d.version (Microversion progression)
  Link       = 10, ///< d.link (Xanalinks / commentary)
  Clone      = 11, ///< d.clone (Clone family)
  Count      = 12
};

inline constexpr std::size_t StandardDimensionCount =
    static_cast<std::size_t>(DimOrdinal::Count);

[[nodiscard]] constexpr std::optional<DimOrdinal>
dimOrdinalFromString(std::string_view name) noexcept {
  if (name == "d.1") return DimOrdinal::D1;
  if (name == "d.2") return DimOrdinal::D2;
  if (name == "d.3") return DimOrdinal::D3;
  if (name == "d.4") return DimOrdinal::D4;
  if (name == "d.5") return DimOrdinal::D5;
  if (name == "d.doc") return DimOrdinal::Doc;
  if (name == "d.transclude") return DimOrdinal::Transclude;
  if (name == "d.ops_time") return DimOrdinal::OpsTime;
  if (name == "d.ops_dag") return DimOrdinal::OpsDag;
  if (name == "d.version") return DimOrdinal::Version;
  if (name == "d.link") return DimOrdinal::Link;
  if (name == "d.clone") return DimOrdinal::Clone;
  return std::nullopt;
}

[[nodiscard]] constexpr std::string_view
dimOrdinalToString(DimOrdinal ord) noexcept {
  switch (ord) {
  case DimOrdinal::D1:
    return "d.1";
  case DimOrdinal::D2:
    return "d.2";
  case DimOrdinal::D3:
    return "d.3";
  case DimOrdinal::D4:
    return "d.4";
  case DimOrdinal::D5:
    return "d.5";
  case DimOrdinal::Doc:
    return "d.doc";
  case DimOrdinal::Transclude:
    return "d.transclude";
  case DimOrdinal::OpsTime:
    return "d.ops_time";
  case DimOrdinal::OpsDag:
    return "d.ops_dag";
  case DimOrdinal::Version:
    return "d.version";
  case DimOrdinal::Link:
    return "d.link";
  case DimOrdinal::Clone:
    return "d.clone";
  case DimOrdinal::Count:
    break;
  }
  return "";
}

/**
 * @struct DynamicDimensionLink
 * @brief Overflow storage for custom or user-defined dimensions.
 */
struct DynamicDimensionLink {
  DimID name;
  LinkPairs links;

  bool operator==(const DynamicDimensionLink &) const = default;
};

/**
 * @struct CompactZZCell
 * @brief A ZigZag cell: its address in the primedia spool, its links along
 *        each dimension, and how its content resolved.
 *
 * **Not 64 bytes, and not zero-copy** -- which this comment, the design
 * document and CLAUDE.md all used to claim. It is around 960 bytes, aligned
 * to 8, and it heap-allocates: a vector of dynamic dimensions, three
 * optionals, a std::string type tag, and `ephemeralText`, which holds a copy
 * of the very primedia that `span` already addresses.
 *
 * The claim survived as long as it did because nothing checked it. The
 * static_assert at the end of this file does now -- not at 64, which would be
 * a lie in the other direction, but at a ceiling this cannot quietly drift
 * past again.
 *
 * Reaching 64 bytes is a real refactor and worth doing: split the hot fields
 * (id, spoolOpIndex, span, standardDimensions) from the cold ones and put the
 * cold ones in a side table keyed by CellID, so a staging pass walks cache
 * lines instead of chasing pointers. Until somebody does that, the honest
 * thing is to say what the struct costs.
 */
struct CompactZZCell {
  CellID id{0};
  std::uint32_t spoolOpIndex{0}; ///< CompactOpNode index in SegmentedOpsSpool
  xudu::PrimediaSpan span{};     ///< Canonical address in primedia spool

  /// Fast inline fixed-size link table for standard dimensions
  std::array<LinkPairs, StandardDimensionCount> standardDimensions{};

  /// Dynamic overflow for user-defined dimensions
  std::vector<DynamicDimensionLink> dynamicDimensions{};

  std::optional<Preflet> preflet{};
  std::string type{"cell"};
  std::string ephemeralText{};
  xudu::ResolutionStatus resolutionStatus{
      xudu::ResolutionStatus::VerifiedBytes};
  std::optional<xudu::TranscopyrightDescriptor> transcopyrightInfo{};
  std::optional<xudu::PublishedHoleRecord> holeRecord{};

  [[nodiscard]] bool isWithheld() const noexcept {
    return resolutionStatus == xudu::ResolutionStatus::WithheldRedacted;
  }
  [[nodiscard]] bool isTranscopyrightLocked() const noexcept {
    return resolutionStatus == xudu::ResolutionStatus::TranscopyrightLocked;
  }

  [[nodiscard]] LinkPairs linksOn(DimOrdinal ord) const noexcept {
    const auto idx = static_cast<std::size_t>(ord);
    if (idx < StandardDimensionCount) {
      return standardDimensions[idx];
    }
    return {};
  }

  void setLinks(DimOrdinal ord, LinkPairs lp) noexcept {
    const auto idx = static_cast<std::size_t>(ord);
    if (idx < StandardDimensionCount) {
      standardDimensions[idx] = lp;
    }
  }

  [[nodiscard]] LinkPairs linksOn(const DimID &name) const noexcept {
    if (const auto ord = dimOrdinalFromString(name); ord.has_value()) {
      return linksOn(*ord);
    }
    for (const auto &d : dynamicDimensions) {
      if (d.name == name) {
        return d.links;
      }
    }
    return {};
  }

  void setLinks(const DimID &name, LinkPairs lp) {
    if (const auto ord = dimOrdinalFromString(name); ord.has_value()) {
      setLinks(*ord, lp);
      return;
    }
    for (auto &d : dynamicDimensions) {
      if (d.name == name) {
        d.links = lp;
        return;
      }
    }
    dynamicDimensions.push_back({name, lp});
  }

  /**
   * @brief Read cell content as a string.
   */
  [[nodiscard]] std::string
  readText(const xudu::PrimediaSpool &primedia, const xudu::Resolver &resolver,
           const std::vector<xudu::Scroll> &externals) const {
    if (!ephemeralText.empty()) {
      return ephemeralText;
    }
    if (isWithheld()) {
      return "[Redacted - Withheld]";
    }
    if (isTranscopyrightLocked()) {
      if (transcopyrightInfo) {
        return "[🔒 " + std::to_string(transcopyrightInfo->priceAtomicUnits) +
               " " + transcopyrightInfo->currencySymbol + "]";
      }
      return "[🔒 Locked]";
    }
    if (span.empty()) {
      return {};
    }
    if (span.scroll == xudu::localScroll) {
      return std::string(primedia.readView(span));
    }
    const auto scrollIdx = static_cast<std::size_t>(span.scroll - 1);
    if (scrollIdx < externals.size()) {
      const auto res = resolver.resolve(externals[scrollIdx], span);
      if (res.status == xudu::ResolutionStatus::VerifiedBytes) {
        return res.text;
      }
      if (res.status == xudu::ResolutionStatus::WithheldRedacted) {
        return "[Redacted - Withheld]";
      }
      if (res.status == xudu::ResolutionStatus::TranscopyrightLocked) {
        if (res.lockInfo) {
          return "[🔒 " + std::to_string(res.lockInfo->priceAtomicUnits) + " " +
                 res.lockInfo->currencySymbol + "]";
        }
        return "[🔒 Locked]";
      }
    }
    return {};
  }

  [[nodiscard]] std::string
  readText(const xudu::SegmentedPrimediaSpool &primedia,
           const xudu::Resolver &resolver,
           const std::vector<xudu::Scroll> &externals) const {
    if (!ephemeralText.empty()) {
      return ephemeralText;
    }
    if (isWithheld()) {
      return "[Redacted - Withheld]";
    }
    if (isTranscopyrightLocked()) {
      if (transcopyrightInfo) {
        return "[🔒 " + std::to_string(transcopyrightInfo->priceAtomicUnits) +
               " " + transcopyrightInfo->currencySymbol + "]";
      }
      return "[🔒 Locked]";
    }
    if (span.empty()) {
      return {};
    }
    if (span.scroll == xudu::localScroll) {
      return std::string(primedia.readView(span));
    }
    const auto scrollIdx = static_cast<std::size_t>(span.scroll - 1);
    if (scrollIdx < externals.size()) {
      const auto res = resolver.resolve(externals[scrollIdx], span);
      if (res.status == xudu::ResolutionStatus::VerifiedBytes) {
        return res.text;
      }
      if (res.status == xudu::ResolutionStatus::WithheldRedacted) {
        return "[Redacted - Withheld]";
      }
      if (res.status == xudu::ResolutionStatus::TranscopyrightLocked) {
        if (res.lockInfo) {
          return "[🔒 " + std::to_string(res.lockInfo->priceAtomicUnits) + " " +
                 res.lockInfo->currencySymbol + "]";
        }
        return "[🔒 Locked]";
      }
    }
    return {};
  }

  /**
   * @brief Zero-copy view into local primedia spool memory.
   */
  [[nodiscard]] std::string_view
  resolveLocalView(const xudu::PrimediaSpool &primedia) const noexcept {
    if (!ephemeralText.empty()) {
      return ephemeralText;
    }
    if (span.empty() || span.scroll != xudu::localScroll) {
      return {};
    }
    return primedia.readView(span);
  }

  [[nodiscard]] std::string_view resolveLocalView(
      const xudu::SegmentedPrimediaSpool &primedia) const noexcept {
    if (!ephemeralText.empty()) {
      return ephemeralText;
    }
    if (span.empty() || span.scroll != xudu::localScroll) {
      return {};
    }
    return primedia.readView(span);
  }
};

// A ceiling, not a target. Its job is to make the next field somebody adds
// to CompactZZCell an explicit decision rather than a silent one -- the
// documented size was 64 and the real size had reached 960 without anyone
// having to notice. Lower it as the hot/cold split above gets done.
static_assert(sizeof(CompactZZCell) <= 1024,
              "CompactZZCell has grown past its budget. It is meant to be "
              "shrinking towards a cache-line-sized hot struct, not growing: "
              "put new cold fields in a side table keyed by CellID.");

} // namespace zigzag

#endif // ZIGZAG_COMPACT_ZZCELL_HPP
