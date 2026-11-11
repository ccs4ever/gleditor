#include "common/ui/evidence/evidence_case_panel.hpp"
#include "common/xanadu/evidence/query_composition.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <string_view>
#include <utility>

namespace xanadu::evidence {
namespace ui = gleditor::ui;
namespace {

std::string quote(std::string_view value) {
  std::string result{"\""};
  for (const char ch : value) {
    if (ch == '\\' || ch == '"') result.push_back('\\');
    result.push_back(ch);
  }
  result.push_back('"');
  return result;
}

bool containsFolded(std::string_view text, std::string_view query) {
  if (query.empty()) return true;
  if (text.size() < query.size()) return false;
  for (std::size_t offset = 0; offset + query.size() <= text.size(); ++offset) {
    bool equal = true;
    for (std::size_t index = 0; index < query.size(); ++index) {
      if (std::tolower(static_cast<unsigned char>(text[offset + index])) !=
          std::tolower(static_cast<unsigned char>(query[index]))) {
        equal = false;
        break;
      }
    }
    if (equal) return true;
  }
  return false;
}

std::vector<std::string> quotedArguments(std::string_view text) {
  std::istringstream input{std::string{text}};
  std::vector<std::string> values;
  std::string value;
  while (input >> std::ws && !input.eof()) {
    if (!(input >> std::quoted(value)))
      throw std::invalid_argument(
          "Arguments must be quoted as separate values");
    values.push_back(value);
  }
  return values;
}

std::optional<std::uint32_t> number(std::string_view value) {
  std::uint32_t parsed{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size())
    return std::nullopt;
  return parsed;
}

std::string escaped(std::string_view value) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  for (const unsigned char ch : value) {
    if (ch >= 0x20U && ch < 0x7FU && ch != '\\') {
      result.push_back(static_cast<char>(ch));
    } else {
      result += "\\x";
      result.push_back(digits[ch >> 4U]);
      result.push_back(digits[ch & 0x0FU]);
    }
  }
  return result;
}

std::string spanContext(std::string_view raw, SpanSelection span) {
  const auto start = static_cast<std::size_t>(span.offset);
  const auto end   = start + static_cast<std::size_t>(span.length);
  constexpr std::size_t contextBytes = 48;
  const auto before = start > contextBytes ? start - contextBytes : 0;
  const auto after  = std::min(end + contextBytes, raw.size());
  return "Context before: " + escaped(raw.substr(before, start - before)) +
         "\nContext after: " + escaped(raw.substr(end, after - end));
}

ui::Theme compactTheme(ui::Theme theme) {
  theme.paddingEm = std::min(theme.paddingEm, 0.25F);
  theme.gapEm     = std::min(theme.gapEm, 0.25F);
  return theme;
}

ui::UiMetrics modalMetrics(const ui::UiMetrics &metrics) {
  auto local   = metrics;
  local.chrome = {};
  return local;
}

ui::Rect panelBounds(const ui::UiMetrics &metrics,
                     const xanadu::ModalPresentationConfig &config) {
  const auto localMetrics = modalMetrics(metrics);
  const auto safe         = localMetrics.pixelSafeArea();
  const xanadu::ModalPresentationConfig defaults{840.0F, 720.0F, 0.95F, 0.9F};
  const auto share = [](const float value, const float fallback) {
    return std::isfinite(value) ? std::clamp(value, 0.1F, 1.0F) : fallback;
  };
  const auto length = [](const float value, const float fallback) {
    return value > 0 && std::isfinite(value) ? value : fallback;
  };
  const auto width  = std::floor(std::min(
      metrics.px(length(config.widthPx, defaults.widthPx)),
      safe.width * share(config.maxWidthShare, defaults.maxWidthShare)));
  const auto height = std::floor(std::min(
      metrics.px(length(config.heightPx, defaults.heightPx)),
      safe.height * share(config.maxHeightShare, defaults.maxHeightShare)));
  return metrics.rounded({safe.left + (safe.width - width) * 0.5F,
                          safe.bottom + (safe.height - height) * 0.5F, width,
                          height});
}

std::string limitsDescription(const CaptureLimits &limits) {
  return "Capture budget: up to " + std::to_string(limits.maxRows) + " rows, " +
         std::to_string(limits.maxColumns) + " columns, " +
         std::to_string(limits.maxPages) + " REST pages, " +
         std::to_string(limits.maxBytes) + " bytes, " +
         std::to_string(limits.timeoutMs) +
         " ms. Refresh retains its raw "
         "response in this private case.";
}

void addWrappedRows(std::vector<ui::ListRow> &rows, ui::WidgetId &id,
                    std::string_view text) {
  constexpr std::size_t maxBytes = 80;
  if (text.empty()) {
    rows.push_back({id++, "", {}, false});
    return;
  }
  std::size_t start = 0;
  while (start < text.size()) {
    auto end = std::min(start + maxBytes, text.size());
    if (end < text.size()) {
      while (end > start &&
             (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
        --end;
      const auto space = text.rfind(' ', end);
      if (space != std::string_view::npos && space > start + maxBytes / 2)
        end = space;
    }
    if (end == start) end = std::min(start + maxBytes, text.size());
    rows.push_back(
        {id++, std::string{text.substr(start, end - start)}, {}, true});
    start = end;
    while (start < text.size() && text[start] == ' ') ++start;
  }
}

void addDetailRows(std::vector<ui::ListRow> &rows, ui::WidgetId &id,
                   const std::vector<std::string> &details) {
  for (const auto &detail : details) {
    std::size_t lineStart = 0;
    do {
      const auto lineEnd = detail.find('\n', lineStart);
      addWrappedRows(rows, id,
                     std::string_view(detail).substr(
                         lineStart, lineEnd == std::string::npos
                                        ? std::string::npos
                                        : lineEnd - lineStart));
      if (lineEnd == std::string::npos) break;
      lineStart = lineEnd + 1;
    } while (lineStart <= detail.size());
  }
}

ui::Widget detailList(ui::WidgetId widgetId,
                      const std::vector<std::string> &details,
                      ui::WidgetId firstRowId, std::string placeholder) {
  ui::Widget result{.id = widgetId, .model = ui::List{}};
  auto &rows         = std::get<ui::List>(result.model).rows;
  auto visibleDetail = details;
  if (visibleDetail.empty()) visibleDetail.push_back(std::move(placeholder));
  addDetailRows(rows, firstRowId, visibleDetail);
  return result;
}

ui::Widget label(ui::WidgetId id, std::string text) {
  return {.id = id, .model = ui::Label{std::move(text)}};
}

ui::Widget descriptionLabel(ui::WidgetId id, std::string text,
                            const std::uint16_t maxLines) {
  return {.id       = id,
          .model    = ui::Label{std::move(text), ui::TextPurpose::Description},
          .maxLines = maxLines};
}

ui::Widget field(ui::WidgetId id, std::string caption, const std::string &value,
                 std::string action,
                 const std::size_t caret = std::string::npos) {
  return {.id = id,
          .model =
              ui::TextField{value, std::move(caption), std::move(action),
                            caret == std::string::npos ? value.size() : caret}};
}

ui::Widget multilineField(const ui::WidgetId id, std::string caption,
                          const std::string &value, std::string action,
                          const std::size_t caret,
                          const float horizontalScrollPx,
                          const float verticalScrollPx,
                          const std::optional<std::size_t> preferredColumn,
                          const bool followCaret, const float height) {
  auto widget = field(id, std::move(caption), value, std::move(action), caret);
  auto &input = std::get<ui::TextField>(widget.model);
  input.multiline         = true;
  input.scrollPx          = horizontalScrollPx;
  input.verticalScrollPx  = verticalScrollPx;
  input.preferredColumn   = preferredColumn;
  input.followCaret       = followCaret;
  widget.preferred.height = height;
  return widget;
}

ui::Widget button(ui::WidgetId id, std::string caption, std::string action) {
  return {.id = id, .model = ui::Button{std::move(caption), std::move(action)}};
}

void addFields(ui::Widget &root, std::initializer_list<ui::Widget> fields) {
  root.children.insert(root.children.end(), fields.begin(), fields.end());
}

} // namespace

EvidenceCasePanel::EvidenceCasePanel(
    std::shared_ptr<UserPermascroll> permascroll)
    : ui::ScreenOverlay(
          ui::Widget{.id = 1, .model = ui::Modal{"Evidence case"}}),
      controller_(std::move(permascroll)) {
  const SqliteObservationJoinRequest joinDefaults;
  joinPairs_     = std::to_string(joinDefaults.joinLimits.maxOutputPairs);
  joinWork_      = std::to_string(joinDefaults.joinLimits.maxWork);
  joinRows_      = std::to_string(joinDefaults.joinLimits.maxInputRows);
  joinBytes_     = std::to_string(joinDefaults.joinLimits.maxKeyBytes);
  joinScan_      = std::to_string(joinDefaults.maxScanRecords);
  joinScanBytes_ = std::to_string(joinDefaults.maxScanBytes);
  joinRetained_  = std::to_string(joinDefaults.maxRetainedRecords);
  joinMetadata_  = std::to_string(joinDefaults.maxMetadataBytes);
  setVisible(false);
  setActionHandler([this](const ui::WidgetAction &action) { handle(action); });
  rebuild();
}

EvidenceCasePanel *EvidenceCasePanel::toggle() {
  return setVisible(!visible());
}

EvidenceCasePanel *EvidenceCasePanel::setVisible(const bool visible) {
  if (!visible) clearCredentialFields();
  ui::ScreenOverlay::setVisible(visible);
  rebuild();
  return this;
}

void EvidenceCasePanel::clearCredentialFields() {
  std::fill(mailUsername_.begin(), mailUsername_.end(), '\0');
  std::fill(mailPassword_.begin(), mailPassword_.end(), '\0');
  std::fill(smtpUsername_.begin(), smtpUsername_.end(), '\0');
  std::fill(smtpPassword_.begin(), smtpPassword_.end(), '\0');
  std::fill(ircSaslUsername_.begin(), ircSaslUsername_.end(), '\0');
  std::fill(ircSaslPassword_.begin(), ircSaslPassword_.end(), '\0');
  mailUsername_.clear();
  mailPassword_.clear();
  smtpUsername_.clear();
  smtpPassword_.clear();
  ircSaslUsername_.clear();
  ircSaslPassword_.clear();
}

std::shared_ptr<const ui::WidgetScene>
EvidenceCasePanel::prepare(const ui::UiMetrics &metrics,
                           const ui::Theme &theme) {
  const auto localMetrics = modalMetrics(metrics);
  const auto bounds       = panelBounds(localMetrics, config_);
  if (!panelBounds_ || panelBounds_->left != bounds.left ||
      panelBounds_->bottom != bounds.bottom ||
      panelBounds_->width != bounds.width ||
      panelBounds_->height != bounds.height) {
    panelBounds_ = bounds;
    setBounds(bounds);
  }
  return ui::ScreenOverlay::prepare(localMetrics, compactTheme(theme));
}

void EvidenceCasePanel::drawFrame(gleditor::FrameContext &context) {
  auto metrics         = context.metrics;
  metrics.screenWidth  = context.screenWidth;
  metrics.screenHeight = context.screenHeight;
  metrics              = modalMetrics(metrics);
  const auto bounds    = panelBounds(metrics, config_);
  if (!panelBounds_ || panelBounds_->left != bounds.left ||
      panelBounds_->bottom != bounds.bottom ||
      panelBounds_->width != bounds.width ||
      panelBounds_->height != bounds.height) {
    panelBounds_ = bounds;
    setBounds(bounds);
  }
  const auto theme = compactTheme(context.theme);
  gleditor::FrameContext themedContext{context.state,
                                       context.viewProjection,
                                       context.screenWidth,
                                       context.screenHeight,
                                       context.timeline,
                                       context.chrome,
                                       context.settledChrome,
                                       metrics,
                                       theme,
                                       context.contentRegion};
  ui::ScreenOverlay::drawFrame(themedContext);
}

EvidenceCasePanel *EvidenceCasePanel::setPresentationConfig(
    const xanadu::ModalPresentationConfig &config) {
  if (config_ != config) {
    config_ = config;
    panelBounds_.reset();
    invalidate();
  }
  return this;
}

bool EvidenceCasePanel::run(std::string command) {
  try {
    const bool accepted = controller_.execute(command);
    const auto state    = controller_.snapshot();
    feedback_           = state.status;
    rebuild();
    return accepted;
  } catch (const std::exception &error) {
    feedback_ = error.what();
    rebuild();
    return false;
  }
}

void EvidenceCasePanel::clearSpanPreview(const bool clearRange) {
  spanTarget_.reset();
  spanRange_.reset();
  locatedTarget_.reset();
  locatedRange_.reset();
  spanPreviewDetails_.clear();
  if (clearRange) {
    spanOffset_.clear();
    spanLength_.clear();
  }
}

void EvidenceCasePanel::handle(const ui::WidgetAction &action) {
  if (action.action.starts_with("page:")) {
    clearCredentialFields();
    const auto page = number(std::string_view(action.action).substr(5));
    if (page && *page < 9) page_ = *page;
    rebuild();
    return;
  }
  if (action.action.starts_with("communication-screen:")) {
    clearCredentialFields();
    const auto screen = number(std::string_view(action.action).substr(21));
    if (screen && *screen < 8) {
      communicationPage_ = *screen;
      if (*screen == 4) smtpConnectionPage_ = 0;
      if (*screen == 5) mailDraftPage_ = 0;
      if (*screen == 7) ircPage_ = 0;
    }
    rebuild();
    return;
  }
  if (action.action.starts_with("smtp-connection-screen:")) {
    const auto screen = number(std::string_view(action.action).substr(23));
    if (screen && *screen < 2) smtpConnectionPage_ = *screen;
    rebuild();
    return;
  }
  if (action.action.starts_with("mail-draft-screen:")) {
    const auto screen = number(std::string_view(action.action).substr(18));
    if (screen && *screen < 3) mailDraftPage_ = *screen;
    rebuild();
    return;
  }
  if (action.action.starts_with("irc-screen:")) {
    const auto screen = number(std::string_view(action.action).substr(11));
    if (screen && *screen < 8) ircPage_ = *screen;
    clearCredentialFields();
    rebuild();
    return;
  }
  if (action.action.starts_with("join-screen:")) {
    const auto screen = number(std::string_view(action.action).substr(12));
    if (screen && *screen < 3) joinPage_ = *screen;
    rebuild();
    return;
  }
  if (action.action == "join-kind" || action.action == "join-cardinality" ||
      action.action == "join-nulls") {
    if (action.action == "join-kind")
      joinKind_ = joinKind_ == "inner" ? "left" : "inner";
    else if (action.action == "join-cardinality")
      joinCardinality_ = joinCardinality_ == "many" ? "one" : "many";
    else
      joinNulls_ = joinNulls_ == "never" ? "equal" : "never";
    rebuild();
    return;
  }
  if (action.action.starts_with("work-mode:")) {
    effectMode_ = action.action.substr(10);
    rebuild();
    return;
  }
  if (action.action.starts_with("work-screen:")) {
    const auto screen = number(std::string_view(action.action).substr(12));
    if (screen && *screen < 3) workPage_ = *screen;
    rebuild();
    return;
  }
  if (action.action.starts_with("window-screen:")) {
    const auto screen = number(std::string_view(action.action).substr(14));
    if (screen && *screen < 3) {
      windowPage_ = *screen;
    }
    rebuild();
    return;
  }
  if (action.action == "window-basis") {
    windowTimeBasis_ = windowTimeBasis_ == "event" ? "observation" : "event";
    rebuild();
    return;
  }
  if (action.action == "window-policy") {
    windowLatePolicy_ = windowLatePolicy_ == "exclude"   ? "include"
                        : windowLatePolicy_ == "include" ? "reject"
                                                         : "exclude";
    rebuild();
    return;
  }
  if (action.action == "close-panel") {
    setVisible(false);
    return;
  }
  if (action.action == "toggle-metadata") {
    showMetadata_ = !showMetadata_;
    rebuild();
    return;
  }
  if (action.action.starts_with("field:")) {
    const auto name               = std::string_view(action.action).substr(6);
    const auto saveMultilineState = [&](MultilineInputState &state) {
      state.caret              = action.caret.value_or(action.value.size());
      state.horizontalScrollPx = action.horizontalScrollPx.value_or(0.0F);
      state.verticalScrollPx   = action.verticalScrollPx.value_or(0.0F);
      state.preferredColumn    = action.preferredColumn;
      state.followCaret        = action.followCaret.value_or(true);
    };
    if (name == "case-path")
      casePath_ = action.value;
    else if (name == "case-title")
      caseTitle_ = action.value;
    else if (name == "communication-name")
      communicationName_ = action.value;
    else if (name == "communication-origin")
      communicationOrigin_ = action.value;
    else if (name == "communication-account")
      communicationAccount_ = action.value;
    else if (name == "communication-actor")
      communicationActor_ = action.value;
    else if (name == "mail-name")
      mailName_ = action.value;
    else if (name == "mail-url")
      mailUrl_ = action.value;
    else if (name == "mail-account")
      mailAccount_ = action.value;
    else if (name == "mail-mailbox")
      mailMailbox_ = action.value;
    else if (name == "mail-ca-bundle")
      mailCaBundle_ = action.value;
    else if (name == "mail-username") {
      std::fill(mailUsername_.begin(), mailUsername_.end(), '\0');
      mailUsername_ = action.value;
    } else if (name == "mail-password") {
      std::fill(mailPassword_.begin(), mailPassword_.end(), '\0');
      mailPassword_ = action.value;
    } else if (name == "mail-lease-ms")
      mailLeaseMilliseconds_ = action.value;
    else if (name == "smtp-name")
      smtpName_ = action.value;
    else if (name == "smtp-url")
      smtpUrl_ = action.value;
    else if (name == "smtp-account")
      smtpAccount_ = action.value;
    else if (name == "smtp-timeout")
      smtpTimeoutMs_ = action.value;
    else if (name == "smtp-ca-bundle")
      smtpCaBundle_ = action.value;
    else if (name == "smtp-username") {
      std::fill(smtpUsername_.begin(), smtpUsername_.end(), '\0');
      smtpUsername_ = action.value;
    } else if (name == "smtp-password") {
      std::fill(smtpPassword_.begin(), smtpPassword_.end(), '\0');
      smtpPassword_ = action.value;
    } else if (name == "smtp-lease-ms")
      smtpLeaseMilliseconds_ = action.value;
    else if (name == "irc-name")
      ircName_ = action.value;
    else if (name == "irc-host")
      ircHost_ = action.value;
    else if (name == "irc-port")
      ircPort_ = action.value;
    else if (name == "irc-account")
      ircAccount_ = action.value;
    else if (name == "irc-network-scope")
      ircNetworkScope_ = action.value;
    else if (name == "irc-nick")
      ircNick_ = action.value;
    else if (name == "irc-username")
      ircUsername_ = action.value;
    else if (name == "irc-real-name")
      ircRealName_ = action.value;
    else if (name == "irc-ca-bundle")
      ircCaBundle_ = action.value;
    else if (name == "irc-timeout")
      ircTimeoutMs_ = action.value;
    else if (name == "irc-line-budget")
      ircLineBudget_ = action.value;
    else if (name == "irc-tag-budget")
      ircTagBudget_ = action.value;
    else if (name == "irc-batch-depth")
      ircBatchDepth_ = action.value;
    else if (name == "irc-event-budget")
      ircEventBudget_ = action.value;
    else if (name == "irc-target-budget")
      ircReadTargetBudget_ = action.value;
    else if (name == "irc-pending-read-budget")
      ircPendingReadBudget_ = action.value;
    else if (name == "irc-pending-record-budget")
      ircPendingRecordBudget_ = action.value;
    else if (name == "irc-pending-byte-budget")
      ircPendingByteBudget_ = action.value;
    else if (name == "irc-sasl-username") {
      std::fill(ircSaslUsername_.begin(), ircSaslUsername_.end(), '\0');
      ircSaslUsername_ = action.value;
    } else if (name == "irc-sasl-password") {
      std::fill(ircSaslPassword_.begin(), ircSaslPassword_.end(), '\0');
      ircSaslPassword_ = action.value;
    } else if (name == "irc-credential-lifetime")
      ircCredentialLifetime_ = action.value;
    else if (name == "irc-target")
      ircTarget_ = action.value;
    else if (name == "irc-after-message-id")
      ircAfterMessageId_ = action.value;
    else if (name == "irc-message-limit")
      ircMessageLimit_ = action.value;
    else if (name == "irc-history-limit")
      ircHistoryLimit_ = action.value;
    else if (name == "irc-draft-connection")
      ircDraftConnection_ = action.value;
    else if (name == "irc-draft-target")
      ircDraftTarget_ = action.value;
    else if (name == "irc-draft-payload") {
      ircDraftPayload_ = action.value;
      saveMultilineState(ircDraftPayloadState_);
    } else if (name == "draft-from")
      mailDraftFrom_ = action.value;
    else if (name == "draft-subject")
      mailDraftSubject_ = action.value;
    else if (name == "draft-to") {
      mailDraftTo_ = action.value;
      saveMultilineState(mailDraftToState_);
    } else if (name == "draft-cc") {
      mailDraftCc_ = action.value;
      saveMultilineState(mailDraftCcState_);
    } else if (name == "draft-bcc") {
      mailDraftBcc_ = action.value;
      saveMultilineState(mailDraftBccState_);
    } else if (name == "draft-text") {
      mailDraftText_ = action.value;
      saveMultilineState(mailDraftTextState_);
    } else if (name == "draft-html") {
      mailDraftHtml_ = action.value;
      saveMultilineState(mailDraftHtmlState_);
    } else if (name == "join-left-observation")
      joinLeftObservation_ = action.value;
    else if (name == "join-left-key")
      joinLeftKey_ = action.value;
    else if (name == "join-right-observation")
      joinRightObservation_ = action.value;
    else if (name == "join-right-key")
      joinRightKey_ = action.value;
    else if (name == "join-pairs")
      joinPairs_ = action.value;
    else if (name == "join-work")
      joinWork_ = action.value;
    else if (name == "join-rows")
      joinRows_ = action.value;
    else if (name == "join-bytes")
      joinBytes_ = action.value;
    else if (name == "join-scan")
      joinScan_ = action.value;
    else if (name == "join-scan-bytes")
      joinScanBytes_ = action.value;
    else if (name == "join-retained")
      joinRetained_ = action.value;
    else if (name == "join-metadata")
      joinMetadata_ = action.value;
    else if (name == "window-observation")
      windowObservation_ = action.value;
    else if (name == "window-time-column")
      windowTimeColumn_ = action.value;
    else if (name == "window-arrival-column")
      windowArrivalColumn_ = action.value;
    else if (name == "window-key-column")
      windowKeyColumn_ = action.value;
    else if (name == "window-start-ms")
      windowStartMs_ = action.value;
    else if (name == "window-end-ms")
      windowEndMs_ = action.value;
    else if (name == "window-watermark-ms")
      windowWatermarkMs_ = action.value;
    else if (name == "window-output")
      windowOutput_ = action.value;
    else if (name == "window-retained")
      windowRetained_ = action.value;
    else if (name == "window-work")
      windowWork_ = action.value;
    else if (name == "window-scan")
      windowScan_ = action.value;
    else if (name == "window-scan-bytes")
      windowScanBytes_ = action.value;
    else if (name == "source-name")
      sourceName_ = action.value;
    else if (name == "locator")
      locator_ = action.value;
    else if (name == "query")
      query_ = action.value;
    else if (name == "key-column")
      keyColumn_ = action.value;
    else if (name == "note")
      noteText_ = action.value;
    else if (name == "evidence-filter") {
      evidenceFilter_      = action.value;
      evidenceFilterCaret_ = action.caret.value_or(action.value.size());
      rebuild();
      return;
    } else if (name == "quote-text") {
      const bool refresh = locatedTarget_ || spanTarget_ ||
                           !spanPreviewDetails_.empty() ||
                           !spanOffset_.empty() || !spanLength_.empty();
      quoteText_      = action.value;
      quoteTextCaret_ = action.caret.value_or(action.value.size());
      clearSpanPreview(true);
      if (refresh) rebuild();
    } else if (name == "span-offset") {
      const bool refresh =
          locatedTarget_ || spanTarget_ || !spanPreviewDetails_.empty();
      spanOffset_      = action.value;
      spanOffsetCaret_ = action.caret.value_or(action.value.size());
      clearSpanPreview(false);
      if (refresh) rebuild();
    } else if (name == "span-length") {
      const bool refresh =
          locatedTarget_ || spanTarget_ || !spanPreviewDetails_.empty();
      spanLength_      = action.value;
      spanLengthCaret_ = action.caret.value_or(action.value.size());
      clearSpanPreview(false);
      if (refresh) rebuild();
    } else if (name == "before")
      compareBefore_ = action.value;
    else if (name == "after")
      compareAfter_ = action.value;
    else if (name == "export-path")
      exportPath_ = action.value;
    else if (name == "export-id")
      exportId_ = action.value;
    else if (name == "refresh-name")
      refreshName_ = action.value;
    else if (name == "effect-target")
      effectTarget_ = action.value;
    else if (name == "effect-delay")
      effectDelay_ = action.value;
    else if (name == "effect-unknown")
      effectUnknown_ = action.value;
    else if (name == "effect-executable")
      effectExecutable_ = action.value;
    else if (name == "effect-cwd")
      effectCwd_ = action.value;
    else if (name == "effect-args")
      effectArgs_ = action.value;
    else if (name == "effect-sql")
      effectSql_ = action.value;
    else if (name == "effect-guard")
      effectGuard_ = action.value;
    else if (name == "effect-expected")
      effectExpected_ = action.value;
    else if (name == "effect-params")
      effectParams_ = action.value;
    else if (name == "effect-guard-params")
      effectGuardParams_ = action.value;
    else if (name == "history-source")
      historySource_ = action.value;
    else if (name == "history-text")
      historyText_ = action.value;
    else if (name == "effect-intent-id")
      effectIntentId_ = action.value;
    else if (name == "effect-grant-id")
      effectGrantId_ = action.value;
    else if (name == "effect-grant-lifetime")
      effectGrantLifetime_ = action.value;
    else if (name == "effect-duplicate-ack")
      effectDuplicateAck_ = action.value;
    return;
  }
  if (action.action.starts_with("record:")) {
    const auto id = number(std::string_view(action.action).substr(7));
    if (id) {
      controller_.select(*id);
      clearSpanPreview(true);
      exportId_        = std::to_string(*id);
      const auto state = controller_.snapshot();
      const auto found = std::ranges::find(state.records, *id, &Record::id);
      if (found != state.records.end() && found->kind == "effect-intent" &&
          found->attributes.contains("operation") &&
          found->attributes.at("operation") == "mail-submit")
        effectIntentId_ = std::to_string(*id);
      if (found != state.records.end() && found->kind == "connection") {
        try {
          refreshName_ = controller_.evidenceCase().source(*id).name;
        } catch (const std::exception &) {
          refreshName_.clear();
        }
      }
      feedback_ = state.status;
      rebuild();
    }
    return;
  }

  if (action.action == "new-case") {
    clearSpanPreview(true);
    run("new " + quote(casePath_) + " " + quote(caseTitle_));
  } else if (action.action == "open-case") {
    clearSpanPreview(true);
    run("open " + quote(casePath_));
  } else if (action.action == "save-case")
    run("save");
  else if (action.action == "close-case") {
    clearCredentialFields();
    clearSpanPreview(true);
    run("close");
  } else if (action.action == "compute-join") {
    run("join " + quote(joinLeftObservation_) + " " + quote(joinLeftKey_) +
        " " + quote(joinRightObservation_) + " " + quote(joinRightKey_) + " " +
        joinKind_ + " " + joinCardinality_ + " " + joinNulls_ + " " +
        quote("pairs=" + joinPairs_) + " " + quote("work=" + joinWork_) + " " +
        quote("rows=" + joinRows_) + " " + quote("bytes=" + joinBytes_) + " " +
        quote("scan=" + joinScan_) + " " +
        quote("scan-bytes=" + joinScanBytes_) + " " +
        quote("retain=" + joinRetained_) + " " +
        quote("metadata=" + joinMetadata_));
  } else if (action.action == "add-imap") {
    run("imap " + quote(mailName_) + " " + quote(mailUrl_) + " " +
        quote(mailAccount_) + " " + quote(mailMailbox_) + " " +
        quote(mailCaBundle_));
  } else if (action.action == "bind-imap") {
    const auto lifetime = number(mailLeaseMilliseconds_);
    if (!lifetime || *lifetime > 900000 || *lifetime == 0) {
      feedback_ = "Read lease lifetime must be 1–900000 milliseconds";
    } else {
      controller_.bindMailReadCredential(mailName_, std::move(mailUsername_),
                                         std::move(mailPassword_),
                                         std::chrono::milliseconds(*lifetime));
      feedback_ = controller_.snapshot().status;
    }
    clearCredentialFields();
    rebuild();
  } else if (action.action == "add-smtp") {
    run("smtp " + quote(smtpName_) + " " + quote(smtpUrl_) + " " +
        quote(smtpAccount_) + " " + quote(smtpTimeoutMs_) + " " +
        quote(smtpCaBundle_));
  } else if (action.action == "create-mail-draft") {
    const auto command =
        "mail-draft " + quote(smtpName_) + " " + quote(mailDraftFrom_) + " " +
        quote(mailDraftSubject_) + " " + quote(mailDraftTo_) + " " +
        quote(mailDraftCc_) + " " + quote(mailDraftBcc_) + " " +
        quote(mailDraftText_) +
        (mailDraftHtml_.empty() ? std::string{} : " " + quote(mailDraftHtml_));
    if (run(command)) {
      if (const auto intent = controller_.snapshot().selected)
        effectIntentId_ = std::to_string(*intent);
    }
  } else if (action.action == "bind-smtp-submit") {
    const auto intent   = number(effectIntentId_);
    const auto lifetime = number(smtpLeaseMilliseconds_);
    if (!intent || !lifetime || *lifetime == 0 || *lifetime > 900000) {
      feedback_ =
          "Enter the exact intent ID and a 1–900000 ms submit lease lifetime";
    } else {
      static_cast<void>(controller_.bindMailSubmitCredential(
          *intent, std::move(smtpUsername_), std::move(smtpPassword_),
          std::chrono::milliseconds(*lifetime)));
      feedback_ = controller_.snapshot().status;
    }
    clearCredentialFields();
    rebuild();
  } else if (action.action == "revoke-smtp-submit") {
    const auto intent = number(effectIntentId_);
    if (intent) {
      static_cast<void>(controller_.revokeMailSubmitCredential(*intent));
      feedback_ = controller_.snapshot().status;
    } else {
      feedback_ = "Enter the exact SMTP intent ID to revoke its lease";
    }
    rebuild();
  } else if (action.action == "review-mail-intent") {
    page_     = 5;
    workPage_ = 1;
    rebuild();
  } else if (action.action == "read-imap") {
    run("mail-read " + quote(mailName_));
  } else if (action.action == "revoke-imap") {
    clearCredentialFields();
    run("mail-revoke " + quote(mailName_));
  } else if (action.action == "read-public-fediverse") {
    run("fediverse-read " + quote(communicationName_) + " " +
        quote(communicationOrigin_) + " " + quote(communicationAccount_) + " " +
        quote(communicationActor_));
  } else if (action.action == "draft-irc") {
    if (run("irc-draft " + quote(ircDraftConnection_) + " " +
            quote(ircDraftTarget_) + " " + quote(ircDraftPayload_))) {
      const auto selected = controller_.snapshot().selected;
      if (selected) effectIntentId_ = std::to_string(*selected);
      page_     = 5;
      workPage_ = 1;
      rebuild();
    }
  } else if (action.action == "add-irc") {
    run("irc " + quote(ircName_) + " " + quote(ircHost_) + " " +
        quote(ircPort_) + " " + quote(ircAccount_) + " " +
        quote(ircNetworkScope_) + " " + quote(ircNick_) + " " +
        quote(ircUsername_) + " " + quote(ircRealName_) + " " +
        quote(ircCaBundle_) + " " + quote(ircTimeoutMs_) + " " +
        quote(ircLineBudget_) + " " + quote(ircTagBudget_) + " " +
        quote(ircBatchDepth_) + " " + quote(ircEventBudget_) + " " +
        quote(ircReadTargetBudget_) + " " + quote(ircPendingReadBudget_) + " " +
        quote(ircPendingRecordBudget_) + " " + quote(ircPendingByteBudget_));
  } else if (action.action == "bind-irc-sasl") {
    const auto lifetime = number(ircCredentialLifetime_);
    if (!lifetime || *lifetime == 0 || *lifetime > 900000) {
      feedback_ = "SASL lease lifetime must be 1–900000 milliseconds";
    } else {
      static_cast<void>(controller_.bindIrcSaslCredential(
          ircName_, std::move(ircSaslUsername_), std::move(ircSaslPassword_),
          std::chrono::milliseconds(*lifetime)));
      feedback_ = controller_.snapshot().status;
    }
    clearCredentialFields();
    rebuild();
  } else if (action.action == "revoke-irc-sasl") {
    clearCredentialFields();
    if (!controller_.revokeIrcSaslCredential(ircName_))
      feedback_ = controller_.snapshot().status;
    else
      feedback_ = controller_.snapshot().status;
    rebuild();
  } else if (action.action == "irc-connect") {
    run("irc-connect " + quote(ircName_));
  } else if (action.action == "irc-join") {
    run("irc-join " + quote(ircName_) + " " + quote(ircTarget_));
  } else if (action.action == "irc-live") {
    run("irc-live " + quote(ircName_) + " " + quote(ircTarget_) + " " +
        quote(ircMessageLimit_));
  } else if (action.action == "irc-history") {
    run("irc-history " + quote(ircName_) + " " + quote(ircTarget_) + " " +
        quote(ircAfterMessageId_) + " " + quote(ircHistoryLimit_));
  } else if (action.action == "irc-cancel") {
    run("irc-cancel");
  } else if (action.action == "irc-close") {
    clearCredentialFields();
    run("irc-close " + quote(ircName_));
  } else if (action.action == "add-sqlite") {
    auto command = "sqlite " + quote(sourceName_) + " " + quote(locator_) +
                   " " + quote(query_);
    if (!keyColumn_.empty()) command += " " + quote(keyColumn_);
    run(std::move(command));
  } else if (action.action == "add-rest")
    run("rest " + quote(sourceName_) + " " + quote(locator_));
  else if (action.action == "refresh")
    run("refresh " + quote(refreshName_));
  else if (action.action == "poll") {
    const auto previous = controller_.snapshot().selected;
    (void)controller_.poll();
    const auto state = controller_.snapshot();
    if (state.selected != previous) clearSpanPreview(true);
    feedback_ = state.status;
    rebuild();
  } else if (action.action == "cancel-capture") {
    run("cancel");
  } else if (action.action == "locate-span") {
    clearSpanPreview(true);
    const auto selected = controller_.snapshot().selected;
    if (!selected || quoteText_.empty()) {
      feedback_ = "Select an evidence record and enter exact text to quote.";
      rebuild();
    } else {
      const bool located =
          run("locate " + std::to_string(*selected) + " " + quote(quoteText_));
      const auto state    = controller_.snapshot();
      spanPreviewDetails_ = state.details;
      if (located && state.span && state.spanTarget == selected) {
        locatedTarget_   = *selected;
        locatedRange_    = state.span;
        spanOffset_      = std::to_string(state.span->offset);
        spanLength_      = std::to_string(state.span->length);
        spanOffsetCaret_ = spanOffset_.size();
        spanLengthCaret_ = spanLength_.size();
        feedback_ = "Exact text located. Preview and verify this range before "
                    "annotating.";
      } else if (located) {
        feedback_ =
            "The text was missing or ambiguous; inspect the listed locations.";
      }
      rebuild();
    }
  } else if (action.action == "preview-span") {
    const auto selected = controller_.snapshot().selected;
    const auto offset   = number(spanOffset_);
    const auto length   = number(spanLength_);
    clearSpanPreview(false);
    if (!selected || !offset || !length || *length == 0) {
      feedback_ = "Select a record and enter a nonzero byte range to preview.";
      rebuild();
    } else if (run("preview " + std::to_string(*selected) + " " +
                   std::to_string(*offset) + " " + std::to_string(*length))) {
      const auto state = controller_.snapshot();
      if (state.spanTarget == selected && state.span &&
          state.span->offset == *offset && state.span->length == *length) {
        spanTarget_ = *selected;
        spanRange_  = state.span;
        try {
          const auto raw   = controller_.evidenceCase().rawText(*selected);
          const auto start = static_cast<std::size_t>(state.span->offset);
          const auto count = static_cast<std::size_t>(state.span->length);
          spanPreviewDetails_.push_back(
              "Selected bytes: " +
              escaped(std::string_view(raw).substr(start, count)));
          spanPreviewDetails_.push_back(
              "Verified target: record #" + std::to_string(*selected) +
              " · UTF-8 bytes [" + std::to_string(*offset) + ", " +
              std::to_string(*offset + *length) + ")");
          spanPreviewDetails_.push_back(spanContext(raw, *state.span));
        } catch (const std::exception &error) {
          spanPreviewDetails_ = {
              "Selected bytes: context could not be read.",
              "Verified target: record #" + std::to_string(*selected) +
                  " · UTF-8 bytes [" + std::to_string(*offset) + ", " +
                  std::to_string(*offset + *length) + ")",
              std::string{"Context unavailable: "} + error.what()};
        }
        spanPreviewDetails_.insert(spanPreviewDetails_.end(),
                                   state.details.begin(), state.details.end());
        feedback_ =
            "Review the verified context, then annotate this exact range.";
      } else {
        feedback_ =
            "Preview did not verify the requested record and byte range.";
      }
      rebuild();
    }
  } else if (action.action == "note-field") {
    const auto id = controller_.snapshot().selected;
    if (id)
      run("note " + std::to_string(*id) + " " + quote(noteText_));
    else
      feedback_ = "Select an evidence field first.";
  } else if (action.action == "note-span") {
    const auto selected = controller_.snapshot().selected;
    const auto offset   = number(spanOffset_);
    const auto length   = number(spanLength_);
    if (spanTarget_ && spanRange_ && selected == spanTarget_ && offset &&
        length && spanRange_->offset == *offset &&
        spanRange_->length == *length) {
      if (run("span " + std::to_string(*spanTarget_) + " " +
              std::to_string(spanRange_->offset) + " " +
              std::to_string(spanRange_->length) + " " + quote(noteText_))) {
        clearSpanPreview(true);
        rebuild();
      }
    } else {
      feedback_ = "Preview this exact record and byte range before annotating.";
      rebuild();
    }
  } else if (action.action == "compare") {
    const auto before = number(compareBefore_);
    const auto after  = number(compareAfter_);
    if (before && after)
      run("compare " + std::to_string(*before) + " " + std::to_string(*after));
    else
      feedback_ = "Enter observation record IDs to compare.";
  } else if (action.action == "run-window") {
    const auto timeColumn = windowTimeBasis_ == "observation"
                                ? std::string{"-"}
                                : windowTimeColumn_;
    auto command =
        "window " + quote(windowObservation_) + " " + windowTimeBasis_ + " " +
        quote(timeColumn) + " " + quote(windowStartMs_) + " " +
        quote(windowEndMs_) + " " + quote(windowWatermarkMs_) + " " +
        windowLatePolicy_ + " " + quote("output=" + windowOutput_) + " " +
        quote("retain=" + windowRetained_) + " " +
        quote("work=" + windowWork_) + " " + quote("scan=" + windowScan_) +
        " " + quote("scan-bytes=" + windowScanBytes_);
    if (!windowArrivalColumn_.empty())
      command += " " + quote("arrival=" + windowArrivalColumn_);
    if (!windowKeyColumn_.empty())
      command += " " + quote("key=" + windowKeyColumn_);
    run(std::move(command));
  } else if (action.action == "cancel-window") {
    run("cancel-window");
  } else if (action.action == "export") {
    const auto id = number(exportId_);
    if (id)
      run("export " + std::to_string(*id) + " " + quote(exportPath_));
    else
      feedback_ = "Enter a record ID for the export closure.";
  } else if (action.action == "draft-work") {
    try {
      std::string command;
      if (effectMode_ == "fake") {
        if (!effectUnknown_.empty() && effectUnknown_ != "yes")
          throw std::invalid_argument(
              "Type yes only when simulating an unknown outcome.");
        command = "draft-fake " + quote(effectTarget_) + " " +
                  (effectDelay_.empty() ? "0" : effectDelay_);
        if (effectUnknown_ == "yes") command += " unknown";
      } else if (effectMode_ == "process") {
        command = "draft-process " + quote(effectExecutable_) + " " +
                  quote(effectCwd_);
        for (const auto &argument : quotedArguments(effectArgs_))
          command += " " + quote(argument);
      } else if (effectMode_ == "sql") {
        command = "draft-sql " + quote(effectTarget_) + " " +
                  quote(effectSql_) + " " + quote(effectGuard_) + " " +
                  quote(effectExpected_);
        for (const auto &parameter : quotedArguments(effectParams_))
          command += " " + quote(parameter);
        command += " --guard";
        for (const auto &parameter : quotedArguments(effectGuardParams_))
          command += " " + quote(parameter);
      } else if (effectMode_ == "irc") {
        command = "irc-draft " + quote(ircDraftConnection_) + " " +
                  quote(ircDraftTarget_) + " " + quote(ircDraftPayload_);
      } else if (effectMode_ == "history") {
        command =
            "history-text " + quote(historySource_) + " " + quote(historyText_);
      } else {
        throw std::invalid_argument("Choose a work intent type first.");
      }
      if (run(std::move(command))) {
        const auto selected = controller_.snapshot().selected;
        if (selected) effectIntentId_ = std::to_string(*selected);
        rebuild();
      }
    } catch (const std::exception &error) {
      feedback_ = error.what();
      rebuild();
    }
  } else if (action.action == "review-work") {
    const auto id = number(effectIntentId_);
    if (id) {
      if (run("review " + std::to_string(*id))) {
        const auto selected = controller_.snapshot().selected;
        if (selected) effectIntentId_ = std::to_string(*selected);
        rebuild();
      }
    } else {
      feedback_ = "Enter an intent ID to review.";
      rebuild();
    }
  } else if (action.action == "grant-work") {
    const auto id       = number(effectIntentId_);
    const auto lifetime = number(effectGrantLifetime_);
    if (id && lifetime) {
      if (run("grant " + std::to_string(*id) + " " +
              std::to_string(*lifetime))) {
        const auto selected = controller_.snapshot().selected;
        if (selected) effectGrantId_ = std::to_string(*selected);
        rebuild();
      }
    } else {
      feedback_ = "Enter an intent ID and grant lifetime in milliseconds.";
      rebuild();
    }
  } else if (action.action == "dispatch-work") {
    const auto intent = number(effectIntentId_);
    const auto grant  = number(effectGrantId_);
    if (intent && grant)
      run("dispatch " + std::to_string(*intent) + " " + std::to_string(*grant));
    else {
      feedback_ = "Review an intent and enter its grant ID before dispatch.";
      rebuild();
    }
  } else if (action.action == "retry-work") {
    const auto intent = number(effectIntentId_);
    const auto grant  = number(effectGrantId_);
    if (intent && grant) {
      auto command =
          "retry " + std::to_string(*intent) + " " + std::to_string(*grant);
      if (effectDuplicateAck_ == "DUPLICATE")
        command += " acknowledge-duplication";
      run(std::move(command));
    } else {
      feedback_ = "Enter an intent ID and its grant ID before retry.";
      rebuild();
    }
  } else if (action.action == "revoke-work") {
    const auto grant = number(effectGrantId_);
    if (grant)
      run("revoke " + std::to_string(*grant));
    else {
      feedback_ = "Enter a grant ID to revoke.";
      rebuild();
    }
  } else if (action.action == "cancel-work") {
    run("cancel-work");
  } else if (action.action == "pending-work") {
    static_cast<void>(controller_.poll());
    run("pending");
  }
  if (!feedback_.empty()) rebuild();
}

void EvidenceCasePanel::rebuild() {
  const auto state = controller_.snapshot();
  auto statusText  = state.status;
  if (state.busy)
    statusText +=
        (statusText.empty() ? "" : "\n") + std::string{"Capture in progress."};
  if (!feedback_.empty() && feedback_ != statusText)
    statusText += (statusText.empty() ? "" : "\n") + feedback_;
  ui::Widget root{.id = 1, .model = ui::Modal{"Evidence case"}};
  root.preserveReadableControls = true;
  ui::Widget header{.id = 2, .model = ui::ButtonFlow{}};
  header.children = {
      label(93, state.open ? state.title : "Private evidence case"),
      button(90, "Close panel", "close-panel")};
  root.children.push_back(std::move(header));
  root.children.push_back(descriptionLabel(3, std::move(statusText), 3));
  ui::Widget tabs{.id = 4, .model = ui::ButtonFlow{}};
  tabs.children = {button(5, "Case", "page:0"),
                   button(6, "Sources", "page:1"),
                   button(7, "Evidence", "page:2"),
                   button(8, "Notes", "page:3"),
                   button(9, "Compare and export", "page:4"),
                   button(10, "Work", "page:5"),
                   button(80, "Communications", "page:6")};
  root.children.push_back(std::move(tabs));

  switch (page_) {
  case 0:
    addFields(root,
              {
                  label(18, "Create or reopen a private case"),
                  field(11, "Case directory", casePath_, "field:case-path"),
                  field(12, "Case title", caseTitle_, "field:case-title"),
                  [&] {
                    ui::Widget actions{.id = 17, .model = ui::ButtonFlow{}};
                    actions.children = {button(13, "New case", "new-case"),
                                        button(14, "Open case", "open-case"),
                                        button(15, "Save case", "save-case"),
                                        button(16, "Close case", "close-case")};
                    return actions;
                  }(),
              });
    break;
  case 1:
    addFields(
        root,
        {
            label(20, "Read-only source definitions"),
            descriptionLabel(
                31,
                [&] {
                  const auto selected = state.selected;
                  if (selected) {
                    const auto found = std::ranges::find(
                        state.records, *selected, &Record::id);
                    if (found != state.records.end() &&
                        found->kind == "connection") {
                      try {
                        return limitsDescription(controller_.evidenceCase()
                                                     .source(*selected)
                                                     .limits);
                      } catch (const std::exception &) {
                      }
                    }
                  }
                  return limitsDescription(CaptureLimits{});
                }(),
                4),
            field(21, "Source name", sourceName_, "field:source-name"),
            field(22, "SQLite path or REST URL", locator_, "field:locator"),
            field(23, "SQLite SELECT query", query_, "field:query"),
            field(24, "Stable key column (optional)", keyColumn_,
                  "field:key-column"),
            [&] {
              ui::Widget actions{.id = 32, .model = ui::ButtonFlow{}};
              actions.children = {
                  button(25, "Add SQLite source", "add-sqlite"),
                  button(26, "Add REST GET source", "add-rest")};
              return actions;
            }(),
            field(27, "Refresh source name", refreshName_,
                  "field:refresh-name"),
            [&] {
              ui::Widget actions{.id = 33, .model = ui::ButtonFlow{}};
              actions.children = {
                  button(28, "Refresh", "refresh"),
                  button(29, "Check capture status", "poll"),
                  button(30, "Cancel capture", "cancel-capture")};
              return actions;
            }(),
        });
    break;
  case 2: {
    ui::Widget evidenceHeader{.id = 35, .model = ui::ButtonFlow{}};
    evidenceHeader.children = {
        label(30, "Captured requests, coverage, schema fields and values"),
        button(34, showMetadata_ ? "Hide metadata" : "Show metadata",
               "toggle-metadata")};
    root.children.push_back(std::move(evidenceHeader));
    root.children.push_back(field(36, "Filter by kind, label, ID or parent",
                                  evidenceFilter_, "field:evidence-filter",
                                  evidenceFilterCaret_));
    ui::Widget records{.id = 31, .model = ui::List{}};
    auto &rows         = std::get<ui::List>(records.model).rows;
    ui::WidgetId rowId = 100;
    for (const auto &record : state.records) {
      const bool metadata =
          record.kind == "attribute" || record.kind == "reference";
      if (metadata && !showMetadata_) continue;
      const auto id = std::to_string(record.id);
      auto kindLabel =
          record.kind == "value" ? std::string{"observed value"} : record.kind;
      if (record.kind == "attribute") kindLabel = "metadata attribute";
      if (record.kind == "reference") kindLabel = "metadata reference";
      auto rowLabel = "#" + id + " " + kindLabel + " · " + record.label;
      if (record.parent) {
        const auto parent =
            std::ranges::find(state.records, *record.parent, &Record::id);
        rowLabel += " · parent #" + std::to_string(*record.parent);
        if (parent != state.records.end())
          rowLabel += " " + parent->kind + " · " + parent->label;
      }
      if (!containsFolded(rowLabel, evidenceFilter_) &&
          !containsFolded(record.text, evidenceFilter_))
        continue;
      rows.push_back({rowId++, std::move(rowLabel), "record:" + id, true});
    }
    root.children.push_back(std::move(records));
    root.children.push_back(label(32, "Complete inspection details"));
    ui::Widget details{.id = 33, .model = ui::List{}};
    auto &detailRows      = std::get<ui::List>(details.model).rows;
    ui::WidgetId detailId = rowId + 1U;
    auto detailText       = state.details;
    if (detailText.empty())
      detailText.emplace_back(
          "Select a request, observation, field, row, page or note above.");
    addDetailRows(detailRows, detailId, detailText);
    root.children.push_back(std::move(details));
    break;
  }
  case 3:
    addFields(
        root,
        {
            label(40, "Select an observed value or response body in Evidence, "
                      "then attach a note"),
            field(41, "Note text", noteText_, "field:note"),
            [&] {
              ui::Widget actions{.id = 51, .model = ui::ButtonFlow{}};
              actions.children = {
                  button(42, "Annotate selected field", "note-field"),
                  button(44, "Locate exact text", "locate-span")};
              return actions;
            }(),
            field(43, "Text to quote exactly", quoteText_, "field:quote-text",
                  quoteTextCaret_),
            field(45, "Exact UTF-8 byte offset", spanOffset_,
                  "field:span-offset", spanOffsetCaret_),
            field(46, "Exact UTF-8 byte length", spanLength_,
                  "field:span-length", spanLengthCaret_),
            [&] {
              ui::Widget actions{.id = 52, .model = ui::ButtonFlow{}};
              actions.children = {
                  button(47, "Preview exact span", "preview-span"),
                  button(50, "Annotate exact span", "note-span")};
              return actions;
            }(),
            label(48,
                  [&] {
                    if (spanTarget_ && spanRange_)
                      return "Verified preview · record #" +
                             std::to_string(*spanTarget_) + " · UTF-8 bytes [" +
                             std::to_string(spanRange_->offset) + ", " +
                             std::to_string(spanRange_->offset +
                                            spanRange_->length) +
                             ")";
                    if (locatedTarget_ && locatedRange_)
                      return "Located match · record #" +
                             std::to_string(*locatedTarget_) +
                             " · UTF-8 bytes [" +
                             std::to_string(locatedRange_->offset) + ", " +
                             std::to_string(locatedRange_->offset +
                                            locatedRange_->length) +
                             ") · Preview to verify";
                    return std::string{"No verified byte range. Locate text or "
                                       "enter bytes, then preview."};
                  }()),
            [&] {
              ui::Widget preview{.id = 49, .model = ui::List{}};
              auto &rows         = std::get<ui::List>(preview.model).rows;
              ui::WidgetId rowId = 3000;
              addDetailRows(rows, rowId, spanPreviewDetails_);
              if (rows.empty())
                rows.push_back(
                    {rowId, "Preview context appears here.", {}, true});
              return preview;
            }(),
        });
    break;
  case 4: {
    addFields(
        root,
        {
            label(
                50,
                "Compare immutable observations and select an export closure"),
            field(51, "Earlier observation record ID", compareBefore_,
                  "field:before"),
            field(52, "Later observation record ID", compareAfter_,
                  "field:after"),
            [&] {
              ui::Widget actions{.id = 59, .model = ui::ButtonFlow{}};
              actions.children = {button(53, "Compare observations", "compare"),
                                  button(70, "Join observations", "page:7"),
                                  button(71, "Window observations", "page:8")};
              return actions;
            }(),
        });
    root.children.push_back(label(57, "Comparison results"));
    ui::Widget details{.id = 58, .model = ui::List{}};
    auto &rows         = std::get<ui::List>(details.model).rows;
    ui::WidgetId rowId = 2000;
    auto lines         = state.details;
    if (lines.empty())
      lines.emplace_back("Run a comparison to inspect retained changes.");
    addDetailRows(rows, rowId, lines);
    root.children.push_back(std::move(details));
    addFields(root,
              {field(54, "Export record ID", exportId_, "field:export-id"),
               field(55, "Export directory", exportPath_, "field:export-path"),
               [&] {
                 ui::Widget actions{.id = 60, .model = ui::ButtonFlow{}};
                 actions.children = {
                     button(56, "Prepare selective export", "export")};
                 return actions;
               }()});
    break;
  }
  case 5: {
    ui::Widget screens{.id = 600, .model = ui::ButtonFlow{}};
    screens.children = {button(601, "Draft", "work-screen:0"),
                        button(602, "Review and grant", "work-screen:1"),
                        button(603, "Pending work", "work-screen:2")};
    root.children.push_back(std::move(screens));
    if (workPage_ == 0) {
      ui::Widget modes{.id = 604, .model = ui::ButtonFlow{}};
      modes.children = {button(605, "Fake", "work-mode:fake"),
                        button(606, "Process", "work-mode:process"),
                        button(607, "SQL patch", "work-mode:sql"),
                        button(608, "History import", "work-mode:history"),
                        button(616, "IRC message", "work-mode:irc")};
      root.children.push_back(std::move(modes));
      if (effectMode_ == "fake") {
        root.children.push_back(descriptionLabel(
            609, "Fake effect for exercising review, grants, and outcomes.",
            1));
        addFields(root, {field(610, "Fake work label", effectTarget_,
                               "field:effect-target"),
                         field(611, "Delay in milliseconds", effectDelay_,
                               "field:effect-delay"),
                         field(612, "Unknown outcome (type yes to simulate)",
                               effectUnknown_, "field:effect-unknown")});
      } else if (effectMode_ == "process") {
        root.children.push_back(descriptionLabel(
            609, "Arguments are separate quoted tokens; no shell is used.", 1));
        addFields(root, {field(610, "Canonical executable path",
                               effectExecutable_, "field:effect-executable"),
                         field(611, "Working directory", effectCwd_,
                               "field:effect-cwd"),
                         field(612, "Arguments as quoted tokens", effectArgs_,
                               "field:effect-args")});
      } else if (effectMode_ == "sql") {
        addFields(
            root,
            {field(610, "Canonical SQLite database path", effectTarget_,
                   "field:effect-target"),
             field(611, "SQL patch statement", effectSql_, "field:effect-sql"),
             field(612, "Guard SELECT query", effectGuard_,
                   "field:effect-guard"),
             field(613, "Expected guard value (n:/i:/r:/t:/b:HEX)",
                   effectExpected_, "field:effect-expected"),
             field(614, "Typed patch parameters", effectParams_,
                   "field:effect-params"),
             field(615, "Typed guard parameters", effectGuardParams_,
                   "field:effect-guard-params")});
      } else if (effectMode_ == "irc") {
        root.children.push_back(descriptionLabel(
            609,
            "One IRC protocol line is one message. Embedded CR or LF is "
            "refused, never split into multiple sends. Intent creation does "
            "not send it.",
            2));
        addFields(root,
                  {label(6100, "Saved IRC connection name"),
                   field(610, "Saved IRC connection name", ircDraftConnection_,
                         "field:irc-draft-connection"),
                   label(6110, "Joined channel or configured nick"),
                   field(611, "Joined channel or configured nick",
                         ircDraftTarget_, "field:irc-draft-target"),
                   label(6120, "Message text (one IRC protocol line)"),
                   multilineField(612, "Message text (one IRC protocol line)",
                                  ircDraftPayload_, "field:irc-draft-payload",
                                  ircDraftPayloadState_.caret,
                                  ircDraftPayloadState_.horizontalScrollPx,
                                  ircDraftPayloadState_.verticalScrollPx,
                                  ircDraftPayloadState_.preferredColumn,
                                  ircDraftPayloadState_.followCaret, 160.0F)});
      } else {
        root.children.push_back(
            descriptionLabel(609,
                             "History text is imported as evidence in this "
                             "private case. No command is executed.",
                             2));
        addFields(root, {field(610, "History source label", historySource_,
                               "field:history-source"),
                         field(611, "Raw history text to stage", historyText_,
                               "field:history-text")});
      }
      root.children.push_back(
          button(621,
                 effectMode_ == "history" ? "Import history as evidence"
                 : effectMode_ == "irc"   ? "Create local IRC intent"
                                          : "Draft work intent",
                 "draft-work"));
      root.children.push_back(detailList(
          639, state.details, 4000,
          "Draft summary and retained history occurrences appear here."));
    } else if (workPage_ == 1) {
      root.children.push_back(descriptionLabel(
          609,
          "Review is read-only. Grant creates a temporary host-bound "
          "authority; a reopened case has no restored grant.",
          2));
      ui::Widget reviewActions{.id = 626, .model = ui::ButtonFlow{}};
      reviewActions.children = {
          button(631, "Review intent", "review-work"),
          button(632, "Grant reviewed intent", "grant-work"),
          button(633, "Dispatch with grant", "dispatch-work"),
          button(635, "Revoke grant", "revoke-work")};
      addFields(
          root,
          {label(640, "Intent ID"),
           field(622, "Intent ID", effectIntentId_, "field:effect-intent-id"),
           std::move(reviewActions),
           label(641, "Grant lifetime (milliseconds)"),
           field(624, "Grant lifetime in milliseconds", effectGrantLifetime_,
                 "field:effect-grant-lifetime"),
           label(642, "Grant ID"),
           field(623, "Grant ID", effectGrantId_, "field:effect-grant-id"),
           label(643, "Retry acknowledgement"),
           field(625, "Type DUPLICATE to acknowledge retry risk",
                 effectDuplicateAck_, "field:effect-duplicate-ack"),
           button(634, "Retry unknown outcome", "retry-work")});
      root.children.push_back(descriptionLabel(
          638, "Retry after an unknown result is a separate attempt.", 1));
      root.children.push_back(descriptionLabel(
          637, "Type DUPLICATE to acknowledge possible repeated effects.", 1));
      root.children.push_back(detailList(
          639, state.details, 4000,
          "Review an intent before granting authority or dispatching it."));
    } else {
      root.children.push_back(descriptionLabel(
          609,
          "Newest effects first; scroll for older work and imported history.\n"
          "Use Evidence filter to find retained output.",
          2));
      ui::Widget workActions{.id = 630, .model = ui::ButtonFlow{}};
      workActions.children = {
          button(636, "Cancel active work", "cancel-work"),
          button(637, "Refresh pending work", "pending-work")};
      root.children.push_back(std::move(workActions));
      root.children.push_back(
          detailList(639, state.details, 4000,
                     "Refresh pending work to inspect statuses and outputs."));
    }
    break;
  }
  case 6: {
    ui::Widget screens{.id = 720, .model = ui::ButtonFlow{}};
    screens.children = {
        button(721, "Public statuses", "communication-screen:0"),
        button(722, "Mail connection", "communication-screen:1"),
        button(723, "Mail read credential", "communication-screen:2"),
        button(724, "Mail TLS trust", "communication-screen:3"),
        button(725, "SMTP connection", "communication-screen:4"),
        button(726, "Mail draft", "communication-screen:5"),
        button(727, "Submit credential", "communication-screen:6"),
        button(728, "IRC connection and session", "communication-screen:7")};
    root.children.push_back(std::move(screens));
    if (communicationPage_ == 1) {
      addFields(
          root,
          {label(730, "Read-only IMAP connection"),
           label(731, "Connection revision name"),
           field(732, "Connection revision name", mailName_, "field:mail-name"),
           label(733, "IMAPS server URL"),
           field(734, "IMAPS server URL", mailUrl_, "field:mail-url"),
           label(735, "Account scope label"),
           field(736, "Account scope label", mailAccount_,
                 "field:mail-account"),
           label(737, "Mailbox name"),
           field(738, "Mailbox name", mailMailbox_, "field:mail-mailbox"),
           descriptionLabel(
               739,
               "Save the definition, then bind a session credential. "
               "Reads use EXAMINE and BODY.PEEK.",
               2)});
      ui::Widget actions{.id = 740, .model = ui::ButtonFlow{}};
      actions.children = {button(741, "Save mail connection", "add-imap"),
                          button(742, "Read mailbox", "read-imap"),
                          button(743, "Check read status", "poll"),
                          button(744, "Cancel read", "cancel-capture")};
      root.children.push_back(std::move(actions));
    } else if (communicationPage_ == 2) {
      auto username =
          field(752, "Username", mailUsername_, "field:mail-username");
      auto password =
          field(754, "Password", mailPassword_, "field:mail-password");
      std::get<ui::TextField>(username.model).sensitive = true;
      std::get<ui::TextField>(password.model).sensitive = true;
      addFields(root,
                {label(750, "Session credential for read-only IMAP access"),
                 label(745, "Saved connection revision name"),
                 field(746, "Saved connection revision name", mailName_,
                       "field:mail-name"),
                 label(751, "Username (protected)"), std::move(username),
                 label(753, "Password (protected)"), std::move(password),
                 label(755, "Read lease lifetime in milliseconds (1–900000)"),
                 field(756, "Read lease lifetime in milliseconds",
                       mailLeaseMilliseconds_, "field:mail-lease-ms"),
                 descriptionLabel(
                     757,
                     "Credentials stay in this session and are cleared "
                     "from these fields after binding or leaving this screen.",
                     2)});
      ui::Widget actions{.id = 758, .model = ui::ButtonFlow{}};
      actions.children = {button(759, "Bind read credential", "bind-imap"),
                          button(760, "Read mailbox", "read-imap"),
                          button(761, "Revoke read credential", "revoke-imap")};
      root.children.push_back(std::move(actions));
    } else if (communicationPage_ == 3) {
      addFields(
          root,
          {label(770, "TLS trust for a new mail connection revision"),
           label(771, "CA bundle path (empty uses system trust)"),
           field(772, "CA bundle path (empty uses system trust)", mailCaBundle_,
                 "field:mail-ca-bundle"),
           descriptionLabel(773,
                            "Peer certificate and server hostname verification "
                            "are always required. A custom bundle supplies "
                            "trust for a private mail service.",
                            3),
           descriptionLabel(774,
                            "Set trust before saving a new connection. Saved "
                            "connection revisions keep their configured path.",
                            2),
           button(775, "Back to mail connection", "communication-screen:1")});
    } else if (communicationPage_ == 4) {
      ui::Widget screens{.id = 779, .model = ui::ButtonFlow{}};
      screens.children = {button(778, "Connection", "smtp-connection-screen:0"),
                          button(789, "TLS trust", "smtp-connection-screen:1")};
      root.children.push_back(std::move(screens));
      if (smtpConnectionPage_ == 0) {
        addFields(
            root,
            {label(780, "SMTP submission connection revision"),
             label(7810, "Connection revision name"),
             field(781, "Connection revision name", smtpName_,
                   "field:smtp-name"),
             label(7820, "SMTP URL (smtps:// or STARTTLS smtp://)"),
             field(782, "SMTP URL (smtps:// or STARTTLS smtp://)", smtpUrl_,
                   "field:smtp-url"),
             label(7830, "Account scope label"),
             field(783, "Account scope label", smtpAccount_,
                   "field:smtp-account"),
             label(7840, "Timeout milliseconds (1–5000)"),
             field(784, "Timeout milliseconds (1–5000)", smtpTimeoutMs_,
                   "field:smtp-timeout"),
             descriptionLabel(
                 786,
                 "The server certificate and hostname are always verified. "
                 "STARTTLS must succeed before credentials are sent. The "
                 "connection record contains no credentials.",
                 3),
             button(787, "Save SMTP connection", "add-smtp")});
      } else {
        addFields(
            root,
            {label(788, "TLS trust for this SMTP connection revision"),
             label(7850, "Custom CA bundle path (optional)"),
             field(785, "Custom CA bundle path (optional)", smtpCaBundle_,
                   "field:smtp-ca-bundle"),
             descriptionLabel(
                 786,
                 "Peer certificate and server hostname verification are "
                 "always required. STARTTLS must succeed before credentials "
                 "are sent. Leave empty to use system trust.",
                 3),
             button(787, "Save SMTP connection", "add-smtp")});
      }
    } else if (communicationPage_ == 5) {
      ui::Widget screens{.id = 7900, .model = ui::ButtonFlow{}};
      screens.children = {button(7901, "Envelope", "mail-draft-screen:0"),
                          button(7902, "Content", "mail-draft-screen:1"),
                          button(7903, "Review", "mail-draft-screen:2")};
      root.children.push_back(std::move(screens));
      if (mailDraftPage_ == 0) {
        addFields(
            root,
            {label(790, "Compose the mail envelope for a local draft"),
             label(7910, "Saved SMTP connection name"),
             field(791, "Saved SMTP connection name", smtpName_,
                   "field:smtp-name"),
             label(7920, "From mailbox"),
             field(792, "From mailbox", mailDraftFrom_, "field:draft-from"),
             label(7930, "Subject"),
             field(793, "Subject", mailDraftSubject_, "field:draft-subject"),
             label(7940, "To mailboxes (one per line; duplicates preserved)"),
             multilineField(
                 794, "To mailboxes (one per line; duplicates preserved)",
                 mailDraftTo_, "field:draft-to", mailDraftToState_.caret,
                 mailDraftToState_.horizontalScrollPx,
                 mailDraftToState_.verticalScrollPx,
                 mailDraftToState_.preferredColumn,
                 mailDraftToState_.followCaret, 72.0F),
             label(7950, "Cc mailboxes (one per line; duplicates preserved)"),
             multilineField(
                 795, "Cc mailboxes (one per line; duplicates preserved)",
                 mailDraftCc_, "field:draft-cc", mailDraftCcState_.caret,
                 mailDraftCcState_.horizontalScrollPx,
                 mailDraftCcState_.verticalScrollPx,
                 mailDraftCcState_.preferredColumn,
                 mailDraftCcState_.followCaret, 72.0F),
             label(7960, "Bcc mailboxes (one per line; hidden from headers)"),
             multilineField(
                 796, "Bcc mailboxes (one per line; hidden from headers)",
                 mailDraftBcc_, "field:draft-bcc", mailDraftBccState_.caret,
                 mailDraftBccState_.horizontalScrollPx,
                 mailDraftBccState_.verticalScrollPx,
                 mailDraftBccState_.preferredColumn,
                 mailDraftBccState_.followCaret, 72.0F),
             descriptionLabel(
                 799,
                 "Each recipient occurrence stays separate. Bcc addresses "
                 "are SMTP envelope recipients and never message headers.",
                 2),
             button(800, "Next: message content", "mail-draft-screen:1")});
      } else if (mailDraftPage_ == 1) {
        auto plainBody = multilineField(
            797, "Plain text body", mailDraftText_, "field:draft-text",
            mailDraftTextState_.caret, mailDraftTextState_.horizontalScrollPx,
            mailDraftTextState_.verticalScrollPx,
            mailDraftTextState_.preferredColumn,
            mailDraftTextState_.followCaret, 168.0F);
        auto htmlBody = multilineField(
            798, "HTML body (optional)", mailDraftHtml_, "field:draft-html",
            mailDraftHtmlState_.caret, mailDraftHtmlState_.horizontalScrollPx,
            mailDraftHtmlState_.verticalScrollPx,
            mailDraftHtmlState_.preferredColumn,
            mailDraftHtmlState_.followCaret, 128.0F);
        addFields(root,
                  {label(790, "Compose message content"),
                   label(7970, "Plain text body"), std::move(plainBody),
                   label(7980, "HTML body (optional)"), std::move(htmlBody),
                   descriptionLabel(
                       799,
                       "The body is retained in the local draft. Continue to "
                       "Review to inspect the exact recipients and content; "
                       "no network action happens here.",
                       2),
                   button(800, "Back: envelope", "mail-draft-screen:0"),
                   button(801, "Next: review draft", "mail-draft-screen:2")});
      } else {
        std::vector<std::string> review{"SMTP connection: " + smtpName_,
                                        "From: " + mailDraftFrom_,
                                        "Subject: " + mailDraftSubject_};
        const auto appendRecipients = [&](const std::string_view role,
                                          const std::string_view text) {
          std::size_t begin{};
          std::size_t occurrence{};
          while (begin < text.size()) {
            const auto end     = text.find('\n', begin);
            const auto address = text.substr(
                begin, end == std::string_view::npos ? std::string_view::npos
                                                     : end - begin);
            if (!address.empty())
              review.push_back(std::string(role) + " #" +
                               std::to_string(++occurrence) + ": " +
                               std::string(address));
            if (end == std::string_view::npos) break;
            begin = end + 1;
          }
          if (!occurrence) review.push_back(std::string(role) + ": (none)");
        };
        appendRecipients("To", mailDraftTo_);
        appendRecipients("Cc", mailDraftCc_);
        appendRecipients("Bcc", mailDraftBcc_);
        review.push_back("Plain text body:");
        review.push_back(mailDraftText_);
        if (!mailDraftHtml_.empty()) {
          review.push_back("HTML body:");
          review.push_back(mailDraftHtml_);
        }
        auto reviewDetails =
            detailList(802, review, 4100,
                       "Enter envelope and content fields to review them.");
        reviewDetails.maxLines         = 12;
        reviewDetails.preferred.height = 320.0F;
        addFields(
            root,
            {label(790, "Review the exact local draft before creating intent"),
             descriptionLabel(
                 799,
                 "Scroll the review list to inspect every recipient "
                 "occurrence and message body. Creating an intent does not "
                 "send it.",
                 2),
             std::move(reviewDetails),
             button(803, "Back: message content", "mail-draft-screen:1"),
             button(800, "Create exact draft intent", "create-mail-draft")});
      }
    } else if (communicationPage_ == 6) {
      auto username =
          field(812, "Username", smtpUsername_, "field:smtp-username");
      auto password =
          field(814, "Password", smtpPassword_, "field:smtp-password");
      std::get<ui::TextField>(username.model).sensitive = true;
      std::get<ui::TextField>(password.model).sensitive = true;
      addFields(
          root,
          {label(810, "Session-only credential for one exact SMTP intent"),
           label(8110, "Mail submit intent ID"),
           field(811, "Mail submit intent ID", effectIntentId_,
                 "field:effect-intent-id"),
           label(8120, "Username (protected)"), std::move(username),
           label(8140, "Password (protected)"), std::move(password),
           label(8150, "Submit lease lifetime in milliseconds (1–900000)"),
           field(815, "Submit lease lifetime in milliseconds (1–900000)",
                 smtpLeaseMilliseconds_, "field:smtp-lease-ms"),
           descriptionLabel(
               816,
               "Credentials are scoped to this intent's SMTP server and "
               "account. Binding a credential does not grant dispatch. "
               "Closing the case revokes the lease.",
               3)});
      ui::Widget actions{.id = 817, .model = ui::ButtonFlow{}};
      actions.children = {
          button(818, "Bind submit credential", "bind-smtp-submit"),
          button(819, "Revoke this intent lease", "revoke-smtp-submit"),
          button(820, "Review and grant intent", "review-mail-intent")};
      root.children.push_back(std::move(actions));
    } else if (communicationPage_ == 7) {
      ui::Widget ircScreens{.id = 8200, .model = ui::ButtonFlow{}};
      ircScreens.children = {button(8201, "Service", "irc-screen:0"),
                             button(8202, "Identity", "irc-screen:1"),
                             button(8203, "TLS", "irc-screen:2"),
                             button(8204, "Protocol bounds", "irc-screen:3"),
                             button(8205, "Pending reads", "irc-screen:4"),
                             button(8206, "SASL credential", "irc-screen:5"),
                             button(8207, "Session and reads", "irc-screen:6"),
                             button(8208, "Message draft", "irc-screen:7")};
      root.children.push_back(std::move(ircScreens));
      if (ircPage_ == 0) {
        addFields(
            root,
            {label(8210, "Verified TLS IRC connection definition"),
             label(8211, "Immutable connection name"),
             field(8212, "Immutable connection name", ircName_,
                   "field:irc-name"),
             label(8213, "TLS server host"),
             field(8214, "TLS server host", ircHost_, "field:irc-host"),
             label(8215, "TLS port"),
             field(8216, "TLS port", ircPort_, "field:irc-port"),
             label(8217, "Account scope claim"),
             field(8218, "Account scope claim", ircAccount_,
                   "field:irc-account"),
             label(8219, "Network scope claim"),
             field(8220, "Network scope claim", ircNetworkScope_,
                   "field:irc-network-scope"),
             descriptionLabel(8227,
                              "Account and network scopes are caller claims, "
                              "not verified identities.",
                              1),
             button(8228, "Next: identity", "irc-screen:1")});
      } else if (ircPage_ == 1) {
        addFields(
            root,
            {label(8230, "IRC client identity"), label(8231, "Nickname"),
             field(8232, "Nickname", ircNick_, "field:irc-nick"),
             label(8233, "Username"),
             field(8234, "Username", ircUsername_, "field:irc-username"),
             label(8235, "Real name"),
             field(8236, "Real name", ircRealName_, "field:irc-real-name"),
             descriptionLabel(
                 8251,
                 "This identity is part of the saved connection "
                 "definition; it does not establish account ownership.",
                 2),
             button(8252, "Next: TLS", "irc-screen:2")});
      } else if (ircPage_ == 2) {
        addFields(root,
                  {label(8230, "TLS trust"),
                   label(8231, "Custom CA bundle path (optional)"),
                   field(8232, "Custom CA bundle path (optional)", ircCaBundle_,
                         "field:irc-ca-bundle"),
                   label(8233, "Connection timeout milliseconds"),
                   field(8234, "Connection timeout milliseconds", ircTimeoutMs_,
                         "field:irc-timeout"),
                   descriptionLabel(
                       8251,
                       "Certificate and hostname verification are required. "
                       "Sessions never reconnect or rejoin automatically.",
                       2),
                   button(8252, "Next: protocol bounds", "irc-screen:3")});
      } else if (ircPage_ == 3) {
        addFields(
            root,
            {label(8230, "Protocol processing bounds"),
             label(8235, "Maximum IRC line bytes"),
             field(8236, "Maximum IRC line bytes", ircLineBudget_,
                   "field:irc-line-budget"),
             label(8237, "Maximum tag bytes"),
             field(8238, "Maximum tag bytes", ircTagBudget_,
                   "field:irc-tag-budget"),
             label(8239, "Maximum batch depth"),
             field(8240, "Maximum batch depth", ircBatchDepth_,
                   "field:irc-batch-depth"),
             label(8241, "Maximum events per read"),
             field(8242, "Maximum events per read", ircEventBudget_,
                   "field:irc-event-budget"),
             label(8243, "Maximum tracked read targets"),
             field(8244, "Maximum tracked read targets", ircReadTargetBudget_,
                   "field:irc-target-budget"),
             descriptionLabel(8251,
                              "These limits bound protocol parsing and each "
                              "explicit read operation.",
                              2),
             button(8252, "Next: pending read bounds", "irc-screen:4")});
      } else if (ircPage_ == 4) {
        addFields(
            root,
            {label(8230, "Pending read buffer bounds"),
             label(8245, "Maximum pending read tickets"),
             field(8246, "Maximum pending read tickets", ircPendingReadBudget_,
                   "field:irc-pending-read-budget"),
             label(8247, "Maximum pending records"),
             field(8248, "Maximum pending records", ircPendingRecordBudget_,
                   "field:irc-pending-record-budget"),
             label(8249, "Maximum pending bytes"),
             field(8250, "Maximum pending bytes", ircPendingByteBudget_,
                   "field:irc-pending-byte-budget"),
             descriptionLabel(
                 8251,
                 "Pending snapshots replay until durable retention "
                 "succeeds; these limits bound unacknowledged results.",
                 2),
             button(8252, "Save immutable connection", "add-irc")});
      } else if (ircPage_ == 5) {
        auto username = field(8262, "SASL username", ircSaslUsername_,
                              "field:irc-sasl-username");
        auto password = field(8264, "SASL password", ircSaslPassword_,
                              "field:irc-sasl-password");
        std::get<ui::TextField>(username.model).sensitive = true;
        std::get<ui::TextField>(password.model).sensitive = true;
        addFields(
            root,
            {label(8260, "Session-only SASL PLAIN credential"),
             label(8261, "Saved IRC connection name"),
             field(8263, "Saved IRC connection name", ircName_,
                   "field:irc-name"),
             label(8265, "SASL username (protected)"), std::move(username),
             label(8266, "SASL password (protected)"), std::move(password),
             label(8267, "Lease lifetime in milliseconds (1–900000)"),
             field(8268, "Lease lifetime in milliseconds",
                   ircCredentialLifetime_, "field:irc-credential-lifetime"),
             descriptionLabel(
                 8269,
                 "Credentials remain in host memory only and are sent "
                 "after verified TLS and SASL negotiation. Leaving "
                 "this screen clears the inputs.",
                 3)});
        ui::Widget actions{.id = 8270, .model = ui::ButtonFlow{}};
        actions.children = {
            button(8271, "Bind SASL credential", "bind-irc-sasl"),
            button(8272, "Revoke SASL credential", "revoke-irc-sasl")};
        root.children.push_back(std::move(actions));
      } else if (ircPage_ == 6) {
        addFields(
            root,
            {label(8280, "Explicit IRC session operations"),
             label(8281, "Saved connection name"),
             field(8282, "Saved connection name", ircName_, "field:irc-name"),
             label(8283, "Joined channel or configured nick"),
             field(8284, "Joined channel or configured nick", ircTarget_,
                   "field:irc-target"),
             label(8285, "Maximum live messages"),
             field(8286, "Maximum live messages", ircMessageLimit_,
                   "field:irc-message-limit"),
             label(8287, "History after message ID (optional)"),
             field(8288, "History after message ID", ircAfterMessageId_,
                   "field:irc-after-message-id"),
             label(8289, "Maximum history messages"),
             field(8290, "Maximum history messages", ircHistoryLimit_,
                   "field:irc-history-limit"),
             descriptionLabel(
                 8291,
                 "Connect, join, live read and history are separate "
                 "actions. Poll retains observations before advancing "
                 "a read ticket.",
                 2)});
        ui::Widget actions{.id = 8292, .model = ui::ButtonFlow{}};
        actions.children = {button(8293, "Connect", "irc-connect"),
                            button(8294, "Join target", "irc-join"),
                            button(8295, "Read live events", "irc-live"),
                            button(8296, "Read history", "irc-history"),
                            button(8297, "Poll and retain", "poll"),
                            button(8298, "Cancel operation", "irc-cancel"),
                            button(8299, "Close session", "irc-close")};
        root.children.push_back(std::move(actions));
        root.children.push_back(detailList(8300, state.details, 4000,
                                           "IRC observations and source gaps "
                                           "appear as annotatable evidence."));
      } else {
        addFields(root,
                  {label(8310, "Local IRC message draft"),
                   label(8311, "Saved connection name"),
                   field(8312, "Saved connection name", ircDraftConnection_,
                         "field:irc-draft-connection"),
                   label(8313, "Joined channel or configured nick"),
                   field(8314, "Joined channel or configured nick",
                         ircDraftTarget_, "field:irc-draft-target"),
                   label(8315, "Message text (one IRC protocol line)"),
                   multilineField(8316, "Message text (one IRC protocol line)",
                                  ircDraftPayload_, "field:irc-draft-payload",
                                  ircDraftPayloadState_.caret,
                                  ircDraftPayloadState_.horizontalScrollPx,
                                  ircDraftPayloadState_.verticalScrollPx,
                                  ircDraftPayloadState_.preferredColumn,
                                  ircDraftPayloadState_.followCaret, 160.0F),
                   descriptionLabel(
                       8317,
                       "Embedded CR or LF is refused; one payload is never "
                       "split into multiple sends. Creating the intent sends "
                       "nothing. Review, grant, and dispatch are separate.",
                       2),
                   button(8318, "Create local message intent", "draft-irc")});
      }
    } else {
      addFields(
          root,
          {label(700, "Read a public Mastodon-compatible account collection"),
           label(711, "Connection revision name"),
           field(701, "Connection revision name", communicationName_,
                 "field:communication-name"),
           label(712, "HTTPS instance origin"),
           field(702, "HTTPS instance origin", communicationOrigin_,
                 "field:communication-origin"),
           label(713, "Origin-local account ID"),
           field(703, "Origin-local account ID", communicationAccount_,
                 "field:communication-account"),
           label(714, "Known ActivityPub actor URI"),
           field(704, "Known ActivityPub actor URI", communicationActor_,
                 "field:communication-actor"),
           descriptionLabel(
               705,
               "Use the instance account ID and verified actor URI; "
               "profile URLs do not establish identity.",
               1),
           [&] {
             ui::Widget actions{.id = 706, .model = ui::ButtonFlow{}};
             actions.children = {
                 button(707, "Read public statuses", "read-public-fediverse"),
                 button(708, "Check capture status", "poll"),
                 button(709, "Cancel read", "cancel-capture")};
             return actions;
           }(),
           descriptionLabel(710,
                            "Inspect captures in Evidence; annotate them in "
                            "Notes. Each read adds an observation.",
                            1)});
    }
    break;
  }
  case 7: {
    ui::Widget screens{.id = 800, .model = ui::ButtonFlow{}};
    screens.children = {button(801, "Join inputs", "join-screen:0"),
                        button(802, "Evaluation limits", "join-screen:1"),
                        button(803, "Retention limits", "join-screen:2"),
                        button(804, "Back to compare", "page:4")};
    root.children.push_back(std::move(screens));
    if (joinPage_ == 0) {
      addFields(root, {label(805, "Finite join over saved SQLite observations"),
                       label(811, "Left observation ID"),
                       field(810, "Left observation ID", joinLeftObservation_,
                             "field:join-left-observation"),
                       label(813, "Left key column"),
                       field(812, "Left key column", joinLeftKey_,
                             "field:join-left-key"),
                       label(815, "Right observation ID"),
                       field(814, "Right observation ID", joinRightObservation_,
                             "field:join-right-observation"),
                       label(817, "Right key column"),
                       field(816, "Right key column", joinRightKey_,
                             "field:join-right-key")});
      ui::Widget policies{.id = 818, .model = ui::ButtonFlow{}};
      policies.children = {
          button(819, "Join: " + joinKind_, "join-kind"),
          button(820,
                 joinCardinality_ == "many" ? "Cardinality: many"
                                            : "Cardinality: at most one",
                 "join-cardinality"),
          button(821,
                 joinNulls_ == "never" ? "NULLs: never match" : "NULLs: equal",
                 "join-nulls")};
      root.children.push_back(std::move(policies));
      root.children.push_back(
          descriptionLabel(822,
                           "Keys compare storage class and exact retained "
                           "bytes. Duplicate rows remain distinct.",
                           1));
    } else if (joinPage_ == 1) {
      addFields(root, {label(805, "Bounds for this local evaluation"),
                       label(811, "Maximum combined input rows"),
                       field(810, "Maximum combined input rows", joinRows_,
                             "field:join-rows"),
                       label(813, "Maximum total key bytes"),
                       field(812, "Maximum total key bytes", joinBytes_,
                             "field:join-bytes"),
                       label(815, "Maximum occurrence pairs"),
                       field(814, "Maximum occurrence pairs", joinPairs_,
                             "field:join-pairs"),
                       label(817, "Maximum evaluation work"),
                       field(816, "Maximum evaluation work", joinWork_,
                             "field:join-work"),
                       descriptionLabel(822,
                                        "A computation cutoff is explicit and "
                                        "does not change input coverage.",
                                        1)});
    } else {
      addFields(
          root,
          {label(805, "Bounds for source inspection and retained lineage"),
           label(811, "Maximum records scanned"),
           field(810, "Maximum records scanned", joinScan_, "field:join-scan"),
           label(813, "Maximum derived records"),
           field(812, "Maximum derived records", joinRetained_,
                 "field:join-retained"),
           label(815, "Maximum query metadata bytes"),
           field(814, "Maximum query metadata bytes", joinMetadata_,
                 "field:join-metadata"),
           label(817, "Maximum scanned record bytes"),
           field(816, "Maximum scanned record bytes", joinScanBytes_,
                 "field:join-scan-bytes"),
           descriptionLabel(822,
                            "Pinned input, row and key witnesses remain "
                            "inspectable and annotatable.",
                            1)});
    }
    ui::Widget actions{.id = 829, .model = ui::ButtonFlow{}};
    actions.children = {button(830, "Compute join", "compute-join"),
                        button(831, "Check query status", "poll"),
                        button(832, "Cancel query", "cancel-capture"),
                        button(833, "Inspect query evidence", "page:2")};
    root.children.push_back(std::move(actions));
    break;
  }
  case 8: {
    ui::Widget screens{.id = 850, .model = ui::ButtonFlow{}};
    screens.children = {button(851, "Inputs", "window-screen:0"),
                        button(852, "Bounds and lateness", "window-screen:1"),
                        button(853, "Limits and run", "window-screen:2"),
                        button(854, "Back to compare", "page:4")};
    root.children.push_back(std::move(screens));
    if (windowPage_ == 0) {
      addFields(
          root,
          {label(855, "Window one retained SQLite observation"),
           label(8560, "Observation record ID"),
           field(856, "Observation record ID", windowObservation_,
                 "field:window-observation"),
           button(857,
                  windowTimeBasis_ == "event" ? "Time basis: event time"
                                              : "Time basis: observation time",
                  "window-basis"),
           label(8580,
                 windowTimeBasis_ == "event"
                     ? "Event timestamp column (Unix milliseconds)"
                     : "Event timestamp column (unused in observation time)"),
           field(858,
                 windowTimeBasis_ == "event"
                     ? "Event timestamp column (Unix milliseconds)"
                     : "Event timestamp column (unused in observation time)",
                 windowTimeColumn_, "field:window-time-column"),
           label(8590, "Arrival watermark column (optional)"),
           field(859, "Arrival watermark column (optional)",
                 windowArrivalColumn_, "field:window-arrival-column"),
           label(8600, "Partition key column (optional)"),
           field(860, "Partition key column (optional)", windowKeyColumn_,
                 "field:window-key-column"),
           descriptionLabel(
               861,
               "Observation time uses the retained capture start. Event time "
               "uses a selected integer or timestamp column. Arrival "
               "watermarks classify lateness per occurrence.",
               3)});
    } else if (windowPage_ == 1) {
      ui::Widget policies{.id = 870, .model = ui::ButtonFlow{}};
      policies.children = {
          button(871, "Late data: " + windowLatePolicy_, "window-policy")};
      addFields(
          root,
          {label(862, "Finite half-open window bounds in Unix milliseconds"),
           label(8630, "Start inclusive (milliseconds)"),
           field(863, "Start inclusive (milliseconds)", windowStartMs_,
                 "field:window-start-ms"),
           label(8640, "End exclusive (milliseconds)"),
           field(864, "End exclusive (milliseconds)", windowEndMs_,
                 "field:window-end-ms"),
           label(8650, "Finalization watermark (milliseconds)"),
           field(865, "Finalization watermark (milliseconds)",
                 windowWatermarkMs_, "field:window-watermark-ms"),
           std::move(policies),
           descriptionLabel(
               866,
               "The declared watermark can reach the window end while "
               "source gaps or unknown lateness remain. Lateness is "
               "determined only from each row's arrival watermark.",
               3)});
    } else {
      addFields(
          root,
          {label(872, "Bounded local evaluation and retained lineage"),
           label(8730, "Maximum output occurrences (default 1000)"),
           field(873, "Maximum output occurrences (default 1000)",
                 windowOutput_, "field:window-output"),
           label(8740, "Maximum retained records (default 10000)"),
           field(874, "Maximum retained records (default 10000)",
                 windowRetained_, "field:window-retained"),
           label(8750, "Maximum evaluation work"),
           field(875, "Maximum evaluation work", windowWork_,
                 "field:window-work"),
           label(8760, "Maximum records scanned"),
           field(876, "Maximum records scanned", windowScan_,
                 "field:window-scan"),
           label(8770, "Maximum scan bytes"),
           field(877, "Maximum scan bytes", windowScanBytes_,
                 "field:window-scan-bytes"),
           descriptionLabel(
               878,
               "Inputs, source gaps, keys, scan bytes, work, output and "
               "retained lineage are bounded. Cutoffs and source coverage "
               "are reported separately.",
               2)});
      ui::Widget actions{.id = 880, .model = ui::ButtonFlow{}};
      actions.children = {button(881, "Run window", "run-window"),
                          button(882, "Poll", "poll"),
                          button(883, "Cancel window", "cancel-window")};
      root.children.push_back(std::move(actions));
      root.children.push_back(detailList(
          884, state.details, 4000,
          "Window coverage, gaps, truncation and lineage appear here."));
    }
    break;
  }
  }
  setModel(std::move(root));
  invalidate();
}

} // namespace xanadu::evidence
