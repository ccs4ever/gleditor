#include "views.hpp"

#include <gleditor/logging.hpp>

namespace xanadu {
namespace {
using Field = gleditor::Form::Field;
Field choice(std::string label, std::vector<std::string> labels,
             std::vector<std::string> values = {}) {
  Field field;
  field.label        = std::move(label);
  field.kind         = gleditor::Form::Kind::Choice;
  field.options      = std::move(labels);
  field.optionValues = std::move(values);
  if (field.options.empty()) {
    field.options      = {"None"};
    field.optionValues = {""};
  }
  return field;
}
Field actions(std::vector<std::string> labels,
              std::vector<std::string> values) {
  auto field          = choice("Action", std::move(labels), std::move(values));
  field.submitOnEnter = true;
  return field;
}
std::string sourceLabel(const Publication &pub) {
  return pub.title + " #" + std::to_string(pub.sequence) + " — " +
         pub.publisher.hex().substr(0, 12) + " / " + pub.version.str();
}
Field sources(const std::vector<Publication> &pubs) {
  auto field = choice("Signed source publication", {}, {});
  if (!pubs.empty()) {
    field.options.clear();
    field.optionValues.clear();
  }
  for (std::size_t i = 0; i < pubs.size(); ++i) {
    field.options.push_back(sourceLabel(pubs[i]));
    field.optionValues.push_back(std::to_string(i));
  }
  return field;
}
std::string rangeOf(const GlobalSpan &span) {
  return "[" + std::to_string(span.start) + ", " + std::to_string(span.end()) +
         ")";
}
Field keyParts(const std::string &label, const std::string &key) {
  std::vector<std::string> parts;
  for (std::size_t start = 0; start < key.size();) {
    auto end = std::min(start + 32, key.size());
    while (end > start && end < key.size() &&
           (static_cast<unsigned char>(key[end]) & 0xc0) == 0x80)
      --end;
    if (end == start) ++end;
    parts.push_back(std::to_string(parts.size() + 1) + ": " +
                    key.substr(start, end - start));
    start = end;
  }
  auto field = choice(label, std::move(parts));
  field.optionDescriptions.assign(field.options.size(), key);
  return field;
}
Field endset(const std::string &label, const std::vector<GlobalSpan> &spans) {
  auto field = choice(label, {});
  field.options.clear();
  field.optionValues.clear();
  for (const auto &span : spans) {
    field.options.push_back(rangeOf(span) + " — " + span.scroll);
    field.optionValues.push_back(rangeOf(span) + " " + span.scroll);
    field.optionDescriptions.push_back(field.optionValues.back());
  }
  return field;
}
} // namespace
void Views::publishIndependentLinks() {
  renderer->runWithState([this](RenderState &) {
    try {
      const auto pubs = session.packagePublicationSources();
      Field title;
      title.label = "Package title";
      Field salt;
      salt.label = "Package name";
      salt.value = "curations:links";
      form.open(
          "Publish independent links",
          "Choose a signed snapshot containing authored links. Review before "
          "publishing.",
          {actions({"Prepare for review", "Review downloaded package", "Close"},
                   {"prepare", "cached", "close"}),
           sources(pubs), std::move(title), std::move(salt)},
          [this, pubs](const auto &answers) {
            renderer->runWithState([this, pubs, answers](RenderState &) {
              try {
                if (answers[0].answer() == "close") return;
                if (answers[0].answer() == "cached") {
                  linksAndResponses("cached");
                  return;
                }
                if (answers[1].answer().empty())
                  throw std::runtime_error(
                      "Publish or download a signed source publication first.");
                const auto &source = pubs.at(std::stoull(answers[1].answer()));
                if (source.links.empty())
                  throw std::runtime_error(
                      "This signed snapshot contains no authored links. Create "
                      "links in the document, then publish that snapshot "
                      "first.");
                const auto id = session.prepareLinkPackage(
                    source, answers[3].answer(), answers[2].answer(), false);
                linkPackageStatus(id);
              } catch (const std::exception &error) {
                state->showDialog(render::DiagnosticSeverity::Error,
                                  "Package preparation unavailable",
                                  error.what());
              }
            });
          });
    } catch (const std::exception &error) {
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Package preparation unavailable", error.what());
    }
  });
}
void Views::linksAndResponses(const std::string &query) {
  renderer->runWithState([this, query](RenderState &) {
    try {
      if (query == "cached" || query == "matching" || query == "reader" ||
          query == "removed") {
        const auto statuses = session.linkPackageExchange().statuses();
        auto &reader        = session.readerLinkPackages();
        auto packages       = choice("Immutable package carrier", {}, {});
        std::map<std::string, std::string> ids;
        std::size_t retainedCount = 0, enabledCount = 0, knownLinks = 0,
                    enabledLinks = 0, unavailable = 0;
        for (const auto &[hash, preference] : reader.preferences()) {
          if (!preference.retained) continue;
          ++retainedCount;
          if (preference.enabled) ++enabledCount;
          if (const auto *payload =
                  reader.verifiedPackage(ReaderLinkPackages::keyOf(hash, 0))) {
            knownLinks += payload->links.size();
            if (preference.enabled) enabledLinks += payload->links.size();
          } else
            ++unavailable;
        }
        const auto append = [&](const InfoHash &hash, std::string label) {
          if (packages.options == std::vector<std::string>{"None"}) {
            packages.options.clear();
            packages.optionValues.clear();
          }
          packages.options.push_back(std::move(label));
          packages.optionValues.push_back(hash.hex());
        };
        for (const auto &status : statuses)
          ids.emplace(status.hash.hex(), status.id);
        if (query == "reader" || query == "removed") {
          for (const auto &[hash, preference] : reader.preferences()) {
            if (preference.retained != (query == "reader")) continue;
            const auto *payload =
                reader.verifiedPackage(ReaderLinkPackages::keyOf(hash, 0));
            append(hash,
                   (payload ? payload->title : "Payload unavailable") + " / " +
                       hash.hex() +
                       (preference.enabled ? " · enabled" : " · disabled") +
                       (preference.emphasis == PackageEmphasis::Quiet
                            ? " · quiet"
                            : " · normal"));
          }
        } else
          for (const auto &status : statuses) {
            if (query == "matching" &&
                (status.phase != LinkPackagePhase::Ready &&
                 status.phase != LinkPackagePhase::Published))
              continue;
            if (query == "matching" && packageFilterSource &&
                !packageReferences(status.package, *packageFilterSource))
              continue;
            append(status.hash,
                   status.package.title + " / " + status.hash.hex() + " · " +
                       std::string(linkPackagePhaseName(status.phase)) +
                       (reader.preference(status.hash).retained
                            ? " · retained"
                            : " · not retained"));
          }
        auto control =
            actions({"Show selected package", "Close", "Find packages"},
                    {"show", "close", "find"});
        if (query == "reader") {
          control.options.push_back("Remove from reader");
          control.optionValues.push_back("remove");
        }
        if (query == "removed") {
          control.options.push_back("Restore to reader");
          control.optionValues.push_back("restore");
        }
        const auto inventory =
            std::to_string(retainedCount) + " retained carriers / " +
            std::to_string(enabledCount) + " enabled; " +
            std::to_string(knownLinks) + " known retained links / " +
            std::to_string(enabledLinks) + " drawable links; " +
            std::to_string(unavailable) + " retained payloads unavailable.";
        form.open(
            query == "reader"    ? "Reader package inventory"
            : query == "removed" ? "Removed reader packages"
                                 : "Downloaded link packages",
            inventory +
                " Downloaded candidates are separate from reader retention.",
            {std::move(control), std::move(packages)},
            [this, query, ids = std::move(ids)](const auto &answers) {
              renderer->runWithState([this, query, ids,
                                      answers](RenderState &) {
                const auto action  = answers[0].answer();
                const auto carrier = answers[1].answer();
                if (action == "find") {
                  linksAndResponses();
                  return;
                }
                if (action == "close" || carrier.empty()) return;
                try {
                  if (action == "remove") {
                    if (auto result = session.setLinkPackageRetained(
                            ReaderLinkPackages::keyOf(
                                InfoHash::fromHex(carrier), 0),
                            false);
                        !result)
                      throw std::runtime_error(result.error());
                    if (packageVisibilityChanged) packageVisibilityChanged();
                    linksAndResponses(query);
                    return;
                  }
                  const auto found = ids.find(carrier);
                  if (found == ids.end())
                    throw std::runtime_error(
                        "Exact verified payload unavailable for carrier " +
                        carrier +
                        ". Fetch and verify this immutable carrier to restore "
                        "it.");
                  if (action == "restore") {
                    if (auto result = session.setLinkPackageRetained(
                            session.linkPackageExchange().status(found->second),
                            true);
                        !result)
                      throw std::runtime_error(result.error());
                    if (packageVisibilityChanged) packageVisibilityChanged();
                    linksAndResponses(query);
                    return;
                  }
                  linkPackageStatus(found->second);
                } catch (const std::exception &error) {
                  state->showDialog(render::DiagnosticSeverity::Error,
                                    "Reader package unavailable", error.what());
                }
              });
            });
        return;
      }
      if (query.empty()) {
        const auto pubs = session.packagePublicationSources();
        form.open(
            "Links and responses",
            "Search the source's registered global scrolls. No curator key is "
            "required.",
            {actions(
                 {"Find packages", "Review downloaded packages", "Close",
                  "Review packages matching selected passages",
                  "Reader package inventory", "Removed reader packages"},
                 {"find", "cached", "close", "matching", "reader", "removed"}),
             sources(pubs)},
            [this, pubs](const auto &answers) {
              renderer->runWithState([this, pubs, answers](RenderState &) {
                try {
                  if (answers[0].answer() == "close") return;
                  if (answers[0].answer() == "cached" ||
                      answers[0].answer() == "reader" ||
                      answers[0].answer() == "removed") {
                    linksAndResponses(answers[0].answer());
                    return;
                  }
                  if (answers[1].answer().empty())
                    throw std::runtime_error(
                        "Publish or download the signed work first.");
                  const auto source = pubs.at(std::stoull(answers[1].answer()));
                  packageFilterSource = source;
                  if (answers[0].answer() == "matching") {
                    linksAndResponses("matching");
                    return;
                  }
                  std::vector<std::string> keys;
                  for (const auto &[key, scroll] :
                       pubs.at(std::stoull(answers[1].answer())).scrolls) {
                    (void)scroll;
                    keys.push_back(key);
                  }
                  const auto id = session.publicationDiscovery().submitLinks(
                      std::move(keys));
                  packageQuerySources.insert_or_assign(id, source);
                  linksAndResponses(id);
                } catch (const std::exception &error) {
                  state->showDialog(render::DiagnosticSeverity::Error,
                                    "Links and responses unavailable",
                                    error.what());
                }
              });
            });
        return;
      }
      const auto status = session.publicationDiscovery().status(query);
      std::vector<std::pair<SignedAuthorCatalog, AuthorCatalogEntry>> entries;
      auto packages = choice("Advertised package", {}, {});
      for (const auto &catalog : status.catalogs)
        for (const auto &entry : catalog.entries) {
          if (entry.kind != CatalogEntryKind::LinkPackage ||
              !std::ranges::any_of(status.scrollKeys, [&](const auto &key) {
                return std::ranges::find(entry.scrollKeys, key) !=
                       entry.scrollKeys.end();
              }))
            continue;
          if (entries.empty()) {
            packages.options.clear();
            packages.optionValues.clear();
          }
          packages.options.push_back(entry.title + " #" +
                                     std::to_string(entry.sequence) + " — " +
                                     catalog.publisher.hex().substr(0, 12));
          packages.optionValues.push_back(std::to_string(entries.size()));
          entries.emplace_back(catalog, entry);
        }
      const auto note =
          status.phase == DiscoveryPhase::Failed ? status.error
          : status.phase == DiscoveryPhase::Ready
              ? "Signed catalog metadata received; fetch to verify endpoints. "
                "Enrollment unchecked."
              : "Searching scroll rendezvous peers; refresh to check progress.";
      form.open(
          "Links and responses", note,
          {actions({"Refresh progress", "Fetch selected package",
                    "Retry discovery", "Review downloaded packages", "Close"},
                   {"refresh", "fetch", "retry", "cached", "close"}),
           std::move(packages)},
          [this, query, entries,
           keys = status.scrollKeys](const auto &answers) {
            renderer->runWithState([this, query, entries, keys,
                                    answers](RenderState &) {
              try {
                const auto action = answers[0].answer();
                if (action == "close") return;
                if (action == "cached") {
                  linksAndResponses("cached");
                  return;
                }
                if (action == "fetch" && !answers[1].answer().empty()) {
                  const auto &entry =
                      entries.at(std::stoull(answers[1].answer()));
                  const auto source = packageQuerySources.find(query);
                  linkPackageStatus(
                      session.linkPackageExchange().fetch(entry.first,
                                                          entry.second, keys),
                      source == packageQuerySources.end()
                          ? std::nullopt
                          : std::optional<Publication>{source->second});
                  return;
                }
                if (action == "retry")
                  (void)session.publicationDiscovery().submitLinks(keys);
                linksAndResponses(query);
              } catch (const std::exception &error) {
                state->showDialog(render::DiagnosticSeverity::Error,
                                  "Package discovery unavailable",
                                  error.what());
              }
            });
          });
    } catch (const std::exception &error) {
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Links and responses unavailable", error.what());
    }
  });
}
void Views::linkPackageStatus(const std::string &id,
                              std::optional<Publication> source) {
  renderer->runWithState([this, id, source](RenderState &) {
    try {
      const auto pkg = session.linkPackageExchange().status(id);

      auto control = actions({"Refresh progress", "Review signed endpoints",
                              "Retry", "Cancel", "Close"},
                             {"refresh", "review", "retry", "cancel", "close"});
      if (session.testPublicationSwarmEnabled() && !pkg.received &&
          !pkg.announce && pkg.phase == LinkPackagePhase::Ready) {
        control.options.push_back(
            "Publish reviewed package to test swarm (mock verification)");
        control.optionValues.push_back("publish");
      }
      auto note = std::string(linkPackagePhaseName(pkg.phase));
      if (pkg.identity == PublicationIdentity::MockVerified)
        note += " — mock verification";
      if (!pkg.error.empty()) note += " — " + pkg.error;
      const bool verified = pkg.phase == LinkPackagePhase::Ready ||
                            pkg.phase == LinkPackagePhase::Published;
      const bool matches =
          !source || !verified || packageReferences(pkg.package, *source);
      if (source && verified)
        note += matches ? " — overlaps selected document passages"
                        : " — no byte overlap with selected document passages";
      form.open(
          "Link package status", note,
          {std::move(control),
           choice("Package", {pkg.package.title.empty()
                                  ? id
                                  : pkg.package.title + " #" +
                                        std::to_string(pkg.package.sequence)})},
          [this, id, source, matches](const auto &answers) {
            renderer->runWithState([this, id, source, matches,
                                    answers](RenderState &) {
              try {
                const auto action = answers[0].answer();
                if (action == "close") return;
                if (action == "review") {
                  if (!matches)
                    throw std::runtime_error(
                        "This package references the scroll, but not the "
                        "selected document's passages. Review it in the "
                        "unfiltered downloaded-package list if desired.");
                  reviewIndependentLinks(id);
                  return;
                }
                if (action == "retry") session.linkPackageExchange().retry(id);
                if (action == "cancel")
                  session.linkPackageExchange().cancel(id);
                if (action == "publish") {
                  const auto pkg = session.linkPackageExchange().status(id);
                  (void)session.linkPackageExchange().submit(pkg.package, true);
                }
                linkPackageStatus(id, source);
              } catch (const std::exception &error) {
                state->showDialog(render::DiagnosticSeverity::Error,
                                  "Package unavailable", error.what());
              }
            });
          });
    } catch (const std::exception &error) {
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Package unavailable", error.what());
    }
  });
}
void Views::reviewIndependentLinks(const std::string &id,
                                   std::size_t selected) {
  renderer->runWithState([this, id, selected](RenderState &) {
    try {
      const auto status = session.linkPackageExchange().status(id);
      if (!canReviewLinkPackage(status))
        throw std::runtime_error(
            "Wait for package verification before reviewing endpoints.");
      const auto &pkg = status.package;
      reviewLinkPackage(pkg);
      const auto preference =
          session.readerLinkPackages().preference(status.hash);
      const auto enabled = preference.enabled;
      auto control =
          actions({"Inspect selected link", "Open related publication",
                   "Back to status", "Close", "Inspect full scroll identities"},
                  {"inspect", "open", "back", "close", "keys"});
      if (canReviewLinkPackage(status)) {
        control.options.push_back(preference.retained ? "Remove from reader"
                                                      : "Retain in reader");
        control.optionValues.push_back("retain");
      }
      if (preference.retained) {
        control.options.push_back(enabled ? "Disable reader layer"
                                          : "Enable reader layer");
        control.optionValues.push_back("toggle");
        control.options.push_back(preference.emphasis == PackageEmphasis::Quiet
                                      ? "Normal emphasis"
                                      : "Quiet emphasis");
        control.optionValues.push_back("emphasis");
      }
      if (enabled) {
        control.options.push_back("Select link for navigation");
        control.optionValues.push_back("navigate");
      }
      control.options.push_back("Compare reader editions");
      control.optionValues.push_back("compare");
      const auto index = std::min(selected, pkg.links.size() - 1);
      auto links       = choice("Link identity", {}, {});
      links.options.clear();
      links.optionValues.clear();
      links.chosen = index;
      for (std::size_t i = 0; i < pkg.links.size(); ++i) {
        links.options.push_back(std::to_string(i + 1) + " — " +
                                pkg.links[i].owner);
        links.optionValues.push_back(std::to_string(i));
      }
      auto pins = choice("Cited publication", {}, {});
      if (!pkg.publications.empty()) {
        pins.options.clear();
        pins.optionValues.clear();
      }
      for (std::size_t i = 0; i < pkg.publications.size(); ++i) {
        const auto &pin = pkg.publications[i];
        pins.options.push_back(pin.title + " #" + std::to_string(pin.sequence) +
                               " / " + pin.version.str());
        pins.optionDescriptions.push_back(
            pin.publisher.hex() + " / " + pin.salt + " #" +
            std::to_string(pin.sequence) + " / " + pin.version.str() + " / " +
            pin.hash.hex());
        pins.optionValues.push_back(std::to_string(i));
      }
      auto curator               = choice("Curator key", {pkg.curator.hex()});
      curator.optionDescriptions = {pkg.curator.hex()};
      form.open(
          "Review independent links",
          pkg.title + " — " +
              (enabled ? "reader layer enabled" : "reader layer disabled") +
              (preference.retained ? "; retained" : "; not retained") +
              (preference.emphasis == PackageEmphasis::Quiet
                   ? "; emphasis quiet"
                   : "; emphasis normal") +
              "; carrier " + status.hash.hex() +
              "; signature verified; enrollment unchecked. Cited publication "
              "bytes are checked separately when opened.",
          {std::move(control), std::move(curator), std::move(links),
           endset("Left endset", pkg.links[index].left),
           endset("Right endset", pkg.links[index].right), std::move(pins)},
          [this, id, index, pins = pkg.publications](const auto &answers) {
            renderer->runWithState([this, id, index, pins,
                                    answers](RenderState &) {
              try {
                const auto action = answers[0].answer();
                if (action == "close") return;
                if (action == "compare") {
                  compareReaderLinkPackages(id);
                  return;
                }
                if (action == "retain" || action == "emphasis") {
                  const auto status = session.linkPackageExchange().status(id);
                  const auto preference =
                      session.readerLinkPackages().preference(status.hash);
                  const auto result =
                      action == "retain"
                          ? session.setLinkPackageRetained(status,
                                                           !preference.retained)
                          : session.setLinkPackageEmphasis(
                                status.hash,
                                preference.emphasis == PackageEmphasis::Normal
                                    ? PackageEmphasis::Quiet
                                    : PackageEmphasis::Normal);
                  if (!result) throw std::runtime_error(result.error());
                  if (packageVisibilityChanged) packageVisibilityChanged();
                  reviewIndependentLinks(id, index);
                  return;
                }
                if (action == "toggle") {
                  const auto status = session.linkPackageExchange().status(id);
                  if (auto result = session.setLinkPackageEnabled(
                          status,
                          !session.readerLinkPackages().enabled(status.hash));
                      !result)
                    throw std::runtime_error(result.error());
                  if (packageVisibilityChanged) packageVisibilityChanged();
                  reviewIndependentLinks(id, index);
                  return;
                }
                if (action == "navigate") {
                  const auto status = session.linkPackageExchange().status(id);
                  if (!session.readerLinkPackages().enabled(status.hash))
                    throw std::runtime_error("Enable this reader layer first.");
                  if (selectIndependentLink)
                    selectIndependentLink(ReaderLinkPackages::keyOf(
                        status.hash, std::stoull(answers[2].answer())));
                  return;
                }
                if (action == "keys") {
                  inspectIndependentLinkKeys(id, index, answers[3].chosen,
                                             answers[4].chosen);
                  return;
                }
                if (action == "back") {
                  linkPackageStatus(id);
                  return;
                }
                if (action == "open" && !answers[5].answer().empty()) {
                  publicationDownloadStatus(
                      session.publicationInbox().submitPinned(
                          pins.at(std::stoull(answers[5].answer()))));
                  return;
                }
                reviewIndependentLinks(id, std::stoull(answers[2].answer()));
              } catch (const std::exception &error) {
                state->showDialog(render::DiagnosticSeverity::Error,
                                  "Related publication unavailable",
                                  error.what());
              }
            });
          });
    } catch (const std::exception &error) {
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Package review unavailable", error.what());
    }
  });
}
void Views::compareReaderLinkPackages(const std::string &id,
                                      std::optional<InfoHash> baseline,
                                      std::string notice) {
  renderer->runWithState([this, id, baseline, notice](RenderState &) {
    try {
      const auto status = session.linkPackageExchange().status(id);
      if (auto result = session.cacheReaderLinkPackage(status); !result)
        throw std::runtime_error(result.error());
      auto &reader        = session.readerLinkPackages();
      const auto editions = reader.editions(status.hash);
      auto choices        = choice("Retained edition", {}, {});
      choices.options.clear();
      choices.optionValues.clear();
      std::optional<InfoHash> shown;
      for (const auto &hash : editions) {
        const auto *package =
            reader.verifiedPackage(ReaderLinkPackages::keyOf(hash, 0));
        choices.options.push_back(package->title + " #" +
                                  std::to_string(package->sequence));
        choices.optionValues.push_back(hash.hex());
        choices.optionDescriptions.push_back("Immutable carrier: " +
                                             hash.hex());
        if (baseline && hash == *baseline)
          choices.chosen = choices.options.size() - 1;
      }
      if (!editions.empty())
        shown = editions[choices.chosen];
      else {
        choices.options = {
            "No other retained edition with this curator and name"};
        choices.optionValues = {""};
      }
      const auto describe = [&](const InfoHash &hash) {
        const auto *package =
            reader.verifiedPackage(ReaderLinkPackages::keyOf(hash, 0));
        const auto preference = reader.preference(hash);
        std::size_t left = 0, right = 0;
        for (const auto &link : package->links) {
          left += link.left.size();
          right += link.right.size();
        }
        return package->title + " #" + std::to_string(package->sequence) +
               " · " + (preference.retained ? "retained" : "not retained") +
               " / " + (preference.enabled ? "enabled" : "disabled") + " / " +
               (preference.emphasis == PackageEmphasis::Quiet ? "quiet"
                                                              : "normal") +
               " · " + std::to_string(package->links.size()) + " links, L" +
               std::to_string(left) + "/R" + std::to_string(right);
      };
      auto control = actions({"Refresh comparison", "Keep current drawing",
                              "Back to candidate review"},
                             {"refresh", "keep", "back"});
      if (!reader.preference(status.hash).retained) {
        control.options.push_back("Retain candidate without enabling");
        control.optionValues.push_back("retain");
      } else if (shown && reader.enabled(*shown)) {
        control.options.push_back("Switch drawing to candidate");
        control.optionValues.push_back("switch");
      }
      const auto note =
          "Endpoints verified; curator unchecked; cited bytes checked when "
          "opened. "
          "Candidate " +
          describe(status.hash) +
          (shown ? ". Baseline " + describe(*shown)
                 : ". No verified retained baseline is available") +
          ". Switching keeps the old pinned Link.";
      form.open(
          "Compare reader package editions", note,
          {std::move(control),
           choice("Comparison status",
                  {notice.empty() ? "Review pair before switching" : notice}),
           std::move(choices),
           choice("Baseline immutable carrier",
                  {shown ? shown->hex() : "Unavailable"}),
           choice("Candidate immutable carrier", {status.hash.hex()}),
           choice("Curator package name", {status.package.salt}),
           keyParts("Curator key", status.package.curator.hex())},
          [this, id, shown, candidate = status.hash](const auto &answers) {
            renderer->runWithState([this, id, shown, candidate,
                                    answers](RenderState &) {
              try {
                const auto action = answers[0].answer();
                if (action == "keep") return;
                if (action == "back") {
                  reviewIndependentLinks(id);
                  return;
                }
                const auto chosen =
                    answers[2].answer().empty()
                        ? std::nullopt
                        : std::optional{InfoHash::fromHex(answers[2].answer())};
                if (action == "refresh") {
                  compareReaderLinkPackages(id, chosen);
                  return;
                }
                const auto status = session.linkPackageExchange().status(id);
                if (status.hash != candidate)
                  throw std::runtime_error(
                      "Candidate immutable carrier changed; reopen comparison");
                if (action == "retain") {
                  if (auto result =
                          session.setLinkPackageRetained(status, true);
                      !result)
                    throw std::runtime_error(result.error());
                } else if (action == "switch") {
                  if (!shown || chosen != shown) {
                    compareReaderLinkPackages(id, chosen,
                                              "Switch refused. Pair refreshed; "
                                              "review before switching.");
                    return;
                  }
                  if (auto result =
                          session.switchLinkPackageEnabled(*shown, status);
                      !result)
                    throw std::runtime_error(result.error());
                }
                if (packageVisibilityChanged) packageVisibilityChanged();
                compareReaderLinkPackages(id, chosen);
              } catch (const std::exception &error) {
                state->showDialog(render::DiagnosticSeverity::Error,
                                  "Reader edition unavailable", error.what());
              }
            });
          });
    } catch (const std::exception &error) {
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Reader edition comparison unavailable", error.what());
    }
  });
}
void Views::inspectIndependentLinkKeys(const std::string &id,
                                       std::size_t selected, std::size_t left,
                                       std::size_t right) {
  renderer->runWithState([this, id, selected, left, right](RenderState &) {
    try {
      const auto pkg = session.linkPackageExchange().status(id).package;
      reviewLinkPackage(pkg);
      const auto &link = pkg.links.at(selected);
      const auto &lhs  = link.left.at(left);
      const auto &rhs  = link.right.at(right);
      form.open("Inspect scroll identities",
                "Link " + std::to_string(selected + 1) +
                    " — selected endpoint identities in numbered parts.",
                {actions({"Back to link review", "Close"}, {"back", "close"}),
                 choice("Left range", {rangeOf(lhs)}),
                 choice("Right range", {rangeOf(rhs)}),
                 keyParts("Left scroll", lhs.scroll),
                 keyParts("Right scroll", rhs.scroll),
                 keyParts("Curator key", pkg.curator.hex())},
                [this, id, selected](const auto &answers) {
                  if (answers[0].answer() == "back")
                    reviewIndependentLinks(id, selected);
                });
    } catch (const std::exception &error) {
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Endpoint inspection unavailable", error.what());
    }
  });
}
} // namespace xanadu
