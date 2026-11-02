#include <gtest/gtest.h>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string_view>
#include <thread>
#include <tuple>

#include "common/ui/evidence/evidence_case_panel.hpp"
#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {

bool hasText(const gleditor::ui::WidgetScene &scene, std::string_view needle) {
  return std::ranges::any_of(scene.visuals, [needle](const auto &visual) {
    return visual.text.find(needle) != std::string::npos ||
           visual.accessibleLabel.find(needle) != std::string::npos;
  });
}

TEST(EvidencePanelTest, ShowsTheJourneyAsReachableNamedScreens) {
  auto permascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::evidence::EvidenceCasePanel panel(std::move(permascroll));
  panel.setVisible(true);
  gleditor::ui::UiMetrics metrics{.screenWidth = 1600, .screenHeight = 1200};

  auto scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Case directory"));
  EXPECT_TRUE(hasText(*scene, "New case"));

  ASSERT_TRUE(panel.activate(6)); // Sources tab.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "SQLite path or REST URL"));
  EXPECT_TRUE(hasText(*scene, "Add REST GET source"));
  EXPECT_TRUE(hasText(*scene, "Refresh"));

  ASSERT_TRUE(panel.activate(80)); // Communications tab.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "HTTPS instance origin"));
  EXPECT_TRUE(hasText(*scene, "Known ActivityPub actor URI"));
  EXPECT_TRUE(hasText(*scene, "Read public statuses"));
  EXPECT_TRUE(hasText(*scene, "Cancel read"));

  ASSERT_TRUE(panel.activate(7)); // Evidence tab.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Captured requests, coverage, schema fields"));

  ASSERT_TRUE(panel.activate(8)); // Notes tab.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Annotate selected field"));
  EXPECT_TRUE(hasText(*scene, "Annotate exact span"));

  ASSERT_TRUE(panel.activate(9)); // Compare and export tab.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Compare observations"));
  EXPECT_TRUE(hasText(*scene, "Prepare selective export"));
}

TEST(EvidencePanelTest, MailCredentialsAreProtectedAndClearedOnNavigation) {
  auto permascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::evidence::EvidenceCasePanel panel(std::move(permascroll));
  panel.setVisible(true);
  gleditor::ui::UiMetrics metrics{.screenWidth = 1280, .screenHeight = 900};
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.activate(80));
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.activate(723));
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.typeInto(752, "synthetic-private-login"));
  ASSERT_TRUE(panel.typeInto(754, "synthetic-private-password"));
  auto scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_FALSE(hasText(*scene, "synthetic-private-login"));
  EXPECT_FALSE(hasText(*scene, "synthetic-private-password"));
  ASSERT_NE(scene->find(754), nullptr);
  EXPECT_EQ(scene->find(754)->value.find("synthetic-private-password"),
            std::string::npos);
  ASSERT_TRUE(panel.activate(722));
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.activate(723));
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_NE(scene->find(754), nullptr);
  EXPECT_TRUE(scene->find(754)->value.empty());
  ASSERT_TRUE(panel.typeInto(754, "another-private-password"));
  panel.setVisible(false)->setVisible(true);
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_NE(scene->find(754), nullptr);
  EXPECT_TRUE(scene->find(754)->value.empty());
}

TEST(EvidencePanelTest, SubmitCredentialScreenIsReachableAndMasked) {
  auto permascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::evidence::EvidenceCasePanel panel(std::move(permascroll));
  panel.setVisible(true);
  gleditor::ui::UiMetrics metrics{.screenWidth = 1280, .screenHeight = 900};
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(80)); // Communications.
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(727)); // Submit credential.
  const auto scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(
      hasText(*scene, "Session-only credential for one exact SMTP intent"));
  EXPECT_TRUE(hasText(*scene, "Bind submit credential"));
  EXPECT_TRUE(hasText(*scene, "Review and grant intent"));
  EXPECT_TRUE(hasText(*scene, "Mail submit intent ID"));
  EXPECT_TRUE(hasText(*scene, "Username (protected)"));
  EXPECT_TRUE(hasText(*scene, "Password (protected)"));
  ASSERT_NE(scene->find(812), nullptr);
  ASSERT_NE(scene->find(814), nullptr);
  EXPECT_EQ(scene->find(812)->accessibilityRole,
            gleditor::a11y::Role::PasswordInput);
  EXPECT_EQ(scene->find(814)->accessibilityRole,
            gleditor::a11y::Role::PasswordInput);
  ASSERT_TRUE(panel.typeInto(812, "synthetic-submit-login"));
  ASSERT_TRUE(panel.typeInto(814, "synthetic-submit-password"));
  const auto protectedScene =
      panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(protectedScene);
  EXPECT_FALSE(hasText(*protectedScene, "synthetic-submit-login"));
  EXPECT_FALSE(hasText(*protectedScene, "synthetic-submit-password"));
  EXPECT_EQ(protectedScene->find(812)->value.find("synthetic-submit-login"),
            std::string::npos);
  EXPECT_EQ(protectedScene->find(814)->value.find("synthetic-submit-password"),
            std::string::npos);
  const auto mask = [](const std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < count; ++i) result += "•";
    return result;
  };
  EXPECT_EQ(protectedScene->find(812)->value,
            mask(std::string("synthetic-submit-login").size()));
  EXPECT_EQ(protectedScene->find(814)->value,
            mask(std::string("synthetic-submit-password").size()));
}

TEST(EvidencePanelTest, IrcSessionAndSaslControlsAreExplicitAndProtected) {
  auto permascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::evidence::EvidenceCasePanel panel(std::move(permascroll));
  panel.setVisible(true);
  const gleditor::ui::UiMetrics metrics{.screenWidth  = 1280,
                                        .screenHeight = 900};
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(80)); // Communications.
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(728)); // IRC connection and session.
  auto scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Verified TLS IRC connection definition"));
  EXPECT_TRUE(hasText(*scene, "Account scope claim"));
  ASSERT_TRUE(panel.activate(8202)); // Client identity.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Real name"));
  ASSERT_TRUE(panel.activate(8203)); // TLS trust.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Certificate and hostname verification"));
  ASSERT_TRUE(panel.activate(8205)); // Pending read bounds.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Save immutable connection"));
  ASSERT_TRUE(panel.activate(8206)); // SASL credential.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Session-only SASL PLAIN credential"));
  EXPECT_TRUE(hasText(*scene, "Bind SASL credential"));
  EXPECT_TRUE(hasText(*scene, "Revoke SASL credential"));
  ASSERT_NE(scene->find(8262), nullptr);
  ASSERT_NE(scene->find(8264), nullptr);
  EXPECT_EQ(scene->find(8262)->accessibilityRole,
            gleditor::a11y::Role::PasswordInput);
  EXPECT_EQ(scene->find(8264)->accessibilityRole,
            gleditor::a11y::Role::PasswordInput);
  ASSERT_TRUE(panel.typeInto(8262, "synthetic-sasl-login"));
  ASSERT_TRUE(panel.typeInto(8264, "synthetic-sasl-password"));
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_FALSE(hasText(*scene, "synthetic-sasl-login"));
  EXPECT_FALSE(hasText(*scene, "synthetic-sasl-password"));
  EXPECT_EQ(scene->find(8262)->value.find("synthetic-sasl-login"),
            std::string::npos);
  EXPECT_EQ(scene->find(8264)->value.find("synthetic-sasl-password"),
            std::string::npos);
  ASSERT_TRUE(panel.activate(8207)); // Leaving credential screen clears it.
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(8206));
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(scene->find(8262)->value.empty());
  EXPECT_TRUE(scene->find(8264)->value.empty());

  ASSERT_TRUE(panel.activate(8207)); // Session and reads.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Connect"));
  EXPECT_TRUE(hasText(*scene, "Join target"));
  EXPECT_TRUE(hasText(*scene, "Read live events"));
  EXPECT_TRUE(hasText(*scene, "Read history"));
  EXPECT_TRUE(hasText(*scene, "Poll and retain"));
  EXPECT_TRUE(hasText(*scene, "Cancel operation"));
  EXPECT_TRUE(hasText(*scene, "Close session"));
  ASSERT_TRUE(panel.activate(8203)); // TLS trust.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "never reconnect or rejoin automatically"));

  ASSERT_TRUE(panel.activate(8208)); // Message draft.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Local IRC message draft"));
  EXPECT_TRUE(hasText(*scene, "Create local message intent"));
  EXPECT_TRUE(hasText(*scene, "Creating the intent sends nothing"));
  EXPECT_FALSE(hasText(*scene, "dispatching"));
}

TEST(EvidencePanelTest,
     MailDraftEnvelopeContentAndReviewKeepCaptionsAndOccurrencesVisible) {
  const auto unique =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("gleditor-mail-draft-panel-" + std::to_string(unique));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{path};

  auto permascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::evidence::EvidenceCasePanel panel(permascroll);
  panel.setVisible(true);
  const gleditor::ui::UiMetrics metrics{.screenWidth  = 1280,
                                        .screenHeight = 900};
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.typeInto(11, path.string()));
  ASSERT_TRUE(panel.typeInto(12, "Mail draft UI"));
  ASSERT_TRUE(panel.activate(13)); // Create the private case.
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(80)); // Communications.
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(725)); // SMTP connection.
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  EXPECT_TRUE(hasText(*panel.prepare(metrics, gleditor::ui::defaultTheme()),
                      "Connection revision name"));
  ASSERT_TRUE(panel.typeInto(781, "smtp-local"));
  ASSERT_TRUE(panel.typeInto(782, "smtps://smtp.example.invalid"));
  ASSERT_TRUE(panel.typeInto(783, "test-account"));
  auto scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Connection revision name"));
  EXPECT_TRUE(hasText(*scene, "SMTP URL (smtps:// or STARTTLS smtp://)"));
  ASSERT_TRUE(panel.activate(787)); // Save definition; no network action.
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));

  ASSERT_TRUE(panel.activate(726)); // Mail draft, Envelope.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(
      hasText(*scene, "To mailboxes (one per line; duplicates preserved)"));
  EXPECT_TRUE(
      hasText(*scene, "Bcc mailboxes (one per line; hidden from headers)"));
  ASSERT_NE(scene->find(791), nullptr);
  EXPECT_EQ(scene->find(791)->value, "smtp-local");
  ASSERT_TRUE(panel.typeInto(792, "from@example.invalid"));
  ASSERT_TRUE(panel.typeInto(793, "Synthetic UI draft"));
  ASSERT_TRUE(panel.typeInto(794, "one@example.invalid"));
  ASSERT_TRUE(panel.keyInto(794, gleditor::Key::Return));
  ASSERT_TRUE(panel.typeInto(794, "one@example.invalid"));
  ASSERT_TRUE(panel.typeInto(795, "copy@example.invalid"));
  ASSERT_TRUE(panel.typeInto(796, "blind@example.invalid"));

  ASSERT_TRUE(panel.activate(800)); // Continue to message content.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Plain text body"));
  EXPECT_TRUE(hasText(*scene, "HTML body (optional)"));
  ASSERT_TRUE(panel.typeInto(797, "first line"));
  ASSERT_TRUE(panel.keyInto(797, gleditor::Key::Return));
  ASSERT_TRUE(panel.typeInto(797, "second line"));
  ASSERT_TRUE(panel.typeInto(798, "<p>synthetic</p>"));

  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(801)); // Continue to exact review.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Review the exact local draft"));
  ASSERT_NE(scene->find(800), nullptr);
  EXPECT_EQ(scene->find(800)->action, "create-mail-draft");
  EXPECT_TRUE(hasText(*scene, "To #1: one@example.invalid"));
  std::string reviewedRows;
  for (std::size_t step = 0; step < 20; ++step) {
    scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
    ASSERT_TRUE(scene);
    for (const auto &visual : scene->visuals) {
      reviewedRows += visual.text;
      reviewedRows.push_back('\n');
      reviewedRows += visual.accessibleLabel;
      reviewedRows.push_back('\n');
    }
    ASSERT_TRUE(panel.scrollList(802, 32.0F));
  }
  EXPECT_NE(reviewedRows.find("To #2: one@example.invalid"), std::string::npos);
  EXPECT_NE(reviewedRows.find("Bcc #1: blind@example.invalid"),
            std::string::npos);
  EXPECT_NE(reviewedRows.find("first line"), std::string::npos);
  EXPECT_NE(reviewedRows.find("second line"), std::string::npos);
  EXPECT_NE(reviewedRows.find("<p>synthetic</p>"), std::string::npos);
  ASSERT_TRUE(panel.scrollList(802, -10000.0F));
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  ASSERT_TRUE(panel.activate(800)); // Create a durable local intent only.
  const auto draftSnapshot = panel.controller().snapshot();
  const auto &records      = draftSnapshot.records;
  EXPECT_TRUE(std::ranges::any_of(records, [](const auto &record) {
    return record.kind == "effect-intent" &&
           record.attributes.contains("operation") &&
           record.attributes.at("operation") == "mail-submit";
  })) << draftSnapshot.status;
  EXPECT_FALSE(std::ranges::any_of(records, [](const auto &record) {
    return record.kind == "effect-attempt";
  }));
}

TEST(EvidencePanelTest, GrantLifetimeAndRetryRiskStayVisible) {
  auto permascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::evidence::EvidenceCasePanel panel(std::move(permascroll));
  panel.setVisible(true);
  gleditor::ui::UiMetrics metrics{.screenWidth = 1280, .screenHeight = 900};
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());

  ASSERT_TRUE(panel.activate(10)); // Work.
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.activate(602)); // Review and grant.
  const auto scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "300000"));
  const auto firstCaution  = scene->find(638);
  const auto secondCaution = scene->find(637);
  ASSERT_NE(firstCaution, nullptr);
  ASSERT_NE(secondCaution, nullptr);
  EXPECT_TRUE(hasText(*scene, "Intent ID"));
  EXPECT_TRUE(hasText(*scene, "Grant lifetime (milliseconds)"));
  EXPECT_TRUE(hasText(*scene, "Grant ID"));
  EXPECT_TRUE(hasText(*scene, "Retry acknowledgement"));
  EXPECT_EQ(firstCaution->fitted.lines, 1);
  EXPECT_FALSE(firstCaution->fitted.truncated);
  EXPECT_EQ(secondCaution->fitted.lines, 1);
  EXPECT_FALSE(secondCaution->fitted.truncated);
  EXPECT_NE(firstCaution->text.find("separate attempt"), std::string::npos);
  EXPECT_NE(secondCaution->text.find("possible repeated effects"),
            std::string::npos);
}

TEST(EvidencePanelTest, WindowInputsBoundsAndLimitsAreReachableByNamedScreens) {
  auto permascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::evidence::EvidenceCasePanel panel(std::move(permascroll));
  panel.setVisible(true);
  gleditor::ui::UiMetrics metrics{.screenWidth = 1280, .screenHeight = 900};
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(9)); // Compare and export.
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(71)); // Window observations.
  auto scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Window one retained SQLite observation"));
  EXPECT_TRUE(hasText(*scene, "Observation record ID"));
  EXPECT_TRUE(hasText(*scene, "Arrival watermark column (optional)"));

  ASSERT_TRUE(panel.activate(852)); // Bounds and lateness.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Start inclusive (milliseconds)"));
  EXPECT_TRUE(hasText(*scene, "Finalization watermark (milliseconds)"));
  EXPECT_TRUE(hasText(*scene, "Late data: exclude"));
  ASSERT_TRUE(panel.activate(871));
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Late data: include"));

  ASSERT_TRUE(panel.activate(853)); // Limits and run.
  scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  EXPECT_TRUE(hasText(*scene, "Maximum output occurrences (default 1000)"));
  EXPECT_TRUE(hasText(*scene, "Maximum retained records (default 10000)"));
  EXPECT_TRUE(hasText(*scene, "Run window"));
  EXPECT_TRUE(hasText(*scene, "Cancel window"));
}

TEST(EvidencePanelTest, WindowFormRunsAgainstAStoredSqliteObservation) {
  const auto unique =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() /
                         ("gleditor-window-panel-" + std::to_string(unique));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  std::filesystem::create_directories(directory);
  const auto database = directory / "events.sqlite";
  sqlite3 *raw{};
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  char *error{};
  const auto sqlStatus = sqlite3_exec(
      raw,
      "CREATE TABLE events(ts INTEGER, arrival INTEGER, part TEXT);"
      "INSERT INTO events VALUES(10,15,'a'),(20,15,'b'),(200,50,'c');",
      nullptr, nullptr, &error);
  const std::string sqlError = error ? error : "SQLite fixture failed";
  sqlite3_free(error);
  sqlite3_close(raw);
  ASSERT_EQ(sqlStatus, SQLITE_OK) << sqlError;

  auto permascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::evidence::EvidenceCasePanel panel(permascroll);
  const auto casePath = directory / "case";
  ASSERT_TRUE(panel.controller().execute("new \"" + casePath.string() +
                                         "\" \"Window panel\""));
  ASSERT_TRUE(panel.controller().execute(
      "sqlite events \"" + database.string() +
      "\" \"SELECT ts, arrival AS 'arrival mark', part AS 'part key' "
      "FROM events ORDER BY rowid\""));
  ASSERT_TRUE(panel.controller().execute("refresh events"));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline &&
         panel.controller().poll() == false)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  ASSERT_TRUE(std::ranges::any_of(
      panel.controller().snapshot().records,
      [](const auto &record) { return record.kind == "observation"; }));
  const auto snapshot    = panel.controller().snapshot();
  const auto observation = std::ranges::find(snapshot.records, "observation",
                                             &xanadu::evidence::Record::kind);
  ASSERT_NE(observation, snapshot.records.end());

  panel.setVisible(true);
  const gleditor::ui::UiMetrics metrics{.screenWidth  = 1280,
                                        .screenHeight = 900};
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(9)); // Compare and export.
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(71)); // Window observations.
  ASSERT_TRUE(panel.typeInto(856, std::to_string(observation->id)));
  ASSERT_TRUE(panel.typeInto(858, "ts"));
  ASSERT_TRUE(panel.typeInto(859, "arrival mark"));
  ASSERT_TRUE(panel.typeInto(860, "part key"));
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(852)); // Bounds and lateness.
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.typeInto(863, "0"));
  ASSERT_TRUE(panel.typeInto(864, "100"));
  ASSERT_TRUE(panel.typeInto(865, "100"));
  ASSERT_TRUE(panel.activate(871)); // Include late rows.
  ASSERT_TRUE(panel.activate(853)); // Limits and run.
  ASSERT_TRUE(panel.prepare(metrics, gleditor::ui::defaultTheme()));
  ASSERT_TRUE(panel.activate(881)); // Run window.

  const auto runDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < runDeadline) {
    if (panel.controller().poll()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  const auto results = panel.controller().snapshot().records;
  const auto run =
      std::ranges::find(results, "query-run", &xanadu::evidence::Record::kind);
  ASSERT_NE(run, results.end());
  const auto window = std::ranges::find_if(results, [&](const auto &record) {
    return record.kind == "query-window" && record.parent == run->id;
  });
  ASSERT_NE(window, results.end());
  EXPECT_EQ(std::ranges::count_if(results,
                                  [&](const auto &record) {
                                    return record.kind == "query-member" &&
                                           record.parent == window->id;
                                  }),
            2);
}

TEST(EvidencePanelTest, NewSourceAndNoteUseVisibleOverlayControls) {
  const auto unique =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("gleditor-evidence-panel-" + std::to_string(unique));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{path};

  auto permascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::evidence::EvidenceCasePanel panel(permascroll);
  panel.setVisible(true);
  gleditor::ui::UiMetrics metrics{.screenWidth = 1600, .screenHeight = 1200};
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.typeInto(11, path.string()));
  ASSERT_TRUE(panel.typeInto(12, "Case from visible controls"));
  ASSERT_TRUE(panel.activate(13));
  EXPECT_TRUE(panel.controller().snapshot().open);

  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.activate(6)); // Sources.
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.typeInto(21, "sample-api"));
  ASSERT_TRUE(panel.typeInto(22, "https://example.invalid/items"));
  ASSERT_TRUE(panel.activate(26)); // Add REST source, no request yet.
  const auto withSource = panel.controller().snapshot();
  ASSERT_TRUE(std::ranges::any_of(withSource.records, [](const auto &record) {
    return record.kind == "connection" && record.label == "sample-api";
  }));

  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.activate(8)); // Notes.
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.typeInto(41, "Keep this endpoint for review"));
  ASSERT_TRUE(panel.activate(42));
  const auto withNote = panel.controller().snapshot();
  EXPECT_TRUE(std::ranges::any_of(withNote.records, [](const auto &record) {
    return record.kind == "note" &&
           record.text == "Keep this endpoint for review";
  }));
}

TEST(EvidencePanelTest, LocatesUtf8BytesPreviewsThenPinsTheVerifiedSpan) {
  const auto unique =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("gleditor-evidence-span-" + std::to_string(unique));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{path};

  auto permascroll    = std::make_shared<xanadu::UserPermascroll>();
  const auto casePath = path / "case";
  xanadu::evidence::RecordId pageId{};
  const std::string raw = "Prefix: café is exact. Suffix.";
  {
    xanadu::evidence::EvidenceCase evidence(permascroll);
    evidence.create(casePath, "UTF-8 span review");
    xanadu::evidence::Source source{.name = "fixture",
                                    .kind =
                                        xanadu::evidence::SourceKind::SQLite,
                                    .locator = "fixture.sqlite",
                                    .query   = "select memo from fixture"};
    const auto sourceId = evidence.addSource(source);
    xanadu::evidence::CaptureResult capture;
    capture.source  = source;
    capture.state   = xanadu::evidence::CaptureState::Complete;
    capture.columns = {{"memo", "TEXT"}};
    capture.rows    = {{{{xanadu::evidence::StorageClass::Text, raw}}}};
    static_cast<void>(evidence.retain(sourceId, capture));
    for (const auto &record : evidence.records()) {
      if (record.kind == "value" && record.label == "memo") pageId = record.id;
    }
    ASSERT_NE(pageId, 0U);
    evidence.save();
  }

  xanadu::evidence::EvidenceCasePanel panel(permascroll);
  panel.setVisible(true);
  const gleditor::ui::UiMetrics metrics{.screenWidth  = 1600,
                                        .screenHeight = 1200};
  const auto prepareFrame = [&] {
    std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  };
  prepareFrame();
  ASSERT_TRUE(panel.typeInto(11, casePath.string()));
  ASSERT_TRUE(panel.activate(14)); // Open the prepared private case.
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.activate(7)); // Evidence.
  ASSERT_TRUE(panel.typeInto(36, "observed value · memo"));
  const auto scene = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(scene);
  const auto recordAction = "record:" + std::to_string(pageId);
  const auto recordVisual = std::ranges::find(
      scene->visuals, recordAction, &gleditor::ui::WidgetVisual::action);
  ASSERT_NE(recordVisual, scene->visuals.end());
  ASSERT_TRUE(panel.activate(recordVisual->id));
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.activate(8)); // Notes.
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.typeInto(43, "café"));

  const auto beforeLocate = panel.controller().evidenceCase().store().latest();
  ASSERT_TRUE(panel.activate(44));
  auto state        = panel.controller().snapshot();
  const auto offset = raw.find("café");
  ASSERT_EQ(state.spanTarget, pageId);
  ASSERT_TRUE(state.span);
  EXPECT_EQ(state.span->offset, offset);
  EXPECT_EQ(state.span->length, std::string("café").size());
  EXPECT_EQ(panel.controller().evidenceCase().store().latest(), beforeLocate);

  ASSERT_TRUE(panel.activate(47));
  state = panel.controller().snapshot();
  ASSERT_EQ(state.spanTarget, pageId);
  ASSERT_TRUE(state.span);
  EXPECT_EQ(state.span->offset, offset);
  EXPECT_EQ(state.span->length, std::string("café").size());
  EXPECT_EQ(panel.controller().evidenceCase().store().latest(), beforeLocate);
  const auto previewScene =
      panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(previewScene);
  EXPECT_TRUE(hasText(*previewScene, "Verified preview"));
  EXPECT_TRUE(hasText(*previewScene, "Selected bytes: caf\\xC3\\xA9"));
  const auto selectedBytes =
      std::ranges::find_if(previewScene->visuals, [](const auto &visual) {
        return visual.text.starts_with("Selected bytes:");
      });
  ASSERT_NE(selectedBytes, previewScene->visuals.end());
  EXPECT_TRUE(selectedBytes->interactive);
  EXPECT_TRUE(selectedBytes->action.empty());

  ASSERT_TRUE(panel.typeInto(41, "The accented field was checked"));
  ASSERT_TRUE(panel.activate(50));
  state = panel.controller().snapshot();
  xanadu::evidence::RecordId noteId{};
  xanadu::evidence::RecordId quoteId{};
  for (const auto &record : state.records) {
    if (record.kind == "note" &&
        record.text == "The accented field was checked")
      noteId = record.id;
    if (record.kind == "quote" && record.parent == noteId) quoteId = record.id;
  }
  ASSERT_NE(noteId, 0U);
  ASSERT_NE(quoteId, 0U);
  const auto quote = panel.controller().evidenceCase().inspect(quoteId);
  EXPECT_EQ(quote.attributes.at("offset"), std::to_string(offset));
  EXPECT_EQ(quote.attributes.at("length"),
            std::to_string(std::string("café").size()));
  EXPECT_EQ(panel.controller().evidenceCase().rawText(pageId), raw);

  ASSERT_TRUE(panel.activate(7)); // Evidence.
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.activate(34)); // Reveal selectable metadata records.
  while (true) {
    const auto filterScene =
        panel.prepare(metrics, gleditor::ui::defaultTheme());
    ASSERT_TRUE(filterScene);
    const auto *filterVisual = filterScene->find(36);
    ASSERT_NE(filterVisual, nullptr);
    if (filterVisual->value.empty()) break;
    ASSERT_TRUE(panel.keyInto(36, gleditor::Key::End));
    ASSERT_TRUE(panel.keyInto(36, gleditor::Key::Backspace));
  }
  ASSERT_TRUE(panel.typeInto(36, "coverage"));
  const auto metadataScene =
      panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(metadataScene);
  xanadu::evidence::RecordId coverageId{};
  for (const auto &record : panel.controller().snapshot().records) {
    if (record.kind == "attribute" && record.label == "coverage")
      coverageId = record.id;
  }
  ASSERT_NE(coverageId, 0U);
  const auto metadataAction = "record:" + std::to_string(coverageId);
  const auto metadataVisual =
      std::ranges::find(metadataScene->visuals, metadataAction,
                        &gleditor::ui::WidgetVisual::action);
  ASSERT_NE(metadataVisual, metadataScene->visuals.end());
  EXPECT_NE(metadataVisual->text.find("parent #"), std::string::npos);
  ASSERT_TRUE(panel.activate(metadataVisual->id));
  EXPECT_EQ(panel.controller().snapshot().selected, coverageId);
  std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(panel.activate(8)); // Notes.
  while (true) {
    const auto noteScene = panel.prepare(metrics, gleditor::ui::defaultTheme());
    ASSERT_TRUE(noteScene);
    const auto *noteVisual = noteScene->find(41);
    ASSERT_NE(noteVisual, nullptr);
    if (noteVisual->value.empty()) break;
    ASSERT_TRUE(panel.keyInto(41, gleditor::Key::End));
    ASSERT_TRUE(panel.keyInto(41, gleditor::Key::Backspace));
  }
  ASSERT_TRUE(panel.typeInto(41, "Coverage metadata reviewed"));
  ASSERT_TRUE(panel.activate(42));
  state  = panel.controller().snapshot();
  noteId = 0;
  for (const auto &record : state.records) {
    if (record.kind == "note" && record.text == "Coverage metadata reviewed")
      noteId = record.id;
  }
  ASSERT_NE(noteId, 0U);
  const auto targetReference =
      std::ranges::find_if(state.records, [&](const auto &record) {
        return record.kind == "reference" && record.parent == noteId &&
               record.label == "annotation target";
      });
  ASSERT_NE(targetReference, state.records.end());
  EXPECT_EQ(targetReference->text, std::to_string(coverageId));
}

TEST(EvidencePanelTest, FakeWorkRequiresVisibleReviewGrantAndDispatchSteps) {
  const auto unique =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("gleditor-evidence-work-" + std::to_string(unique));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{path};

  auto permascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::evidence::EvidenceCasePanel panel(permascroll);
  panel.setVisible(true);
  const gleditor::ui::UiMetrics metrics{.screenWidth  = 1600,
                                        .screenHeight = 1200};
  const auto prepareFrame = [&] {
    std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  };
  prepareFrame();
  ASSERT_TRUE(panel.typeInto(11, path.string()));
  ASSERT_TRUE(panel.typeInto(12, "Visible work journey"));
  ASSERT_TRUE(panel.activate(13));
  ASSERT_TRUE(panel.activate(10)); // Work.
  prepareFrame();
  ASSERT_TRUE(panel.typeInto(610, "fake review journey"));
  ASSERT_TRUE(panel.typeInto(611, "0"));
  ASSERT_TRUE(panel.activate(621)); // Draft only.

  auto state = panel.controller().snapshot();
  ASSERT_TRUE(state.selected);
  const auto intent = *state.selected;
  EXPECT_EQ(panel.controller().evidenceCase().inspect(intent).kind,
            "effect-intent");
  EXPECT_FALSE(std::ranges::any_of(state.records, [intent](const auto &record) {
    return record.kind == "effect-attempt" && record.parent == intent;
  }));

  ASSERT_TRUE(panel.activate(602)); // Review and grant screen.
  prepareFrame();
  ASSERT_TRUE(panel.activate(631)); // Review only.
  EXPECT_NE(panel.controller().snapshot().status.find("Review only"),
            std::string::npos);
  prepareFrame();
  ASSERT_TRUE(panel.activate(632)); // Grant is a separate action.
  state = panel.controller().snapshot();
  ASSERT_TRUE(state.selected);
  const auto grant = *state.selected;
  EXPECT_EQ(panel.controller().evidenceCase().inspect(grant).kind,
            "effect-authority");
  prepareFrame();
  ASSERT_TRUE(panel.activate(633)); // Dispatch is separate from grant.

  std::optional<std::string> outcome;
  for (int attempt = 0; attempt < 100; ++attempt) {
    ASSERT_TRUE(panel.activate(603)); // Pending work.
    prepareFrame();
    ASSERT_TRUE(panel.activate(637)); // Poll and refresh.
    prepareFrame();
    state = panel.controller().snapshot();
    xanadu::evidence::RecordId attemptId{};
    for (const auto &record : state.records) {
      if (record.kind == "effect-attempt" && record.parent == intent)
        attemptId = record.id;
    }
    if (attemptId != 0)
      for (const auto &record : state.records)
        if (record.kind == "effect-receipt" && record.parent == attemptId &&
            record.attributes.contains("outcome"))
          outcome = record.attributes.at("outcome");
    if (outcome && *outcome != "admitted") break;
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  ASSERT_TRUE(outcome);
  EXPECT_EQ(*outcome, "completed");

  ASSERT_TRUE(panel.activate(5)); // Case.
  prepareFrame();
  ASSERT_TRUE(panel.activate(16)); // Close and revoke the session grant.
  xanadu::evidence::EvidenceCasePanel reopened(permascroll);
  reopened.setVisible(true);
  std::ignore = reopened.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(reopened.typeInto(11, path.string()));
  ASSERT_TRUE(reopened.activate(14)); // Reopen the retained case.
  ASSERT_TRUE(reopened.activate(10)); // Work.
  std::ignore = reopened.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(reopened.activate(602)); // Review and grant screen.
  std::ignore = reopened.prepare(metrics, gleditor::ui::defaultTheme());
  ASSERT_TRUE(reopened.typeInto(622, std::to_string(intent)));
  ASSERT_TRUE(reopened.typeInto(623, std::to_string(grant)));
  const auto attemptsBefore = std::ranges::count_if(
      reopened.controller().snapshot().records,
      [](const auto &record) { return record.kind == "effect-attempt"; });
  ASSERT_TRUE(reopened.activate(633)); // Old grant is no longer authority.
  EXPECT_EQ(std::ranges::count_if(reopened.controller().snapshot().records,
                                  [](const auto &record) {
                                    return record.kind == "effect-attempt";
                                  }),
            attemptsBefore);
  EXPECT_NE(reopened.controller().snapshot().status.find("authorize"),
            std::string::npos);
}

TEST(EvidencePanelTest, HistoryTextIsImportedWithoutCreatingAnAttempt) {
  const auto unique =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("gleditor-evidence-history-" + std::to_string(unique));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{path};

  xanadu::evidence::EvidenceCasePanel panel(
      std::make_shared<xanadu::UserPermascroll>());
  panel.setVisible(true);
  const gleditor::ui::UiMetrics metrics{.screenWidth  = 1600,
                                        .screenHeight = 1200};
  const auto prepareFrame = [&] {
    std::ignore = panel.prepare(metrics, gleditor::ui::defaultTheme());
  };
  prepareFrame();
  ASSERT_TRUE(panel.typeInto(11, path.string()));
  ASSERT_TRUE(panel.typeInto(12, "History evidence journey"));
  ASSERT_TRUE(panel.activate(13));
  ASSERT_TRUE(panel.activate(10)); // Work.
  prepareFrame();
  ASSERT_TRUE(panel.activate(608)); // History import mode.
  prepareFrame();
  ASSERT_TRUE(panel.typeInto(610, "shell history export"));
  ASSERT_TRUE(panel.typeInto(611, "rm -rf /\n"));
  ASSERT_TRUE(panel.activate(621)); // Import history; do not dispatch.
  const auto state = panel.controller().snapshot();
  EXPECT_TRUE(std::ranges::any_of(state.records, [](const auto &record) {
    return record.kind == "history-import" ||
           record.kind == "history-occurrence";
  }));
  EXPECT_FALSE(std::ranges::any_of(state.records, [](const auto &record) {
    return record.kind == "effect-attempt";
  }));
}

} // namespace
