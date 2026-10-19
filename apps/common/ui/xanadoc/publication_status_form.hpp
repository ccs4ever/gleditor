#ifndef COMMON_UI_XANADOC_PUBLICATION_STATUS_FORM_HPP
#define COMMON_UI_XANADOC_PUBLICATION_STATUS_FORM_HPP

#include "common/xanadu/publication_inbox.hpp"
#include <functional>
#include <gleditor/form.hpp>
#include <span>
#include <string>

namespace xanadu {
enum class PublicationDownloadAction {
  Refresh,
  Open,
  Retry,
  Cancel,
  Close,
  Subscribe,
  DiscardPartialData
};

std::string publicationCitationAvailability(
    const PublicationPin &pin,
    std::span<const PublicationDownloadStatus> requests);

std::string publicationDownloadSummary(const PublicationDownloadStatus &status);

void openPublicationDownloadForm(
    gleditor::Form &form, const PublicationDownloadStatus &status,
    std::function<void(PublicationDownloadAction)> accepted,
    gleditor::Form::Cancelled cancelled = {});
} // namespace xanadu
#endif
