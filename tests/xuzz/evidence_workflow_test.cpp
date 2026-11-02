#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/evidence/workflow.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu::evidence {
namespace {

class EvidenceWorkflowTest : public ::testing::Test {
protected:
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("evidence-workflow-" + UserPermascroll::generateUniqueDeviceId());
  std::filesystem::path casePath          = directory / "case";
  std::shared_ptr<UserPermascroll> scroll = std::make_shared<UserPermascroll>();
  bool failSave{};
  EvidenceCase evidence{
      scroll, [this](SavePhaseForTesting phase) {
        if (failSave && phase == SavePhaseForTesting::operationsCommitted)
          throw std::runtime_error("injected durable-save failure");
      }};

  void SetUp() override {
    std::filesystem::create_directories(directory);
    evidence.create(casePath, "workflow test");
  }

  void TearDown() override {
    if (evidence.isOpen()) evidence.close();
    std::filesystem::remove_all(directory);
  }

  RecordId record(std::string_view kind = "observation") {
    return evidence.appendRecord(kind, "input", "retained input");
  }

  WorkflowDefinitionInput simpleDefinition() const {
    return {.name         = "two step review",
            .steps        = {{.key = "capture", .label = "Capture"},
                             {.key = "review", .label = "Review"}},
            .dependencies = {{.prerequisite = "capture", .step = "review"}}};
  }

  WorkflowRun start(WorkflowJournal &journal, RecordId definition,
                    std::span<const RecordId> inputs = {}) {
    return journal.startRun(definition, inputs, "test-owner", 7);
  }
};

TEST_F(EvidenceWorkflowTest, BoundedDagRunsResumeAndCheckpointDurably) {
  const auto input = record();
  WorkflowJournal journal(evidence);
  const auto definition = journal.define(simpleDefinition());
  const std::array inputs{input};
  const auto run       = start(journal, definition, inputs);
  auto wrongGeneration = run;
  ++wrongGeneration.generation;
  EXPECT_THROW(static_cast<void>(journal.beginStep(wrongGeneration, "capture")),
               std::runtime_error);

  auto view = journal.inspectRun(run);
  ASSERT_EQ(view.steps.size(), 2U);
  EXPECT_EQ(view.steps[0].state, WorkflowStepState::Ready);
  EXPECT_EQ(view.steps[1].state, WorkflowStepState::Pending);

  const auto capture = journal.beginStep(run, "capture");
  const std::array outputs{input};
  static_cast<void>(journal.finishStep(run, capture,
                                       WorkflowStepState::Completed, outputs,
                                       "captured selected input"));
  view = journal.inspectRun(run);
  EXPECT_EQ(view.steps[0].state, WorkflowStepState::Completed);
  EXPECT_EQ(view.steps[0].outputs.size(), 1U);
  EXPECT_EQ(view.steps[0].outputs.front(), input);
  EXPECT_EQ(view.steps[1].state, WorkflowStepState::Ready);

  const auto review = journal.beginStep(run, "review");
  static_cast<void>(
      journal.finishStep(run, review, WorkflowStepState::Completed, outputs));
  const auto beforeCheckpoint = evidence.records().size();
  const auto checkpoint       = journal.checkpoint(run);
  EXPECT_EQ(journal.checkpoint(run), checkpoint);
  EXPECT_EQ(evidence.records().size(), beforeCheckpoint + 1);

  evidence.close();
  evidence.open(casePath);
  WorkflowJournal reopened(evidence);
  const auto resumed = reopened.resume(run);
  ASSERT_EQ(resumed.steps.size(), 2U);
  EXPECT_EQ(resumed.steps[0].state, WorkflowStepState::Completed);
  EXPECT_EQ(resumed.steps[1].state, WorkflowStepState::Completed);
  EXPECT_EQ(resumed.checkpoints, std::vector<RecordId>{checkpoint});
  EXPECT_EQ(reopened.checkpoint(run), checkpoint);
}

TEST_F(EvidenceWorkflowTest, InvalidDagAndReferencesRefuseBeforeAppend) {
  WorkflowJournal journal(evidence);
  auto cyclic = WorkflowDefinitionInput{
      .name         = "cycle",
      .steps        = {{.key = "a", .label = "A"}, {.key = "b", .label = "B"}},
      .dependencies = {{.prerequisite = "a", .step = "b"},
                       {.prerequisite = "b", .step = "a"}}};
  const auto beforeCycle = evidence.records().size();
  EXPECT_THROW(static_cast<void>(journal.define(cyclic)),
               std::invalid_argument);
  EXPECT_EQ(evidence.records().size(), beforeCycle);

  WorkflowLimits oneStepLimit;
  oneStepLimit.maxSteps = 1;
  WorkflowJournal limited(evidence, oneStepLimit);
  EXPECT_THROW(static_cast<void>(limited.define(simpleDefinition())),
               std::invalid_argument);
  EXPECT_EQ(evidence.records().size(), beforeCycle);

  auto badReference =
      WorkflowDefinitionInput{.name         = "bad input",
                              .steps        = {{.key = "a", .label = "A"}},
                              .dependencies = {}};
  const auto definition = journal.define(badReference);
  const auto beforeRun  = evidence.records().size();
  const std::array missing{RecordId{9000000U}};
  EXPECT_THROW(start(journal, definition, missing), std::out_of_range);
  EXPECT_EQ(evidence.records().size(), beforeRun);
}

TEST_F(EvidenceWorkflowTest, DecisionsRetainBranchWitnessAndSkipOtherBranch) {
  const auto witness = record();
  WorkflowDefinitionInput definitionInput{
      .name         = "branch review",
      .steps        = {{.key = "classify", .label = "Classify"},
                       {.key = "approve", .label = "Approve"},
                       {.key = "reject", .label = "Reject"}},
      .dependencies = {{.prerequisite = "classify",
                        .step         = "approve",
                        .decision     = "outcome",
                        .choice       = "approved"},
                       {.prerequisite = "classify",
                        .step         = "reject",
                        .decision     = "outcome",
                        .choice       = "rejected"}},
      .decisions    = {{.key     = "outcome",
                        .label   = "Review outcome",
                        .choices = {"approved", "rejected"}}}};
  WorkflowJournal journal(evidence);
  const auto definition = journal.define(definitionInput);
  const auto run        = start(journal, definition);
  const auto classify   = journal.beginStep(run, "classify");
  static_cast<void>(
      journal.finishStep(run, classify, WorkflowStepState::Completed));
  const std::array witnesses{witness};
  const auto decision = journal.decide(run, "outcome", "rejected", witnesses);
  EXPECT_EQ(evidence.inspect(decision.id).kind, "workflow-decision");
  const auto view = journal.inspectRun(run);
  EXPECT_EQ(view.steps[0].state, WorkflowStepState::Completed);
  EXPECT_EQ(view.steps[1].state, WorkflowStepState::Skipped);
  EXPECT_EQ(view.steps[2].state, WorkflowStepState::Ready);
  const auto skipped =
      std::ranges::find(view.steps, "approve", &WorkflowStepView::key);
  ASSERT_NE(skipped, view.steps.end());
  const auto records = evidence.records();
  const auto retainedSkip =
      std::ranges::find_if(records, [&](const Record &entry) {
        return entry.kind == "workflow-step-transition" &&
               entry.parent == run.id &&
               entry.attributes.at("step-key") == "approve";
      });
  ASSERT_NE(retainedSkip, records.end());
  EXPECT_EQ(retainedSkip->attributes.at("state"), "skipped");
  EXPECT_EQ(retainedSkip->attributes.at("decision-id"),
            std::to_string(decision.id));
  EXPECT_EQ(retainedSkip->parent, run.id);
}

TEST_F(EvidenceWorkflowTest,
       UnknownEffectRequiresExplicitNewAttemptAndAcceptanceIsNotCompletion) {
  const auto intent = record("effect-intent");
  WorkflowDefinitionInput input{.name  = "effect review",
                                .steps = {{.key   = "send",
                                           .label = "Send correction",
                                           .kind  = WorkflowStepKind::Effect,
                                           .effectIntent = intent}}};
  WorkflowJournal journal(evidence);
  const auto definition      = journal.define(input);
  const auto run             = start(journal, definition);
  const auto workflowAttempt = journal.beginStep(run, "send");
  const auto effectAttempt = evidence.appendRecord("effect-attempt", "admitted",
                                                   "durably admitted", intent);
  EXPECT_EQ(journal.reconcileEffect(run, workflowAttempt, effectAttempt),
            WorkflowStepState::Running);
  const auto acceptance = evidence.appendRecord(
      "effect-acceptance", "accepted", "local admission only", effectAttempt);
  EXPECT_EQ(journal.resume(run).steps[0].state,
            WorkflowStepState::AwaitingReceipt);
  EXPECT_EQ(journal.inspectRun(run).steps[0].state,
            WorkflowStepState::AwaitingReceipt);

  const auto unknown =
      evidence.appendRecord("effect-receipt", "unknown",
                            "executor lost after admission", effectAttempt);
  static_cast<void>(unknown);
  EXPECT_EQ(journal.resume(run).steps[0].state, WorkflowStepState::Unknown);
  EXPECT_EQ(evidence.inspect(acceptance).kind, "effect-acceptance");
  EXPECT_EQ(journal.reconcileEffect(run, workflowAttempt, effectAttempt),
            WorkflowStepState::Unknown);

  const auto unrelatedAttempt = evidence.appendRecord(
      "effect-attempt", "other", "unrelated attempt on same intent", intent);
  static_cast<void>(evidence.appendRecord("effect-receipt", "completed",
                                          "completed independently",
                                          unrelatedAttempt));
  const auto beforeRefusal = evidence.records().size();
  EXPECT_THROW(static_cast<void>(journal.reconcileEffect(run, workflowAttempt,
                                                         unrelatedAttempt)),
               std::runtime_error);
  EXPECT_EQ(evidence.records().size(), beforeRefusal);

  EXPECT_THROW(static_cast<void>(journal.beginStep(run, "send")),
               std::runtime_error);
  EXPECT_THROW(static_cast<void>(journal.beginStep(
                   run, "send", WorkflowAttemptMode::ExplicitRetry)),
               std::runtime_error);
  const auto retry =
      journal.beginStep(run, "send", WorkflowAttemptMode::AcknowledgeUnknown);
  EXPECT_NE(retry, workflowAttempt);
  EXPECT_EQ(journal.inspectRun(run).steps[0].attempts.size(), 2U);
  const auto retryEffectAttempt = evidence.appendRecord(
      "effect-attempt", "retry", "explicit retry attempt", intent);
  static_cast<void>(evidence.appendRecord("effect-receipt", "completed",
                                          "completed after explicit retry",
                                          retryEffectAttempt));
  EXPECT_EQ(journal.reconcileEffect(run, retry, retryEffectAttempt),
            WorkflowStepState::Completed);
}

TEST_F(EvidenceWorkflowTest, SaveFailureRetryReusesCheckpointRecord) {
  WorkflowJournal journal(evidence);
  const auto definition = journal.define(simpleDefinition());
  const auto run        = start(journal, definition);
  failSave              = true;
  EXPECT_THROW(static_cast<void>(journal.checkpoint(run)), std::runtime_error);
  failSave               = false;
  const auto beforeRetry = evidence.records().size();
  const auto checkpoint  = journal.checkpoint(run);
  EXPECT_EQ(evidence.records().size(), beforeRetry);
  EXPECT_EQ(journal.checkpoint(run), checkpoint);
}

TEST_F(EvidenceWorkflowTest, AcceptedReceiptIsTerminalWithoutClaimingDelivery) {
  const auto intent = record("effect-intent");
  WorkflowDefinitionInput input{
      .name         = "acceptance review",
      .steps        = {{.key          = "submit",
                        .label        = "Submit",
                        .kind         = WorkflowStepKind::Effect,
                        .effectIntent = intent},
                       {.key = "review", .label = "Review"}},
      .dependencies = {{.prerequisite = "submit", .step = "review"}}};
  WorkflowJournal journal(evidence);
  const auto definition      = journal.define(input);
  const auto run             = start(journal, definition);
  const auto workflowAttempt = journal.beginStep(run, "submit");
  const auto effectAttempt = evidence.appendRecord("effect-attempt", "admitted",
                                                   "durably admitted", intent);
  static_cast<void>(
      journal.reconcileEffect(run, workflowAttempt, effectAttempt));
  static_cast<void>(evidence.appendRecord("effect-acceptance", "accepted",
                                          "local executor admission",
                                          effectAttempt));
  EXPECT_EQ(journal.resume(run).steps[0].state,
            WorkflowStepState::AwaitingReceipt);

  const auto receipt = evidence.appendRecord(
      "effect-receipt", "accepted", "accepted by remote server", effectAttempt);
  const auto view = journal.resume(run);
  ASSERT_EQ(view.steps.size(), 2U);
  EXPECT_EQ(view.steps[0].state, WorkflowStepState::Accepted);
  EXPECT_EQ(view.steps[1].state, WorkflowStepState::Ready);
  EXPECT_THROW(static_cast<void>(journal.beginStep(run, "submit")),
               std::runtime_error);
  const auto records = evidence.records();
  const auto transition =
      std::ranges::find_if(records, [&](const Record &entry) {
        return entry.kind == "workflow-step-transition" &&
               entry.attributes.contains("effect-receipt-id") &&
               entry.attributes.at("effect-receipt-id") ==
                   std::to_string(receipt);
      });
  ASSERT_NE(transition, records.end());
  EXPECT_EQ(transition->text, "accepted by remote server");
  EXPECT_EQ(transition->text.find("delivered"), std::string::npos);
}

TEST_F(EvidenceWorkflowTest,
       ReplacingCaseWithSameRootCellIdFailsDocumentFence) {
  WorkflowJournal journal(evidence);
  const auto definition   = journal.define(simpleDefinition());
  const auto run          = start(journal, definition);
  const auto originalRoot = run.documentRoot;
  const auto originalDoc  = run.documentId;

  evidence.close();
  const auto replacementPath = directory / "replacement";
  evidence.create(replacementPath, "replacement case");
  const auto replacementRecords = evidence.records();
  const auto replacementRoot =
      std::ranges::find(replacementRecords, "case", &Record::kind);
  ASSERT_NE(replacementRoot, replacementRecords.end());
  EXPECT_EQ(replacementRoot->id, originalRoot);
  EXPECT_NE(evidence.store().documentId().str(), originalDoc);
  EXPECT_THROW(static_cast<void>(journal.inspectRun(run)), std::runtime_error);
  EXPECT_THROW(static_cast<void>(journal.checkpoint(run)), std::runtime_error);
}

} // namespace
} // namespace xanadu::evidence
