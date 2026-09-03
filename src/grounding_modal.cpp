/**
 * @file grounding_modal.cpp
 * @brief Implementation of GroundingModal for verifying external quotes.
 */
#include <gleditor/grounding_modal.hpp>

#include <filesystem>
#include <utility>

namespace gleditor {

GroundingModal::GroundingModal(std::string fontName)
    : form_(std::move(fontName)) {}

void GroundingModal::openForQuote(std::string quoteSnippet,
                                  std::vector<KnownSource> knownSources,
                                  OnGrounded onGrounded, OnReject onReject) {
  std::vector<Form::Field> fields;

  // Field 0: Drop-down choice of cached/known sources
  Form::Field choiceField;
  choiceField.label        = "Cached Swarm Source";
  choiceField.kind         = Form::Kind::Choice;
  choiceField.required     = false;
  choiceField.options      = {"[Specify Custom File / Magnet Below]"};
  choiceField.optionValues = {""};

  for (const auto &ks : knownSources) {
    std::string optLabel = ks.displayName;
    if (!ks.mimeType.empty()) {
      optLabel += " <" + ks.mimeType + ">";
    }
    choiceField.options.push_back(std::move(optLabel));
    choiceField.optionValues.push_back(ks.pathOrMagnet);
  }
  fields.push_back(std::move(choiceField));

  // Field 1: Custom path or magnet
  Form::Field customField;
  customField.label    = "Custom File Path or Magnet Link";
  customField.kind     = Form::Kind::Text;
  customField.required = false;
  customField.hint     = "/path/to/source.txt or magnet:?xt=urn:btih:...";
  fields.push_back(std::move(customField));

  const std::string title = "Ground External Quotation in Source Work";
  const std::string note =
      "All external quotations require a verified parent work in the Xanadu "
      "universe. "
      "Select a cached source or provide a local path/magnet.";

  form_.open(
      title, note, std::move(fields),
      [quote = std::move(quoteSnippet), onGrounded = std::move(onGrounded),
       onReject](const std::vector<Form::Field> &resFields) {
        if (resFields.size() < 2) {
          if (onReject) {
            onReject("External paste rejected: invalid form response");
          }
          return;
        }

        std::string sourceTarget;
        const auto &cField = resFields[0];
        if (cField.chosen > 0 && cField.chosen < cField.optionValues.size()) {
          sourceTarget = cField.optionValues[cField.chosen];
        } else {
          sourceTarget = resFields[1].answer();
        }

        if (sourceTarget.empty()) {
          if (onReject) {
            onReject(
                "External paste rejected: no parent source work was provided");
          }
          return;
        }

        if (std::filesystem::exists(sourceTarget)) {
          const auto match = SourceGrounder::groundFile(sourceTarget, quote);
          if (!match) {
            if (onReject) {
              onReject("External paste rejected: quote snippet was not found "
                       "in source file");
            }
            return;
          }
          if (onGrounded) {
            onGrounded(*match);
          }
        } else {
          // Magnet URI or swarmed source
          SubspanMatch match{
              .offset             = 0,
              .length             = static_cast<std::uint64_t>(quote.size()),
              .mimeType           = "text/plain",
              .sourcePathOrMagnet = sourceTarget,
              .contextSnippet     = quote,
          };
          if (onGrounded) {
            onGrounded(match);
          }
        }
      },
      [onReject]() {
        if (onReject) {
          onReject("External paste rejected: source media grounding required");
        }
      });
}

void GroundingModal::openForBinary(std::vector<std::uint8_t> excerptBytes,
                                   std::vector<KnownSource> knownSources,
                                   OnGrounded onGrounded, OnReject onReject) {
  std::vector<Form::Field> fields;

  Form::Field choiceField;
  choiceField.label        = "Cached Media Source";
  choiceField.kind         = Form::Kind::Choice;
  choiceField.required     = false;
  choiceField.options      = {"[Specify Custom File / Magnet Below]"};
  choiceField.optionValues = {""};

  for (const auto &ks : knownSources) {
    std::string optLabel = ks.displayName;
    if (!ks.mimeType.empty()) {
      optLabel += " <" + ks.mimeType + ">";
    }
    choiceField.options.push_back(std::move(optLabel));
    choiceField.optionValues.push_back(ks.pathOrMagnet);
  }
  fields.push_back(std::move(choiceField));

  Form::Field customField;
  customField.label    = "Custom Media File Path or Magnet Link";
  customField.kind     = Form::Kind::Text;
  customField.required = false;
  customField.hint     = "/path/to/recording.flac or magnet:?xt=urn:btih:...";
  fields.push_back(std::move(customField));

  const std::string title = "Ground External Media Clip in Source Media";
  const std::string note  = "Select the parent media recording/asset from the "
                            "swarm or specify a local file.";

  form_.open(
      title, note, std::move(fields),
      [excerpt = std::move(excerptBytes), onGrounded = std::move(onGrounded),
       onReject](const std::vector<Form::Field> &resFields) {
        if (resFields.size() < 2) {
          if (onReject) {
            onReject("External media import rejected: invalid form response");
          }
          return;
        }

        std::string sourceTarget;
        const auto &cField = resFields[0];
        if (cField.chosen > 0 && cField.chosen < cField.optionValues.size()) {
          sourceTarget = cField.optionValues[cField.chosen];
        } else {
          sourceTarget = resFields[1].answer();
        }

        if (sourceTarget.empty()) {
          if (onReject) {
            onReject("External media import rejected: no parent source work "
                     "was provided");
          }
          return;
        }

        if (std::filesystem::exists(sourceTarget)) {
          const auto match =
              SourceGrounder::groundFileBinary(sourceTarget, excerpt);
          if (!match) {
            if (onReject) {
              onReject("External media import rejected: clip was not found in "
                       "source file");
            }
            return;
          }
          if (onGrounded) {
            onGrounded(*match);
          }
        } else {
          SubspanMatch match{
              .offset             = 0,
              .length             = static_cast<std::uint64_t>(excerpt.size()),
              .mimeType           = "application/octet-stream",
              .sourcePathOrMagnet = sourceTarget,
              .contextSnippet     = "",
          };
          if (onGrounded) {
            onGrounded(match);
          }
        }
      },
      [onReject]() {
        if (onReject) {
          onReject("External media import rejected: source media grounding "
                   "required");
        }
      });
}

} // namespace gleditor
