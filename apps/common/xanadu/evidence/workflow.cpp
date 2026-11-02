#include "common/xanadu/evidence/workflow.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <queue>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace xanadu::evidence {
namespace {
std::string recordIdText(RecordId id) { return std::to_string(id); }

RecordId parseRecordId(std::string_view text) {
  RecordId id{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), id);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::runtime_error("invalid workflow record reference");
  return id;
}

std::uint64_t parseGeneration(std::string_view text) {
  std::uint64_t value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::runtime_error("invalid workflow generation");
  return value;
}

std::size_t parseSize(std::string_view text) {
  std::size_t value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::runtime_error("invalid workflow size");
  return value;
}

bool validName(std::string_view value) {
  return !value.empty() && value.size() <= 256 &&
         value.find_first_of("\r\n") == std::string_view::npos &&
         value.find('\0') == std::string_view::npos;
}

std::string_view stateName(WorkflowStepState state) {
  switch (state) {
  case WorkflowStepState::Pending:
    return "pending";
  case WorkflowStepState::Ready:
    return "ready";
  case WorkflowStepState::Running:
    return "running";
  case WorkflowStepState::AwaitingReceipt:
    return "awaiting-receipt";
  case WorkflowStepState::Accepted:
    return "accepted";
  case WorkflowStepState::Completed:
    return "completed";
  case WorkflowStepState::Rejected:
    return "rejected";
  case WorkflowStepState::Failed:
    return "failed";
  case WorkflowStepState::Partial:
    return "partial";
  case WorkflowStepState::Unknown:
    return "unknown";
  case WorkflowStepState::Cancelled:
    return "cancelled";
  case WorkflowStepState::Skipped:
    return "skipped";
  }
  throw std::invalid_argument("invalid workflow step state");
}

WorkflowStepState parseState(std::string_view value) {
  for (const auto state :
       {WorkflowStepState::Pending, WorkflowStepState::Ready,
        WorkflowStepState::Running, WorkflowStepState::AwaitingReceipt,
        WorkflowStepState::Accepted, WorkflowStepState::Completed,
        WorkflowStepState::Rejected, WorkflowStepState::Failed,
        WorkflowStepState::Partial, WorkflowStepState::Unknown,
        WorkflowStepState::Cancelled, WorkflowStepState::Skipped})
    if (stateName(state) == value) return state;
  throw std::runtime_error("unknown workflow step state");
}

WorkflowStepKind parseStepKind(std::string_view value) {
  if (value == "pure-query") return WorkflowStepKind::PureQuery;
  if (value == "effect") return WorkflowStepKind::Effect;
  throw std::runtime_error("unknown workflow step kind");
}

std::string_view stepKindName(WorkflowStepKind kind) {
  return kind == WorkflowStepKind::Effect ? "effect" : "pure-query";
}

bool satisfiesDependency(WorkflowStepState state) {
  return state == WorkflowStepState::Completed ||
         state == WorkflowStepState::Accepted;
}

bool isTerminalState(WorkflowStepState state) {
  switch (state) {
  case WorkflowStepState::Accepted:
  case WorkflowStepState::Completed:
  case WorkflowStepState::Rejected:
  case WorkflowStepState::Failed:
  case WorkflowStepState::Partial:
  case WorkflowStepState::Unknown:
  case WorkflowStepState::Cancelled:
  case WorkflowStepState::Skipped:
    return true;
  case WorkflowStepState::Pending:
  case WorkflowStepState::Ready:
  case WorkflowStepState::Running:
  case WorkflowStepState::AwaitingReceipt:
    return false;
  }
  return false;
}

std::string_view attemptModeName(WorkflowAttemptMode mode) {
  switch (mode) {
  case WorkflowAttemptMode::Initial:
    return "initial";
  case WorkflowAttemptMode::ExplicitRetry:
    return "explicit-retry";
  case WorkflowAttemptMode::AcknowledgeUnknown:
    return "acknowledge-unknown-risk";
  }
  throw std::invalid_argument("invalid workflow attempt mode");
}

std::optional<RecordId> referenceId(const Record &record,
                                    std::string_view attribute) {
  const auto found = record.attributes.find(std::string(attribute));
  if (found == record.attributes.end() || found->second.empty()) return {};
  return parseRecordId(found->second);
}

const Record &
requiredRecord(const std::unordered_map<RecordId, Record> &records,
               RecordId id) {
  const auto found = records.find(id);
  if (found == records.end())
    throw std::out_of_range("unknown workflow evidence record");
  return found->second;
}

std::uint64_t numberAttribute(const Record &record, std::string_view key) {
  const auto found = record.attributes.find(std::string(key));
  if (found == record.attributes.end())
    throw std::runtime_error("workflow record is missing a required attribute");
  return parseGeneration(found->second);
}

std::size_t checkedBytes(std::size_t current, std::size_t extra) {
  if (extra > std::numeric_limits<std::size_t>::max() - current)
    throw std::length_error("workflow metadata size overflow");
  return current + extra;
}
} // namespace

struct WorkflowJournal::Impl {
  EvidenceCase *evidence{};
  WorkflowLimits limits;
  std::string documentId;
  RecordId documentRoot{};

  Impl(EvidenceCase &active, WorkflowLimits configured)
      : evidence(&active), limits(validatedLimits(configured)),
        documentId(active.store().documentId().str()),
        documentRoot(caseDocument(active, limits.maxEvidenceRecords)) {}

  static WorkflowLimits validatedLimits(WorkflowLimits configured) {
    if (!configured.maxSteps || !configured.maxDependencies ||
        !configured.maxInputs || !configured.maxTransitions ||
        !configured.maxMetadataBytes || !configured.maxEvidenceRecords)
      throw std::invalid_argument("workflow limits must be positive");
    return configured;
  }

  static RecordId caseDocument(const EvidenceCase &active,
                               std::size_t maximum) {
    const auto page = active.recordIds(maximum);
    if (!page.complete)
      throw std::length_error("cannot identify an unbounded evidence case");
    std::optional<RecordId> found;
    for (const auto id : page.ids) {
      if (active.inspect(id).kind != "case") continue;
      if (found) throw std::runtime_error("evidence case has multiple roots");
      found = id;
    }
    if (!found) throw std::runtime_error("evidence case root is missing");
    return *found;
  }

  std::vector<Record> scan() const {
    const auto page = evidence->recordIds(limits.maxEvidenceRecords);
    if (!page.complete)
      throw std::length_error("workflow evidence scan limit exceeded");
    std::vector<Record> result;
    result.reserve(page.ids.size());
    for (const auto id : page.ids) result.push_back(evidence->inspect(id));
    return result;
  }

  std::unordered_map<RecordId, Record> index() const {
    auto all = scan();
    std::unordered_map<RecordId, Record> result;
    result.reserve(all.size());
    for (auto &record : all) result.emplace(record.id, std::move(record));
    return result;
  }

  void ensureCapacity(std::size_t existing, std::size_t adding) const {
    if (adding > limits.maxEvidenceRecords -
                     std::min(existing, limits.maxEvidenceRecords))
      throw std::length_error("workflow evidence record budget exceeded");
  }

  void ensureMetadata(std::size_t bytes) const {
    if (bytes > limits.maxMetadataBytes)
      throw std::length_error("workflow metadata byte budget exceeded");
  }

  RecordId currentDocument(const std::vector<Record> &records) const {
    std::optional<RecordId> found;
    for (const auto &record : records)
      if (record.kind == "case") {
        if (found) throw std::runtime_error("evidence case has multiple roots");
        found = record.id;
      }
    if (!found) throw std::runtime_error("evidence case root is missing");
    return *found;
  }

  void validateCurrentDocument(const std::vector<Record> &records) const {
    if (evidence->store().documentId().str() != documentId ||
        currentDocument(records) != documentRoot)
      throw std::runtime_error("workflow document fence mismatch");
  }

  const Record &validateRun(const WorkflowRun &run,
                            const std::vector<Record> &records) const {
    validateCurrentDocument(records);
    if (run.documentId != documentId || run.documentRoot != documentRoot)
      throw std::runtime_error("workflow document fence mismatch");
    const auto found = std::ranges::find(records, run.id, &Record::id);
    if (found == records.end() || found->kind != "workflow-run")
      throw std::out_of_range("workflow run is unavailable");
    if (referenceId(*found, "workflow-document") != documentRoot ||
        found->attributes.at("document-id") != documentId ||
        numberAttribute(*found, "generation") != run.generation)
      throw std::runtime_error("workflow generation fence mismatch");
    return *found;
  }

  WorkflowDefinitionInput
  loadDefinition(RecordId id, const std::vector<Record> &records) const {
    const auto found = std::ranges::find(records, id, &Record::id);
    if (found == records.end() || found->kind != "workflow-definition")
      throw std::out_of_range("workflow definition is unavailable");
    WorkflowDefinitionInput result;
    result.name = found->label;
    for (const auto &record : records) {
      if (record.parent != id) continue;
      if (record.kind == "workflow-step") {
        WorkflowStepDefinition step{
            .key   = record.attributes.at("step-key"),
            .label = record.label,
            .kind  = parseStepKind(record.attributes.at("step-kind"))};
        step.effectIntent = referenceId(record, "effect-intent-id");
        result.steps.push_back(std::move(step));
      } else if (record.kind == "workflow-decision-definition") {
        WorkflowDecisionDefinition decision{
            .key = record.attributes.at("decision-key"), .label = record.label};
        const auto count = parseSize(record.attributes.at("choice-count"));
        if (count > limits.maxSteps)
          throw std::length_error("workflow decision choice limit exceeded");
        for (std::size_t i = 0; i < count; ++i)
          decision.choices.push_back(
              record.attributes.at("choice-" + std::to_string(i)));
        result.decisions.push_back(std::move(decision));
      } else if (record.kind == "workflow-dependency") {
        WorkflowDependency dependency{.prerequisite =
                                          record.attributes.at("prerequisite"),
                                      .step = record.attributes.at("step")};
        if (record.attributes.contains("decision")) {
          dependency.decision = record.attributes.at("decision");
          dependency.choice   = record.attributes.at("choice");
        }
        result.dependencies.push_back(std::move(dependency));
      }
    }
    return result;
  }

  void validateDefinition(const WorkflowDefinitionInput &definition,
                          const std::unordered_map<RecordId, Record> &records,
                          std::size_t &metadata) const {
    if (!validName(definition.name) || definition.steps.empty() ||
        definition.steps.size() > limits.maxSteps ||
        definition.dependencies.size() > limits.maxDependencies ||
        definition.decisions.size() > limits.maxSteps)
      throw std::invalid_argument("workflow definition exceeds its bounds");
    metadata = checkedBytes(definition.name.size(), 128);
    std::unordered_map<std::string, std::size_t> steps;
    for (std::size_t i = 0; i < definition.steps.size(); ++i) {
      const auto &step = definition.steps[i];
      if (!validName(step.key) || !validName(step.label) ||
          !steps.emplace(step.key, i).second)
        throw std::invalid_argument("workflow step key or label is invalid");
      if (step.kind != WorkflowStepKind::PureQuery &&
          step.kind != WorkflowStepKind::Effect)
        throw std::invalid_argument("workflow step kind is invalid");
      metadata = checkedBytes(metadata, step.key.size() + step.label.size());
      metadata = checkedBytes(metadata, 64);
      if (step.kind == WorkflowStepKind::Effect) {
        if (!step.effectIntent ||
            requiredRecord(records, *step.effectIntent).kind != "effect-intent")
          throw std::invalid_argument(
              "effect workflow step needs an existing durable intent");
      } else if (step.effectIntent) {
        throw std::invalid_argument(
            "pure-query workflow step cannot pin an effect intent");
      }
    }
    std::unordered_map<std::string, std::set<std::string>> choices;
    for (const auto &decision : definition.decisions) {
      if (!validName(decision.key) || !validName(decision.label) ||
          decision.choices.size() < 2 ||
          !choices.emplace(decision.key, std::set<std::string>{}).second)
        throw std::invalid_argument("workflow decision is invalid");
      metadata =
          checkedBytes(metadata, decision.key.size() + decision.label.size());
      metadata      = checkedBytes(metadata, 64);
      auto &allowed = choices.at(decision.key);
      for (const auto &choice : decision.choices) {
        if (!validName(choice) || !allowed.insert(choice).second)
          throw std::invalid_argument("workflow decision choice is invalid");
        metadata = checkedBytes(metadata, choice.size());
        metadata = checkedBytes(metadata, 32);
      }
    }
    std::set<std::tuple<std::string, std::string, std::string, std::string>>
        uniqueEdges;
    std::vector<std::vector<std::size_t>> outgoing(definition.steps.size());
    std::vector<std::size_t> indegree(definition.steps.size());
    for (const auto &edge : definition.dependencies) {
      const auto from = steps.find(edge.prerequisite);
      const auto to   = steps.find(edge.step);
      if (from == steps.end() || to == steps.end() ||
          from->second == to->second)
        throw std::invalid_argument(
            "workflow dependency names an invalid step");
      if (edge.decision.has_value() != edge.choice.has_value())
        throw std::invalid_argument("workflow branch condition is incomplete");
      if (edge.decision) {
        const auto decision = choices.find(*edge.decision);
        if (decision == choices.end() ||
            !decision->second.contains(*edge.choice))
          throw std::invalid_argument("workflow branch condition is invalid");
      }
      const auto condition = edge.decision.value_or(std::string{});
      const auto choice    = edge.choice.value_or(std::string{});
      if (!uniqueEdges.emplace(edge.prerequisite, edge.step, condition, choice)
               .second)
        throw std::invalid_argument("workflow repeats a dependency");
      metadata =
          checkedBytes(metadata, edge.prerequisite.size() + edge.step.size() +
                                     condition.size() + choice.size());
      metadata = checkedBytes(metadata, 64);
      outgoing[from->second].push_back(to->second);
      ++indegree[to->second];
    }
    std::queue<std::size_t> ready;
    for (std::size_t i = 0; i < indegree.size(); ++i)
      if (!indegree[i]) ready.push(i);
    std::size_t visited = 0;
    while (!ready.empty()) {
      const auto step = ready.front();
      ready.pop();
      ++visited;
      for (const auto next : outgoing[step])
        if (--indegree[next] == 0) ready.push(next);
    }
    if (visited != definition.steps.size())
      throw std::invalid_argument("workflow dependencies contain a cycle");
    ensureMetadata(metadata);
  }

  std::size_t runEventCount(const std::vector<Record> &records,
                            RecordId runId) const {
    std::unordered_set<RecordId> attempts;
    for (const auto &record : records)
      if (record.kind == "workflow-step-attempt" && record.parent == runId)
        attempts.insert(record.id);
    return static_cast<std::size_t>(
        std::ranges::count_if(records, [&](const Record &record) {
          if (record.kind == "workflow-checkpoint") return false;
          return record.parent == runId ||
                 (record.parent && attempts.contains(*record.parent));
        }));
  }

  void ensureRunEvents(const std::vector<Record> &records, RecordId runId,
                       std::size_t adding) const {
    if (adding > limits.maxTransitions - std::min(runEventCount(records, runId),
                                                  limits.maxTransitions))
      throw std::length_error("workflow run transition budget exceeded");
  }
};

WorkflowJournal::WorkflowJournal(EvidenceCase &evidence, WorkflowLimits limits)
    : impl_(std::make_unique<Impl>(evidence, limits)) {}

WorkflowJournal::~WorkflowJournal() = default;

RecordId WorkflowJournal::define(const WorkflowDefinitionInput &definition,
                                 std::optional<RecordId> supersedes) {
  auto records = impl_->scan();
  impl_->validateCurrentDocument(records);
  const auto indexed = impl_->index();
  std::size_t metadata{};
  impl_->validateDefinition(definition, indexed, metadata);
  std::uint64_t version = 1;
  if (supersedes) {
    const auto &prior = requiredRecord(indexed, *supersedes);
    if (prior.kind != "workflow-definition" || prior.label != definition.name)
      throw std::invalid_argument(
          "workflow revision does not match its parent");
    version = numberAttribute(prior, "version") + 1;
  }
  impl_->ensureCapacity(records.size(), 1 + definition.steps.size() +
                                            definition.dependencies.size() +
                                            definition.decisions.size());

  const auto id = impl_->evidence->appendRecord(
      "workflow-definition", definition.name,
      "Immutable workflow definition version " + std::to_string(version), {},
      {{"version", std::to_string(version)}},
      supersedes ? std::vector<
                       std::pair<std::string, RecordId>>{{"workflow-supersedes",
                                                          *supersedes}}
                 : std::vector<std::pair<std::string, RecordId>>{});
  std::unordered_map<std::string, RecordId> stepIds;
  for (const auto &step : definition.steps) {
    std::map<std::string, std::string> attributes{
        {"step-key", step.key},
        {"step-kind", std::string(stepKindName(step.kind))}};
    std::vector<std::pair<std::string, RecordId>> references;
    if (step.effectIntent) {
      attributes.emplace("effect-intent-id", recordIdText(*step.effectIntent));
      references.emplace_back("workflow-effect-intent", *step.effectIntent);
    }
    stepIds.emplace(step.key,
                    impl_->evidence->appendRecord("workflow-step", step.label,
                                                  "", id, std::move(attributes),
                                                  std::move(references)));
  }
  std::unordered_map<std::string, RecordId> decisionIds;
  for (const auto &decision : definition.decisions) {
    std::map<std::string, std::string> attributes{
        {"decision-key", decision.key},
        {"choice-count", std::to_string(decision.choices.size())}};
    for (std::size_t i = 0; i < decision.choices.size(); ++i)
      attributes.emplace("choice-" + std::to_string(i), decision.choices[i]);
    decisionIds.emplace(decision.key,
                        impl_->evidence->appendRecord(
                            "workflow-decision-definition", decision.label, "",
                            id, std::move(attributes)));
  }
  for (const auto &edge : definition.dependencies) {
    std::map<std::string, std::string> attributes{
        {"prerequisite", edge.prerequisite}, {"step", edge.step}};
    std::vector<std::pair<std::string, RecordId>> references{
        {"workflow-prerequisite", stepIds.at(edge.prerequisite)},
        {"workflow-dependent", stepIds.at(edge.step)}};
    if (edge.decision) {
      attributes.emplace("decision", *edge.decision);
      attributes.emplace("choice", *edge.choice);
      references.emplace_back("workflow-branch-decision",
                              decisionIds.at(*edge.decision));
    }
    static_cast<void>(impl_->evidence->appendRecord(
        "workflow-dependency", edge.step, "", id, std::move(attributes),
        std::move(references)));
  }
  return id;
}

WorkflowRun WorkflowJournal::startRun(RecordId definition,
                                      std::span<const RecordId> inputs,
                                      std::string owner,
                                      std::uint64_t generation) {
  auto records = impl_->scan();
  impl_->validateCurrentDocument(records);
  const auto indexed = impl_->index();
  if (!generation || !validName(owner) ||
      inputs.size() > impl_->limits.maxInputs)
    throw std::invalid_argument(
        "workflow run identity or input bound is invalid");
  const auto definitionInput = impl_->loadDefinition(definition, records);
  std::size_t metadata       = owner.size() + recordIdText(definition).size() +
                         impl_->documentId.size() +
                         recordIdText(impl_->documentRoot).size() + 64;
  for (const auto input : inputs) {
    static_cast<void>(requiredRecord(indexed, input));
    metadata = checkedBytes(metadata, recordIdText(input).size() + 32);
  }
  impl_->ensureMetadata(metadata);
  impl_->ensureCapacity(records.size(), 1);
  std::vector<std::pair<std::string, RecordId>> references{
      {"workflow-definition", definition},
      {"workflow-document", impl_->documentRoot}};
  std::map<std::string, std::string> attributes{
      {"definition-id", recordIdText(definition)},
      {"document-id", impl_->documentId},
      {"document-root-id", recordIdText(impl_->documentRoot)},
      {"generation", std::to_string(generation)},
      {"owner", std::move(owner)},
      {"input-count", std::to_string(inputs.size())}};
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    references.emplace_back("workflow-input-" + std::to_string(i), inputs[i]);
    attributes.emplace("input-" + std::to_string(i), recordIdText(inputs[i]));
  }
  const auto id = impl_->evidence->appendRecord(
      "workflow-run", definitionInput.name + " run",
      "Pinned workflow execution record", {}, std::move(attributes),
      std::move(references));
  return {impl_->documentId, impl_->documentRoot, id, generation};
}

WorkflowRunView WorkflowJournal::inspectRun(const WorkflowRun &run) const {
  const auto records    = impl_->scan();
  const auto &runRecord = impl_->validateRun(run, records);
  const auto definitionId =
      parseRecordId(runRecord.attributes.at("definition-id"));
  const auto definition = impl_->loadDefinition(definitionId, records);
  WorkflowRunView view{.run = run, .definition = definitionId};
  std::unordered_map<std::string, std::size_t> stepIndex;
  for (const auto &step : definition.steps) {
    stepIndex.emplace(step.key, view.steps.size());
    view.steps.push_back({.key = step.key});
  }
  std::unordered_map<std::string, std::string> decisions;
  for (const auto &record : records) {
    if (record.parent != run.id) continue;
    if (record.kind == "workflow-decision") {
      const auto key = record.attributes.at("decision-key");
      decisions.insert_or_assign(key, record.attributes.at("choice"));
      view.decisions.push_back({.id     = record.id,
                                .key    = key,
                                .choice = record.attributes.at("choice")});
    } else if (record.kind == "workflow-checkpoint") {
      view.checkpoints.push_back(record.id);
    }
  }
  std::unordered_map<RecordId, const Record *> attempts;
  std::unordered_map<std::string, const Record *> latestTransitions;
  for (const auto &record : records) {
    if (record.kind == "workflow-step-attempt" && record.parent == run.id) {
      attempts.emplace(record.id, &record);
      const auto key = record.attributes.at("step-key");
      view.steps.at(stepIndex.at(key)).attempts.push_back(record.id);
      if (!latestTransitions.contains(key))
        latestTransitions.emplace(key, &record);
    } else if (record.kind == "workflow-step-transition" &&
               record.parent == run.id) {
      const auto key = record.attributes.at("step-key");
      latestTransitions.insert_or_assign(key, &record);
    }
  }
  for (const auto &record : records) {
    if (record.kind != "workflow-step-output" || !record.parent) continue;
    const auto attempt = attempts.find(*record.parent);
    if (attempt == attempts.end()) continue;
    const auto key = attempt->second->attributes.at("step-key");
    view.steps.at(stepIndex.at(key))
        .outputs.push_back(parseRecordId(record.attributes.at("output-id")));
  }

  for (auto &step : view.steps) {
    if (const auto last = latestTransitions.find(step.key);
        last != latestTransitions.end()) {
      if (last->second->kind == "workflow-step-attempt")
        step.state = WorkflowStepState::Running;
      else
        step.state = parseState(last->second->attributes.at("state"));
    }
    if (step.state != WorkflowStepState::Pending) continue;
    bool skipped              = false;
    bool blockedByDecision    = false;
    bool dependenciesComplete = true;
    bool hasDependency        = false;
    for (const auto &edge : definition.dependencies) {
      if (edge.step != step.key) continue;
      hasDependency = true;
      if (edge.decision) {
        const auto selected = decisions.find(*edge.decision);
        if (selected == decisions.end()) {
          blockedByDecision = true;
          continue;
        }
        if (selected->second != *edge.choice) {
          skipped = true;
          continue;
        }
      }
      if (!satisfiesDependency(
              view.steps.at(stepIndex.at(edge.prerequisite)).state))
        dependenciesComplete = false;
    }
    if (skipped) {
      step.state = WorkflowStepState::Skipped;
    } else if (!blockedByDecision && dependenciesComplete && hasDependency) {
      step.state = WorkflowStepState::Ready;
    } else if (!hasDependency) {
      step.state = WorkflowStepState::Ready;
    }
  }

  std::unordered_set<RecordId> runChildren;
  for (const auto &[id, record] : attempts) {
    static_cast<void>(record);
    runChildren.insert(id);
  }
  view.revision = static_cast<std::size_t>(
      std::ranges::count_if(records, [&](const Record &record) {
        if (record.kind == "workflow-checkpoint") return false;
        return record.parent == run.id ||
               (record.parent && runChildren.contains(*record.parent));
      }));
  return view;
}

WorkflowRunView WorkflowJournal::resume(const WorkflowRun &run) {
  const auto records = impl_->scan();
  static_cast<void>(impl_->validateRun(run, records));
  const auto indexed = impl_->index();
  std::vector<std::pair<RecordId, RecordId>> bindings;
  for (const auto &record : records) {
    if (record.kind != "workflow-effect-binding" || !record.parent) continue;
    const auto &attempt = requiredRecord(indexed, *record.parent);
    if (attempt.kind != "workflow-step-attempt" || attempt.parent != run.id)
      continue;
    const auto effectAttempt = referenceId(record, "effect-attempt-id");
    if (effectAttempt) bindings.emplace_back(attempt.id, *effectAttempt);
  }
  for (const auto &[workflowAttempt, effectAttempt] : bindings)
    static_cast<void>(reconcileEffect(run, workflowAttempt, effectAttempt));
  return inspectRun(run);
}

WorkflowDecisionView
WorkflowJournal::decide(const WorkflowRun &run, std::string_view key,
                        std::string_view choice,
                        std::span<const RecordId> witnesses) {
  auto records          = impl_->scan();
  const auto &runRecord = impl_->validateRun(run, records);
  const auto definition = impl_->loadDefinition(
      parseRecordId(runRecord.attributes.at("definition-id")), records);
  const auto found = std::ranges::find(definition.decisions, key,
                                       &WorkflowDecisionDefinition::key);
  if (found == definition.decisions.end() ||
      std::ranges::find(found->choices, choice) == found->choices.end() ||
      witnesses.size() > impl_->limits.maxInputs)
    throw std::invalid_argument("workflow decision key or choice is invalid");
  const auto current = inspectRun(run);
  if (std::ranges::find(current.decisions, key, &WorkflowDecisionView::key) !=
      current.decisions.end())
    throw std::invalid_argument("workflow decision is immutable within a run");
  const auto indexed   = impl_->index();
  std::size_t metadata = key.size() + choice.size();
  std::vector<std::pair<std::string, RecordId>> references;
  for (std::size_t i = 0; i < witnesses.size(); ++i) {
    static_cast<void>(requiredRecord(indexed, witnesses[i]));
    metadata = checkedBytes(metadata, recordIdText(witnesses[i]).size());
    metadata = checkedBytes(metadata, 32);
    references.emplace_back("workflow-witness-" + std::to_string(i),
                            witnesses[i]);
  }
  impl_->ensureMetadata(metadata);
  std::vector<std::string> skippedSteps;
  for (const auto &edge : definition.dependencies)
    if (edge.decision == key && edge.choice != choice)
      skippedSteps.push_back(edge.step);
  std::ranges::sort(skippedSteps);
  skippedSteps.erase(std::unique(skippedSteps.begin(), skippedSteps.end()),
                     skippedSteps.end());
  impl_->ensureCapacity(records.size(), 1 + skippedSteps.size());
  impl_->ensureRunEvents(records, run.id, 1 + skippedSteps.size());

  std::map<std::string, std::string> attributes{
      {"decision-key", std::string(key)},
      {"choice", std::string(choice)},
      {"generation", std::to_string(run.generation)}};
  const auto decisionId = impl_->evidence->appendRecord(
      "workflow-decision", std::string(choice),
      "Retained workflow branch decision", run.id, std::move(attributes),
      std::move(references));
  for (const auto &step : skippedSteps) {
    const auto prior =
        std::ranges::find(current.steps, step, &WorkflowStepView::key);
    if (prior == current.steps.end() ||
        prior->state != WorkflowStepState::Pending)
      continue;
    static_cast<void>(impl_->evidence->appendRecord(
        "workflow-step-transition", "skipped", "Unselected decision branch",
        run.id,
        {{"step-key", step},
         {"from-state", "pending"},
         {"state", "skipped"},
         {"generation", std::to_string(run.generation)},
         {"decision-id", recordIdText(decisionId)}},
        {{"workflow-decision", decisionId}}));
  }
  return {
      .id = decisionId, .key = std::string(key), .choice = std::string(choice)};
}

RecordId WorkflowJournal::beginStep(const WorkflowRun &run,
                                    std::string_view stepKey,
                                    WorkflowAttemptMode mode) {
  const auto records    = impl_->scan();
  const auto &runRecord = impl_->validateRun(run, records);
  const auto definition = impl_->loadDefinition(
      parseRecordId(runRecord.attributes.at("definition-id")), records);
  const auto step = std::ranges::find(definition.steps, stepKey,
                                      &WorkflowStepDefinition::key);
  if (step == definition.steps.end())
    throw std::invalid_argument("workflow step does not exist");
  const auto view = inspectRun(run);
  const auto current =
      std::ranges::find(view.steps, stepKey, &WorkflowStepView::key);
  if (current == view.steps.end())
    throw std::runtime_error("workflow step state is unavailable");
  switch (mode) {
  case WorkflowAttemptMode::Initial:
    if (current->state != WorkflowStepState::Ready)
      throw std::runtime_error(
          "workflow step is not ready for its first attempt");
    break;
  case WorkflowAttemptMode::ExplicitRetry:
    if (current->state != WorkflowStepState::Rejected &&
        current->state != WorkflowStepState::Failed)
      throw std::runtime_error(
          "explicit retry requires a rejected or failed step");
    if (step->kind == WorkflowStepKind::Effect) {
      const auto terminal = std::ranges::find_if(
          records.rbegin(), records.rend(), [&](const Record &record) {
            return record.kind == "workflow-step-transition" &&
                   record.parent == run.id &&
                   record.attributes.at("step-key") == stepKey &&
                   record.attributes.contains("effect-receipt-id");
          });
      if (terminal == records.rend())
        throw std::runtime_error("effect retry needs a terminal receipt "
                                 "proving it was not executed");
      const auto receiptId =
          parseRecordId(terminal->attributes.at("effect-receipt-id"));
      const auto indexed  = impl_->index();
      const auto &receipt = requiredRecord(indexed, receiptId);
      if (receipt.kind != "effect-receipt" ||
          !receipt.attributes.contains("may-have-executed") ||
          receipt.attributes.at("may-have-executed") != "no")
        throw std::runtime_error(
            "effect retry may duplicate an earlier external action");
    }
    break;
  case WorkflowAttemptMode::AcknowledgeUnknown:
    if (current->state != WorkflowStepState::Unknown)
      throw std::runtime_error(
          "unknown-outcome acknowledgement is not applicable");
    break;
  }
  std::map<std::string, std::string> attributes{
      {"step-key", std::string(stepKey)},
      {"attempt-number", std::to_string(current->attempts.size() + 1)},
      {"attempt-mode", std::string(attemptModeName(mode))},
      {"generation", std::to_string(run.generation)}};
  std::vector<std::pair<std::string, RecordId>> references;
  if (step->effectIntent) {
    attributes.emplace("effect-intent-id", recordIdText(*step->effectIntent));
    references.emplace_back("workflow-effect-intent", *step->effectIntent);
  }
  impl_->ensureMetadata(stepKey.size() + 64);
  impl_->ensureCapacity(records.size(), 2);
  impl_->ensureRunEvents(records, run.id, 2);
  const auto attempt = impl_->evidence->appendRecord(
      "workflow-step-attempt", std::string(stepKey),
      "Explicit workflow attempt", run.id, std::move(attributes),
      std::move(references));
  static_cast<void>(impl_->evidence->appendRecord(
      "workflow-step-transition", "running", "Step attempt started", run.id,
      {{"step-key", std::string(stepKey)},
       {"from-state", std::string(stateName(current->state))},
       {"state", "running"},
       {"generation", std::to_string(run.generation)},
       {"attempt-id", recordIdText(attempt)}},
      {{"workflow-attempt", attempt}}));
  return attempt;
}

RecordId WorkflowJournal::finishStep(const WorkflowRun &run, RecordId attemptId,
                                     WorkflowStepState state,
                                     std::span<const RecordId> outputs,
                                     std::string_view detail) {
  static_cast<void>(stateName(state));
  if (state == WorkflowStepState::Pending ||
      state == WorkflowStepState::Ready ||
      state == WorkflowStepState::Running ||
      state == WorkflowStepState::Skipped)
    throw std::invalid_argument(
        "workflow result state must be terminal or awaiting");
  const auto records    = impl_->scan();
  const auto &runRecord = impl_->validateRun(run, records);
  const auto indexed    = impl_->index();
  const auto &attempt   = requiredRecord(indexed, attemptId);
  if (attempt.kind != "workflow-step-attempt" || attempt.parent != run.id ||
      numberAttribute(attempt, "generation") != run.generation)
    throw std::runtime_error("workflow step attempt fence mismatch");
  const auto stepKey    = attempt.attributes.at("step-key");
  const auto definition = impl_->loadDefinition(
      parseRecordId(runRecord.attributes.at("definition-id")), records);
  const auto step = std::ranges::find(definition.steps, stepKey,
                                      &WorkflowStepDefinition::key);
  if (step == definition.steps.end())
    throw std::runtime_error("workflow attempt references an unknown step");
  if (step->kind == WorkflowStepKind::Effect)
    throw std::invalid_argument(
        "effect steps finish only from a retained terminal effect receipt");
  if (state == WorkflowStepState::AwaitingReceipt)
    throw std::invalid_argument(
        "pure-query step cannot await an effect receipt");
  const auto view = inspectRun(run);
  const auto current =
      std::ranges::find(view.steps, stepKey, &WorkflowStepView::key);
  if (current == view.steps.end() ||
      current->state != WorkflowStepState::Running ||
      current->attempts.empty() || current->attempts.back() != attemptId)
    throw std::runtime_error("workflow attempt is no longer active");
  if (outputs.size() > impl_->limits.maxInputs ||
      detail.size() > impl_->limits.maxMetadataBytes)
    throw std::length_error(
        "workflow result exceeds its input or metadata bound");
  for (const auto output : outputs)
    static_cast<void>(requiredRecord(indexed, output));
  impl_->ensureCapacity(records.size(), outputs.size() + 1);
  impl_->ensureRunEvents(records, run.id, outputs.size() + 1);
  std::size_t metadata = detail.size() + stepKey.size();
  for (const auto output : outputs)
    metadata = checkedBytes(metadata, recordIdText(output).size() + 64);
  impl_->ensureMetadata(metadata);
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    const auto outputId = outputs[i];
    static_cast<void>(impl_->evidence->appendRecord(
        "workflow-step-output", "output " + std::to_string(i),
        recordIdText(outputId), attemptId,
        {{"step-key", stepKey},
         {"output-index", std::to_string(i)},
         {"output-id", recordIdText(outputId)}},
        {{"workflow-output-" + std::to_string(i), outputId}}));
  }
  return impl_->evidence->appendRecord(
      "workflow-step-transition", std::string(stateName(state)), detail, run.id,
      {{"step-key", stepKey},
       {"from-state", "running"},
       {"state", std::string(stateName(state))},
       {"generation", std::to_string(run.generation)},
       {"attempt-id", recordIdText(attemptId)}},
      {{"workflow-attempt", attemptId}});
}

WorkflowStepState WorkflowJournal::reconcileEffect(const WorkflowRun &run,
                                                   RecordId workflowAttempt,
                                                   RecordId effectAttempt) {
  const auto records        = impl_->scan();
  const auto &runRecord     = impl_->validateRun(run, records);
  const auto indexed        = impl_->index();
  const auto &localAttempt  = requiredRecord(indexed, workflowAttempt);
  const auto &remoteAttempt = requiredRecord(indexed, effectAttempt);
  if (localAttempt.kind != "workflow-step-attempt" ||
      localAttempt.parent != run.id || remoteAttempt.kind != "effect-attempt")
    throw std::invalid_argument(
        "effect reconciliation references an invalid attempt");
  const auto stepKey    = localAttempt.attributes.at("step-key");
  const auto definition = impl_->loadDefinition(
      parseRecordId(runRecord.attributes.at("definition-id")), records);
  const auto step = std::ranges::find(definition.steps, stepKey,
                                      &WorkflowStepDefinition::key);
  if (step == definition.steps.end() ||
      step->kind != WorkflowStepKind::Effect ||
      step->effectIntent != remoteAttempt.parent)
    throw std::runtime_error("effect receipt does not match the pinned intent");
  if (numberAttribute(localAttempt, "generation") != run.generation)
    throw std::runtime_error("workflow effect attempt generation mismatch");
  for (const auto &record : records) {
    const bool binding = record.kind == "workflow-effect-binding" &&
                         record.parent == workflowAttempt;
    const bool transition =
        record.kind == "workflow-step-transition" && record.parent == run.id &&
        record.attributes.contains("attempt-id") &&
        record.attributes.at("attempt-id") == recordIdText(workflowAttempt);
    if ((!binding && !transition) ||
        !record.attributes.contains("effect-attempt-id"))
      continue;
    if (record.attributes.at("effect-attempt-id") !=
        recordIdText(effectAttempt))
      throw std::runtime_error(
          "workflow attempt is already bound to another effect attempt");
  }
  const Record *acceptance = nullptr;
  const Record *receipt    = nullptr;
  for (const auto &record : records) {
    if (record.parent != effectAttempt) continue;
    if (record.kind == "effect-acceptance") acceptance = &record;
    if (record.kind == "effect-receipt") receipt = &record;
  }
  const auto view = inspectRun(run);
  const auto current =
      std::ranges::find(view.steps, stepKey, &WorkflowStepView::key);
  if (current == view.steps.end() || current->attempts.empty() ||
      current->attempts.back() != workflowAttempt)
    throw std::runtime_error("workflow effect attempt is no longer current");
  WorkflowStepState state = current->state;
  const Record *evidence  = receipt ? receipt : acceptance;
  if (receipt) {
    const auto &outcome = receipt->label;
    if (outcome == "completed")
      state = WorkflowStepState::Completed;
    else if (outcome == "accepted")
      state = WorkflowStepState::Accepted;
    else if (outcome == "rejected")
      state = WorkflowStepState::Rejected;
    else if (outcome == "failed")
      state = WorkflowStepState::Failed;
    else if (outcome == "partial")
      state = WorkflowStepState::Partial;
    else if (outcome == "unknown")
      state = WorkflowStepState::Unknown;
    else if (outcome == "cancelled")
      state = WorkflowStepState::Cancelled;
    else
      throw std::runtime_error("effect receipt has an unsupported outcome");
  } else if (acceptance && !isTerminalState(state)) {
    state = WorkflowStepState::AwaitingReceipt;
  }
  if (!evidence) {
    const auto binding =
        std::ranges::find_if(records, [&](const Record &record) {
          return record.kind == "workflow-effect-binding" &&
                 record.parent == workflowAttempt &&
                 record.attributes.contains("effect-attempt-id") &&
                 record.attributes.at("effect-attempt-id") ==
                     recordIdText(effectAttempt);
        });
    if (binding != records.end()) return state;
    impl_->ensureCapacity(records.size(), 1);
    impl_->ensureRunEvents(records, run.id, 1);
    static_cast<void>(impl_->evidence->appendRecord(
        "workflow-effect-binding", "effect attempt",
        "Linked an existing durable effect attempt; no dispatch performed",
        workflowAttempt,
        {{"effect-attempt-id", recordIdText(effectAttempt)},
         {"generation", std::to_string(run.generation)}},
        {{"effect-attempt", effectAttempt}}));
    return state;
  }
  const auto priorTransition = std::ranges::find_if(
      records.rbegin(), records.rend(), [&](const Record &record) {
        return record.kind == "workflow-step-transition" &&
               record.parent == run.id &&
               record.attributes.at("step-key") == stepKey &&
               record.attributes.contains("effect-receipt-id") &&
               record.attributes.at("effect-receipt-id") ==
                   recordIdText(evidence->id);
      });
  if (priorTransition != records.rend()) return current->state;
  if (isTerminalState(current->state) && !receipt) return current->state;
  if (isTerminalState(current->state) &&
      current->state != WorkflowStepState::Unknown)
    throw std::runtime_error(
        "workflow already has a different terminal receipt");
  impl_->ensureMetadata(
      checkedBytes(checkedBytes(stepKey.size(), evidence->text.size()), 256));
  impl_->ensureCapacity(records.size(), 1);
  impl_->ensureRunEvents(records, run.id, 1);
  static_cast<void>(impl_->evidence->appendRecord(
      "workflow-step-transition", std::string(stateName(state)),
      receipt ? receipt->text
              : "Effect locally admitted; terminal receipt is pending",
      run.id,
      {{"step-key", stepKey},
       {"from-state", std::string(stateName(current->state))},
       {"state", std::string(stateName(state))},
       {"generation", std::to_string(run.generation)},
       {"attempt-id", recordIdText(workflowAttempt)},
       {"effect-attempt-id", recordIdText(effectAttempt)},
       {"effect-receipt-id", recordIdText(evidence->id)}},
      {{"workflow-attempt", workflowAttempt},
       {"effect-attempt", effectAttempt},
       {receipt ? "effect-receipt" : "effect-acceptance", evidence->id}}));
  return state;
}

RecordId WorkflowJournal::checkpoint(const WorkflowRun &run) {
  const auto view = inspectRun(run);
  if (std::ranges::any_of(view.steps, [](const WorkflowStepView &step) {
        return step.state == WorkflowStepState::Running;
      }))
    throw std::runtime_error("workflow checkpoint requires a quiescent run");
  const auto records    = impl_->scan();
  const auto &runRecord = impl_->validateRun(run, records);
  for (const auto &record : records) {
    if (record.kind == "workflow-checkpoint" && record.parent == run.id &&
        record.attributes.contains("run-revision") &&
        parseSize(record.attributes.at("run-revision")) == view.revision) {
      impl_->evidence->durableSave();
      return record.id;
    }
  }
  const auto definitionId =
      parseRecordId(runRecord.attributes.at("definition-id"));
  impl_->ensureMetadata(96);
  impl_->ensureCapacity(records.size(), 1);
  const auto id = impl_->evidence->appendRecord(
      "workflow-checkpoint", "checkpoint " + std::to_string(view.revision),
      "Quiescent workflow state; rederive execution positions from pinned "
      "evidence",
      run.id,
      {{"run-revision", std::to_string(view.revision)},
       {"definition-id", recordIdText(definitionId)},
       {"document-id", run.documentId},
       {"document-root-id", recordIdText(run.documentRoot)},
       {"generation", std::to_string(run.generation)},
       {"step-count", std::to_string(view.steps.size())}},
      {{"workflow-definition", definitionId},
       {"workflow-document", run.documentRoot}});
  impl_->evidence->durableSave();
  return id;
}
} // namespace xanadu::evidence
