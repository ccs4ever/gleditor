#ifndef XANADU_EVIDENCE_WORKFLOW_HPP
#define XANADU_EVIDENCE_WORKFLOW_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/evidence/evidence_case.hpp"

namespace xanadu::evidence {

enum class WorkflowStepKind : unsigned char { PureQuery, Effect };
enum class WorkflowStepState : unsigned char {
  Pending,
  Ready,
  Running,
  AwaitingReceipt,
  Accepted,
  Completed,
  Rejected,
  Failed,
  Partial,
  Unknown,
  Cancelled,
  Skipped
};
enum class WorkflowAttemptMode : unsigned char {
  Initial,
  ExplicitRetry,
  AcknowledgeUnknown
};

struct WorkflowStepDefinition {
  std::string key;
  std::string label;
  WorkflowStepKind kind{WorkflowStepKind::PureQuery};
  std::optional<RecordId> effectIntent;
};

struct WorkflowDecisionDefinition {
  std::string key;
  std::string label;
  std::vector<std::string> choices;
};

struct WorkflowDependency {
  std::string prerequisite;
  std::string step;
  std::optional<std::string> decision;
  std::optional<std::string> choice;
};

struct WorkflowDefinitionInput {
  std::string name;
  std::vector<WorkflowStepDefinition> steps;
  std::vector<WorkflowDependency> dependencies;
  std::vector<WorkflowDecisionDefinition> decisions;
};

struct WorkflowLimits {
  std::size_t maxSteps{128};
  std::size_t maxDependencies{512};
  std::size_t maxInputs{1024};
  std::size_t maxTransitions{10000};
  std::size_t maxMetadataBytes{65536};
  std::size_t maxEvidenceRecords{10000};
};

struct WorkflowRun {
  std::string documentId;
  RecordId documentRoot;
  RecordId id;
  std::uint64_t generation{};
};

struct WorkflowDecisionView {
  RecordId id;
  std::string key;
  std::string choice;
};

struct WorkflowStepView {
  std::string key;
  WorkflowStepState state{WorkflowStepState::Pending};
  std::vector<RecordId> attempts;
  std::vector<RecordId> outputs;
};

struct WorkflowRunView {
  WorkflowRun run;
  RecordId definition;
  std::vector<WorkflowDecisionView> decisions;
  std::vector<WorkflowStepView> steps;
  std::vector<RecordId> checkpoints;
  std::size_t revision{};
};

/// Append-only workflow records over an EvidenceCase. This class records and
/// inspects work; it never dispatches a step or stores executable callbacks.
class WorkflowJournal {
public:
  explicit WorkflowJournal(EvidenceCase &evidence, WorkflowLimits limits = {});
  ~WorkflowJournal();
  WorkflowJournal(const WorkflowJournal &)            = delete;
  WorkflowJournal &operator=(const WorkflowJournal &) = delete;

  [[nodiscard]] RecordId define(const WorkflowDefinitionInput &definition,
                                std::optional<RecordId> supersedes = {});
  [[nodiscard]] WorkflowRun startRun(RecordId definition,
                                     std::span<const RecordId> inputs,
                                     std::string owner,
                                     std::uint64_t generation);
  [[nodiscard]] WorkflowDecisionView
  decide(const WorkflowRun &run, std::string_view key, std::string_view choice,
         std::span<const RecordId> witnesses = {});
  [[nodiscard]] RecordId
  beginStep(const WorkflowRun &run, std::string_view step,
            WorkflowAttemptMode mode = WorkflowAttemptMode::Initial);
  [[nodiscard]] RecordId finishStep(const WorkflowRun &run, RecordId attempt,
                                    WorkflowStepState state,
                                    std::span<const RecordId> outputs = {},
                                    std::string_view detail           = {});
  [[nodiscard]] WorkflowStepState reconcileEffect(const WorkflowRun &run,
                                                  RecordId workflowAttempt,
                                                  RecordId effectAttempt);
  [[nodiscard]] WorkflowRunView inspectRun(const WorkflowRun &run) const;
  [[nodiscard]] WorkflowRunView resume(const WorkflowRun &run);
  [[nodiscard]] RecordId checkpoint(const WorkflowRun &run);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu::evidence

#endif
