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

namespace xudu {
using namespace ::xanadu;

class BatchOrchestrator {
public:
  static xudu::LinkType parseLinkType(std::string_view str);
  static xudu::ProminenceTier parseProminenceTier(std::string_view str);
  static xudu::FormatAttribute parseFormatAttribute(std::string_view str);

  static std::vector<xudu::PrimediaSpan> resolveSingleSpanToken(
      const xudu::Session &session, const std::string &token,
      std::optional<std::uint32_t> defaultDocIdx = std::nullopt);

  static std::vector<xudu::PrimediaSpan>
  resolveSpans(const xudu::Session &session, const std::string &spec);

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

} // namespace xudu

#endif // XUDU_BATCH_ORCHESTRATOR_HPP
