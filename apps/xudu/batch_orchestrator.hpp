/**
 * @file batch_orchestrator.hpp
 * @brief Decoupled batch CLI and headless orchestration pipeline for Xudu.
 */
#ifndef XUDU_BATCH_ORCHESTRATOR_HPP
#define XUDU_BATCH_ORCHESTRATOR_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <argparse/argparse.hpp>

#include "common/xanadu/format.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/mutable_link.hpp"
#include "common/xanadu/spool.hpp"
#include "xudu/session.hpp"

namespace xanadu {
class BatchOrchestrator {
public:
  static xanadu::LinkType parseLinkType(std::string_view str);
  static xanadu::ProminenceTier parseProminenceTier(std::string_view str);
  static xanadu::FormatAttribute parseFormatAttribute(std::string_view str);

  static std::vector<xanadu::PrimediaSpan> resolveSingleSpanToken(
      const xanadu::Session &session, const std::string &token,
      std::optional<std::uint32_t> defaultDocIdx = std::nullopt);

  static std::vector<xanadu::PrimediaSpan>
  resolveSpans(const xanadu::Session &session, const std::string &spec);

  struct ExecutionResult {
    bool shouldExit = false;
    int exitCode    = 0;
    MicroversionId opening;
    std::vector<std::pair<MicroversionId, std::size_t>> extraImports;
  };

  static ExecutionResult execute(Session &session,
                                 const argparse::ArgumentParser &parser,
                                 bool quiet = false);
};

} // namespace xanadu

#endif // XUDU_BATCH_ORCHESTRATOR_HPP
