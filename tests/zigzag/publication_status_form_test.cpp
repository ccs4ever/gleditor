#include "common/ui/xanadoc/publication_status_form.hpp"

#include <gleditor/a11y/publisher.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace {
using namespace xanadu;
PublicationDownloadStatus requested(PublicationDownloadPhase phase) {
  PublicationPin pin{.publisher = createMutableKeys().publicKey,
                     .salt      = "document:ideas",
                     .hash      = InfoHash::fromHex(std::string(40, 'a')),
                     .sequence  = 7,
                     .version   = MicroversionId::parse("a34"),
                     .title     = "Café ideas"};
  return {.id = "request", .pin = pin, .phase = phase};
}
} // namespace

TEST(PublicationStatusFormTest, FailedRequestKeepsExactIdentityWithoutOpen) {
  auto status  = requested(PublicationDownloadPhase::Failed);
  status.error = "No publication transport configured";
  gleditor::Form form;
  std::optional<PublicationDownloadAction> chosen;
  openPublicationDownloadForm(form, status,
                              [&](auto action) { chosen = action; });
  const auto fields = form.current();
  ASSERT_EQ(fields.size(), 6U);
  EXPECT_THAT(
      fields[0].options,
      testing::ElementsAre("Refresh progress", "Retry download", "Close"));
  EXPECT_EQ(fields[1].answer(), "Not checked");
  EXPECT_EQ(fields[2].answer(), status.pin->hash.hex());
  EXPECT_EQ(fields[3].answer(), "Café ideas / a34 #7");
  EXPECT_EQ(fields[4].answer(), status.pin->salt);
  EXPECT_THAT(fields[5].optionDescriptions,
              testing::Each(status.pin->publisher.hex()));
  EXPECT_THAT(publicationDownloadSummary(status),
              testing::HasSubstr(status.error));
  form.keyPressed(gleditor::Key::Right, gleditor::KeyMods::None);
  form.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);
  EXPECT_EQ(chosen, PublicationDownloadAction::Retry);
}

TEST(PublicationStatusFormTest, PendingAndManifestVerifiedDoNotOfferArrival) {
  auto status = requested(PublicationDownloadPhase::Resolving);
  gleditor::Form form;
  std::optional<PublicationDownloadAction> chosen;
  openPublicationDownloadForm(form, status,
                              [&](auto action) { chosen = action; });
  EXPECT_EQ(publicationDownloadSummary(status), "Fetching pinned publication");
  EXPECT_THAT(
      form.current()[0].options,
      testing::ElementsAre("Refresh progress", "Cancel download", "Close"));
  form.keyPressed(gleditor::Key::Right, gleditor::KeyMods::None);
  form.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);
  EXPECT_EQ(chosen, PublicationDownloadAction::Cancel);
  status.phase        = PublicationDownloadPhase::Failed;
  status.verification = PublicationDownloadVerification::ManifestVerified;
  status.error        = "Dependency bytes unavailable";
  openPublicationDownloadForm(form, status,
                              [&](auto action) { chosen = action; });
  EXPECT_EQ(form.current()[1].answer(),
            "Signed manifest verified; content incomplete");
  EXPECT_THAT(form.current()[0].options,
              testing::Not(testing::Contains("Open completed publication")));
}

TEST(PublicationStatusFormTest,
     CompleteStillRequiresExplicitOpenOrSubscription) {
  auto status = requested(PublicationDownloadPhase::Ready);
  gleditor::Form form;
  std::optional<PublicationDownloadAction> chosen;
  openPublicationDownloadForm(form, status,
                              [&](auto action) { chosen = action; });
  EXPECT_THAT(form.current()[0].options,
              testing::Not(testing::Contains("Open completed publication")));
  status.verification = PublicationDownloadVerification::Complete;
  openPublicationDownloadForm(form, status,
                              [&](auto action) { chosen = action; });
  EXPECT_EQ(form.current()[1].answer(),
            "Signed manifest and complete content verified");
  form.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);
  EXPECT_EQ(chosen, PublicationDownloadAction::Refresh);
  openPublicationDownloadForm(form, status,
                              [&](auto action) { chosen = action; });
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder into(tree, 16);
  form.describe(into);
  ASSERT_TRUE(tree.find(into.id(144)));
  EXPECT_EQ(tree.find(into.id(144))->value, status.pin->hash.hex());
  ASSERT_TRUE(tree.find(into.id(336)));
  EXPECT_EQ(tree.find(into.id(336))->description, status.pin->publisher.hex());
  ASSERT_TRUE(
      form.performAction(into.id(18), gleditor::a11y::Action::Click, {}));
  form.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);
  EXPECT_EQ(chosen, PublicationDownloadAction::Open);
  openPublicationDownloadForm(form, status,
                              [&](auto action) { chosen = action; });
  ASSERT_TRUE(
      form.performAction(into.id(19), gleditor::a11y::Action::Click, {}));
  form.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);
  EXPECT_EQ(chosen, PublicationDownloadAction::Subscribe);
}

TEST(PublicationStatusFormTest, UnresolvedNameDoesNotInventAnImmutableTarget) {
  PublicationDownloadStatus status{.uri   = "mutable request",
                                   .phase = PublicationDownloadPhase::Queued};
  gleditor::Form form;
  bool cancelled = false;
  openPublicationDownloadForm(
      form, status, [](auto) {}, [&] { cancelled = true; });
  ASSERT_EQ(form.current().size(), 4U);
  EXPECT_EQ(form.current()[2].answer(), status.uri);
  EXPECT_EQ(form.current()[3].answer(), "Awaiting signed manifest");
  form.keyPressed(gleditor::Key::Escape, gleditor::KeyMods::None);
  EXPECT_TRUE(cancelled);
}

TEST(PublicationStatusFormTest, CitationAvailabilityMatchesEveryPinnedField) {
  const auto status = requested(PublicationDownloadPhase::Failed);
  const auto pin    = *status.pin;
  EXPECT_EQ(publicationCitationAvailability(pin, {}), "Not checked");
  std::vector<PublicationDownloadStatus> requests{status};
  EXPECT_EQ(publicationCitationAvailability(pin, requests),
            "Request failed; open status");
  requests[0].phase        = PublicationDownloadPhase::Ready;
  requests[0].verification = PublicationDownloadVerification::Complete;
  EXPECT_EQ(publicationCitationAvailability(pin, requests),
            "Complete local copy");
  requests[0].pin->title += " different";
  EXPECT_EQ(publicationCitationAvailability(pin, requests), "Not checked");
  requests[0].pin          = pin;
  requests[0].pin->version = MicroversionId::parse("a35");
  EXPECT_EQ(publicationCitationAvailability(pin, requests), "Not checked");
  requests[0].pin       = pin;
  requests[0].pin->hash = InfoHash::fromHex(std::string(40, 'b'));
  EXPECT_EQ(publicationCitationAvailability(pin, requests), "Not checked");
  requests[0].pin   = pin;
  requests[0].phase = PublicationDownloadPhase::Verifying;
  requests.push_back(status);
  EXPECT_EQ(publicationCitationAvailability(pin, requests), "Checking");
}
