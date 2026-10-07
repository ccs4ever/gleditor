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
    field.options.push_back(rangeOf(span) + " — " + span.scroll.substr(0, 17) +
                            "…");
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
          {actions({"Prepare for review", "Review retained package", "Close"},
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
      if (query == "cached") {
        const auto retained = session.linkPackageExchange().statuses();
        auto packages       = choice("Retained package", {}, {});
        if (!retained.empty()) {
          packages.options.clear();
          packages.optionValues.clear();
        }
        for (const auto &pkg : retained) {
          packages.options.push_back(
              pkg.package.title + " — " +
              std::string(linkPackagePhaseName(pkg.phase)));
          packages.optionValues.push_back(pkg.id);
        }
        form.open(
            "Retained link packages",
            "Signed endpoints are retained privately for review.",
            {actions({"Show selected package", "Close"}, {"show", "close"}),
             std::move(packages)},
            [this](const auto &answers) {
              if (answers[0].answer() == "show" && !answers[1].answer().empty())
                linkPackageStatus(answers[1].answer());
            });
        return;
      }
      if (query.empty()) {
        const auto pubs = session.packagePublicationSources();
        form.open(
            "Links and responses",
            "Search the source's registered global scrolls. No curator key is "
            "required.",
            {actions({"Find packages", "Review retained packages", "Close"},
                     {"find", "cached", "close"}),
             sources(pubs)},
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
                        "Publish or download the signed work first.");
                  std::vector<std::string> keys;
                  for (const auto &[key, scroll] :
                       pubs.at(std::stoull(answers[1].answer())).scrolls) {
                    (void)scroll;
                    keys.push_back(key);
                  }
                  linksAndResponses(session.publicationDiscovery().submitLinks(
                      std::move(keys)));
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
                    "Retry discovery", "Review retained packages", "Close"},
                   {"refresh", "fetch", "retry", "cached", "close"}),
           std::move(packages)},
          [this, query, entries,
           keys = status.scrollKeys](const auto &answers) {
            renderer->runWithState(
                [this, query, entries, keys, answers](RenderState &) {
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
                      linkPackageStatus(session.linkPackageExchange().fetch(
                          entry.first, entry.second, keys));
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
void Views::linkPackageStatus(const std::string &id) {
  renderer->runWithState([this, id](RenderState &) {
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
      form.open(
          "Link package status", note,
          {std::move(control),
           choice("Package", {pkg.package.title.empty()
                                  ? id
                                  : pkg.package.title + " #" +
                                        std::to_string(pkg.package.sequence)})},
          [this, id](const auto &answers) {
            renderer->runWithState([this, id, answers](RenderState &) {
              try {
                const auto action = answers[0].answer();
                if (action == "close") return;
                if (action == "review") {
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
                linkPackageStatus(id);
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
      if (status.phase != LinkPackagePhase::Ready &&
          status.phase != LinkPackagePhase::Published &&
          (status.received ||
           (status.phase != LinkPackagePhase::Superseded &&
            status.phase != LinkPackagePhase::NeedsVerification)))
        throw std::runtime_error(
            "Wait for package verification before reviewing endpoints.");
      const auto &pkg = status.package;
      reviewLinkPackage(pkg);
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
      auto curator =
          choice("Curator key", {pkg.curator.hex().substr(0, 12) + "…" +
                                 pkg.curator.hex().substr(52)});
      curator.optionDescriptions = {pkg.curator.hex()};
      form.open(
          "Review independent links",
          pkg.title + " — signature verified; curator enrollment unchecked.",
          {actions({"Inspect selected link", "Open related publication",
                    "Back to status", "Close",
                    "Inspect full scroll identities"},
                   {"inspect", "open", "back", "close", "keys"}),
           std::move(curator), std::move(links),
           endset("Left endset", pkg.links[index].left),
           endset("Right endset", pkg.links[index].right), std::move(pins)},
          [this, id, index, pins = pkg.publications](const auto &answers) {
            renderer->runWithState([this, id, index, pins,
                                    answers](RenderState &) {
              try {
                const auto action = answers[0].answer();
                if (action == "close") return;
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
