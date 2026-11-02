#ifndef COMMON_UI_EVIDENCE_EVIDENCE_CASE_PANEL_HPP
#define COMMON_UI_EVIDENCE_EVIDENCE_CASE_PANEL_HPP

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gleditor/ui/overlay.hpp>

#include "common/xanadu/evidence/evidence_controller.hpp"
#include "common/xanadu/system_docs.hpp"

namespace xanadu::evidence {

/// Accessible Xuzz surface for creating and working with a private evidence
/// case.
class EvidenceCasePanel : public gleditor::ui::ScreenOverlay {
public:
  explicit EvidenceCasePanel(std::shared_ptr<UserPermascroll> permascroll);

  EvidenceCasePanel *toggle();
  EvidenceCasePanel *setVisible(bool visible);
  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  prepare(const gleditor::ui::UiMetrics &, const gleditor::ui::Theme &);
  void drawFrame(gleditor::FrameContext &) override;
  EvidenceCasePanel *
  setPresentationConfig(const xanadu::ModalPresentationConfig &config);
  [[nodiscard]] EvidenceController &controller() noexcept {
    return controller_;
  }

private:
  struct MultilineInputState {
    std::size_t caret{};
    float horizontalScrollPx{}, verticalScrollPx{};
    std::optional<std::size_t> preferredColumn;
    bool followCaret{true};
  };

  void handle(const gleditor::ui::WidgetAction &action);
  void rebuild();
  bool run(std::string command);
  void clearSpanPreview(bool clearRange);
  void clearCredentialFields();

  EvidenceController controller_;
  std::string casePath_, caseTitle_, sourceName_, locator_, query_, keyColumn_;
  std::string communicationName_, communicationOrigin_, communicationAccount_,
      communicationActor_;
  std::string mailName_, mailUrl_, mailAccount_, mailMailbox_{"INBOX"};
  std::string mailCaBundle_;
  std::string mailUsername_, mailPassword_, mailLeaseMilliseconds_{"300000"};
  std::string smtpName_, smtpUrl_, smtpAccount_, smtpTimeoutMs_{"5000"},
      smtpCaBundle_, smtpUsername_, smtpPassword_,
      smtpLeaseMilliseconds_{"300000"};
  std::string ircName_, ircHost_, ircPort_{"6697"}, ircAccount_,
      ircNetworkScope_, ircNick_, ircUsername_, ircRealName_, ircCaBundle_,
      ircTimeoutMs_{"10000"}, ircLineBudget_{"8192"}, ircTagBudget_{"4096"},
      ircBatchDepth_{"16"}, ircEventBudget_{"4096"},
      ircReadTargetBudget_{"256"}, ircPendingReadBudget_{"64"},
      ircPendingRecordBudget_{"4096"}, ircPendingByteBudget_{"1048576"},
      ircSaslUsername_, ircSaslPassword_, ircCredentialLifetime_{"300000"},
      ircTarget_, ircAfterMessageId_, ircMessageLimit_{"100"},
      ircHistoryLimit_{"100"};
  std::string mailDraftFrom_, mailDraftSubject_, mailDraftTo_, mailDraftCc_,
      mailDraftBcc_, mailDraftText_, mailDraftHtml_;
  MultilineInputState mailDraftToState_, mailDraftCcState_, mailDraftBccState_,
      mailDraftTextState_, mailDraftHtmlState_;
  std::string ircDraftConnection_, ircDraftTarget_, ircDraftPayload_;
  MultilineInputState ircDraftPayloadState_;
  std::size_t smtpConnectionPage_{}, mailDraftPage_{};
  std::size_t ircPage_{};
  std::size_t communicationPage_{};
  std::string noteText_, spanOffset_, spanLength_, compareBefore_,
      compareAfter_;
  std::string quoteText_, evidenceFilter_;
  std::string effectTarget_, effectDelay_, effectUnknown_, effectExecutable_,
      effectCwd_, effectArgs_, effectSql_, effectGuard_, effectExpected_,
      effectParams_, effectGuardParams_, historySource_, historyText_,
      effectIntentId_, effectGrantId_;
  std::string joinLeftObservation_, joinRightObservation_, joinLeftKey_,
      joinRightKey_;
  std::string joinKind_{"inner"}, joinCardinality_{"many"}, joinNulls_{"never"};
  std::string joinPairs_, joinWork_, joinRows_, joinBytes_, joinScan_,
      joinRetained_, joinMetadata_;
  std::string joinScanBytes_;
  std::size_t joinPage_{};
  std::string windowObservation_, windowTimeBasis_{"event"}, windowTimeColumn_,
      windowArrivalColumn_, windowKeyColumn_, windowStartMs_, windowEndMs_,
      windowWatermarkMs_, windowLatePolicy_{"exclude"}, windowOutput_{"1000"},
      windowRetained_{"10000"}, windowWork_{"1000000"}, windowScan_{"100000"},
      windowScanBytes_{"8388608"};
  std::size_t windowPage_{};
  std::string effectGrantLifetime_{"300000"};
  std::string effectDuplicateAck_;
  std::vector<std::string> spanPreviewDetails_;
  std::size_t quoteTextCaret_{}, spanOffsetCaret_{}, spanLengthCaret_{},
      evidenceFilterCaret_{};
  std::string exportPath_, exportId_, refreshName_;
  std::string feedback_;
  std::string effectMode_{"fake"};
  std::optional<RecordId> spanTarget_;
  std::optional<SpanSelection> spanRange_;
  std::optional<RecordId> locatedTarget_;
  std::optional<SpanSelection> locatedRange_;
  std::size_t page_{};
  std::size_t workPage_{};
  bool showMetadata_{};
  std::optional<gleditor::ui::Rect> panelBounds_;
  xanadu::ModalPresentationConfig config_{840.0F, 720.0F, 0.95F, 0.9F};
};

} // namespace xanadu::evidence
#endif
