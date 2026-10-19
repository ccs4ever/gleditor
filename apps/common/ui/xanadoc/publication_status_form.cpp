#include "publication_status_form.hpp"

#include "common/ui/identity_fields.hpp"
#include <utility>
#include <vector>

namespace xanadu {
namespace {
using Field = gleditor::Form::Field;
Field display(std::string label, std::string value) {
  return {.label              = std::move(label),
          .kind               = gleditor::Form::Kind::Choice,
          .options            = {value},
          .optionDescriptions = {std::move(value)}};
}
std::string verification(PublicationDownloadVerification value) {
  switch (value) {
  case PublicationDownloadVerification::Unchecked:
    return "Not checked";
  case PublicationDownloadVerification::ManifestVerified:
    return "Signed manifest verified; content incomplete";
  case PublicationDownloadVerification::Complete:
    return "Signed manifest and complete content verified";
  }
  std::unreachable();
}
std::string cleanupSummary(const PublicationPartialCleanupStatus &cleanup) {
  switch (cleanup.phase) {
  case PublicationPartialCleanupPhase::Pending:
    return "Partial-data cleanup is pending; refresh for its result";
  case PublicationPartialCleanupPhase::Complete:
    return "Discarded " + std::to_string(cleanup.removedLogicalBytes) +
           " logical file bytes of partial data; request and completed copies "
           "kept";
  case PublicationPartialCleanupPhase::Failed:
    return "Partial-data cleanup failed and may be incomplete: " +
           cleanup.error;
  }
  std::unreachable();
}
std::string requestSummary(const PublicationDownloadStatus &status) {
  auto note = status.pin && status.phase == PublicationDownloadPhase::Resolving
                  ? std::string("Fetching pinned publication")
                  : std::string(publicationDownloadPhaseName(status.phase));
  if (status.dependencyCount)
    note += " (" + std::to_string(status.completedDependencies) + "/" +
            std::to_string(status.dependencyCount) + " dependencies)";
  if (!status.error.empty()) note += ": " + status.error;
  return note;
}
} // namespace
std::string publicationCitationAvailability(
    const PublicationPin &pin,
    std::span<const PublicationDownloadStatus> requests) {
  bool checking = false, failed = false, cancelled = false;
  for (const auto &request : requests) {
    if (request.pin != pin) continue;
    if (request.phase == PublicationDownloadPhase::Ready &&
        request.verification == PublicationDownloadVerification::Complete)
      return "Complete local copy";
    if (request.phase == PublicationDownloadPhase::Failed)
      failed = true;
    else if (request.phase == PublicationDownloadPhase::Cancelled)
      cancelled = true;
    else
      checking = true;
  }
  if (checking) return "Checking";
  if (failed) return "Request failed; open status";
  if (cancelled) return "Request cancelled; retry available";
  return "Not checked";
}
std::string
publicationDownloadSummary(const PublicationDownloadStatus &status) {
  auto note = requestSummary(status);
  if (status.cleanup) {
    if (!note.ends_with('.')) note += '.';
    note += ' ' + cleanupSummary(*status.cleanup);
  }
  return note;
}
void openPublicationDownloadForm(
    gleditor::Form &form, const PublicationDownloadStatus &status,
    std::function<void(PublicationDownloadAction)> accepted,
    gleditor::Form::Cancelled cancelled) {
  Field action{.label         = "Action",
               .kind          = gleditor::Form::Kind::Choice,
               .options       = {"Refresh progress"},
               .submitOnEnter = true};
  std::vector<PublicationDownloadAction> kinds{
      PublicationDownloadAction::Refresh};
  const auto add = [&](const char *label, PublicationDownloadAction kind) {
    action.options.emplace_back(label);
    kinds.push_back(kind);
  };
  using Phase = PublicationDownloadPhase;
  if (status.phase == Phase::Ready &&
      status.verification == PublicationDownloadVerification::Complete) {
    add("Open completed publication", PublicationDownloadAction::Open);
    add("Notify me of updates", PublicationDownloadAction::Subscribe);
  } else if (status.phase == Phase::Failed ||
             status.phase == Phase::Cancelled) {
    if (!status.cleanup ||
        status.cleanup->phase != PublicationPartialCleanupPhase::Pending) {
      add("Retry download", PublicationDownloadAction::Retry);
      add("Discard partial data",
          PublicationDownloadAction::DiscardPartialData);
    }
  } else if (status.phase != Phase::Ready) {
    add("Cancel download", PublicationDownloadAction::Cancel);
  }
  add("Close", PublicationDownloadAction::Close);
  auto note = requestSummary(status);
  if (status.cleanup) {
    auto cleanup = cleanupSummary(*status.cleanup);
    if (!cleanup.ends_with('.')) cleanup += '.';
    // The form fits only three note lines. Keep the operation's outcome
    // first; accessibility retains both complete diagnostics if the tail clips.
    note = std::move(cleanup) + ' ' + note;
  }
  if (!note.ends_with('.')) note += '.';
  note += " Publisher enrollment is unchecked. Downloading never enters the "
          "publication.";
  if ((status.phase == Phase::Failed || status.phase == Phase::Cancelled) &&
      !status.cleanup)
    note += " Discard removes incomplete attempts only; the request and "
            "completed copies remain.";
  std::vector<Field> fields;
  fields.push_back(std::move(action));
  fields.push_back(display("Verification", verification(status.verification)));
  if (status.pin) {
    const auto &pin = *status.pin;
    fields.push_back(display("Requested carrier", pin.hash.hex()));
    fields.push_back(
        display("Requested edition", pin.title + " / " + pin.version.str() +
                                         " #" + std::to_string(pin.sequence)));
    fields.push_back(display("Publication name", pin.salt));
    fields.push_back(keyParts("Requested publisher", pin.publisher.hex()));
  } else {
    fields.push_back(display("Requested name", status.uri));
    fields.push_back(display("Requested carrier", "Awaiting signed manifest"));
  }
  form.open(
      "Download publication", std::move(note), std::move(fields),
      [kinds    = std::move(kinds),
       accepted = std::move(accepted)](const auto &answers) {
        if (!answers.empty() && answers[0].chosen < kinds.size())
          accepted(kinds[answers[0].chosen]);
      },
      std::move(cancelled));
}
} // namespace xanadu
