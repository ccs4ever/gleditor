/**
 * @file mail_draft.cpp
 * @brief GMime-backed MIME draft serialization behind the GLEDITOR_MAIL
 *        probe. One TU, two halves (the pg_backend pattern):
 *        GLEDITOR_HAVE_GMIME compiles the real serializer; without it
 *        buildMimeDraft throws the same std::invalid_argument the ops layer
 *        already surfaces for validation failures.
 */

#include "common/xanadu/evidence/mail_transport.hpp"

#ifdef GLEDITOR_HAVE_GMIME

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gmime/gmime.h>

namespace xanadu::evidence {
namespace {

// These bounds cap input memory before GMime allocates encoded MIME framing.
constexpr std::size_t maxDraftFieldBytes = 1024 * 1024;
constexpr std::size_t maxDraftBytes      = 32 * 1024 * 1024;
constexpr std::size_t maxRecipients      = 1024;
constexpr std::size_t maxReferences      = 256;
constexpr std::size_t maxAttachments     = 256;

template <typename T>
using GRef  = std::unique_ptr<T, decltype(&g_object_unref)>;
using GChar = std::unique_ptr<char, decltype(&g_free)>;

void initializeGMime() {
  static std::once_flag once;
  std::call_once(once, [] { g_mime_init(); });
}

bool hasHeaderControl(std::string_view value) {
  return std::ranges::any_of(value, [](const unsigned char ch) {
    return ch == '\r' || ch == '\n' || ch == '\0' || (ch < 0x20 && ch != '\t');
  });
}

void validateField(std::string_view value, std::string_view name) {
  if (value.size() > maxDraftFieldBytes || hasHeaderControl(value))
    throw std::invalid_argument(std::string(name) +
                                " is oversized or contains header controls");
}

void validateBody(std::string_view value, std::string_view name) {
  if (value.size() > maxDraftFieldBytes ||
      std::ranges::any_of(value,
                          [](const unsigned char ch) {
                            return ch == '\0' || (ch < 0x20 && ch != '\r' &&
                                                  ch != '\n' && ch != '\t');
                          }) ||
      !g_utf8_validate(value.data(), static_cast<gssize>(value.size()),
                       nullptr))
    throw std::invalid_argument(std::string(name) +
                                " is oversized or contains invalid body data");
}

struct ParsedMailbox {
  std::string name;
  std::string address;
};

ParsedMailbox parseMailbox(std::string_view input, std::string_view field) {
  validateField(input, field);
  if (input.empty())
    throw std::invalid_argument(std::string(field) + " is empty");
  const std::string owned(input);
  GRef<InternetAddressList> addresses(
      internet_address_list_parse(nullptr, owned.c_str()), g_object_unref);
  if (!addresses || internet_address_list_length(addresses.get()) != 1)
    throw std::invalid_argument(std::string(field) +
                                " must contain exactly one mailbox");
  auto *address = internet_address_list_get_address(addresses.get(), 0);
  if (!address || !INTERNET_ADDRESS_IS_MAILBOX(address))
    throw std::invalid_argument(std::string(field) +
                                " must be a mailbox, not a group");
  const auto *email =
      internet_address_mailbox_get_addr(INTERNET_ADDRESS_MAILBOX(address));
  if (!email || *email == '\0')
    throw std::invalid_argument(std::string(field) + " has no mailbox address");
  const std::string mailbox(email);
  if (std::ranges::any_of(mailbox, [](const unsigned char ch) {
        return ch <= 0x20 || ch >= 0x7f || ch == '<' || ch == '>';
      }))
    throw std::invalid_argument(std::string(field) +
                                " uses an unsupported mailbox encoding");
  const auto *displayName = internet_address_get_name(address);
  return {displayName ? displayName : "", mailbox};
}

void addTextPart(GMimeMultipart *multipart, std::string_view subtype,
                 std::string_view text) {
  const std::string owned(text);
  GRef<GMimeTextPart> part(
      g_mime_text_part_new_with_subtype(std::string(subtype).c_str()),
      g_object_unref);
  if (!part) throw std::runtime_error("GMime could not allocate a text part");
  g_mime_text_part_set_charset(part.get(), "UTF-8");
  g_mime_text_part_set_text(part.get(), owned.c_str());
  g_mime_multipart_add(multipart, GMIME_OBJECT(part.get()));
}

GRef<GMimeObject> textBody(const MailDraftInput &draft) {
  if (!draft.htmlBody) {
    GRef<GMimeTextPart> plain(g_mime_text_part_new_with_subtype("plain"),
                              g_object_unref);
    if (!plain)
      throw std::runtime_error("GMime could not allocate a text part");
    g_mime_text_part_set_charset(plain.get(), "UTF-8");
    g_mime_text_part_set_text(plain.get(), draft.textBody.c_str());
    return GRef<GMimeObject>(GMIME_OBJECT(plain.release()), g_object_unref);
  }

  GRef<GMimeMultipart> alternative(
      g_mime_multipart_new_with_subtype("alternative"), g_object_unref);
  if (!alternative)
    throw std::runtime_error("GMime could not allocate alternative parts");
  addTextPart(alternative.get(), "plain", draft.textBody);
  addTextPart(alternative.get(), "html", *draft.htmlBody);
  return GRef<GMimeObject>(GMIME_OBJECT(alternative.release()), g_object_unref);
}

GRef<GMimePart> attachment(const MailDraftAttachment &attachment) {
  validateField(attachment.filename, "attachment filename");
  validateField(attachment.contentType, "attachment content type");
  if (attachment.filename.empty() || attachment.filename == "." ||
      attachment.filename == ".." ||
      attachment.filename.find_first_of("/\\:") != std::string::npos)
    throw std::invalid_argument("attachment filename must be a basename");
  if (attachment.contentType.empty())
    throw std::invalid_argument("attachment content type is empty");
  auto *parsedType =
      g_mime_content_type_parse(nullptr, attachment.contentType.c_str());
  GRef<GMimeContentType> contentType(parsedType, g_object_unref);
  if (!contentType)
    throw std::invalid_argument("attachment content type is invalid");
  const auto *type = g_mime_content_type_get_media_type(contentType.get());
  const auto *subtype =
      g_mime_content_type_get_media_subtype(contentType.get());
  if (!type || !subtype)
    throw std::invalid_argument("attachment content type is invalid");

  GRef<GMimePart> part(g_mime_part_new_with_type(type, subtype),
                       g_object_unref);
  GRef<GMimeStream> stream(
      g_mime_stream_mem_new_with_buffer(attachment.bytes.data(),
                                        attachment.bytes.size()),
      g_object_unref);
  GRef<GMimeDataWrapper> wrapper(
      g_mime_data_wrapper_new_with_stream(stream.get(),
                                          GMIME_CONTENT_ENCODING_DEFAULT),
      g_object_unref);
  if (!part || !stream || !wrapper)
    throw std::runtime_error("GMime could not allocate an attachment");
  g_mime_object_set_content_type(GMIME_OBJECT(part.get()), contentType.get());
  g_mime_part_set_content(part.get(), wrapper.get());
  g_mime_part_set_content_encoding(part.get(), GMIME_CONTENT_ENCODING_BASE64);
  g_mime_part_set_filename(part.get(), attachment.filename.c_str());
  g_mime_object_set_disposition(GMIME_OBJECT(part.get()),
                                GMIME_DISPOSITION_ATTACHMENT);
  return part;
}

bool validCorrelation(std::string_view value) {
  if (value.empty() || value.size() > 128) return false;
  return std::ranges::all_of(value, [](const unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
  });
}

} // namespace

SerializedMailDraft buildMimeDraft(const MailDraftInput &draft,
                                   std::string correlationId) {
  initializeGMime();
  if (!validCorrelation(correlationId))
    throw std::invalid_argument("mail draft correlation ID is invalid");
  validateField(draft.subject, "subject");
  validateBody(draft.textBody, "text body");
  if (draft.htmlBody) validateBody(*draft.htmlBody, "HTML body");
  if (draft.recipients.empty() || draft.recipients.size() > maxRecipients ||
      draft.references.size() > maxReferences ||
      draft.attachments.size() > maxAttachments)
    throw std::invalid_argument("mail draft occurrence limit exceeded");
  if (draft.textBody.empty() && (!draft.htmlBody || draft.htmlBody->empty()))
    throw std::invalid_argument("mail draft requires a nonempty body");
  if (draft.inReplyTo) validateField(*draft.inReplyTo, "In-Reply-To");
  for (const auto &reference : draft.references)
    validateField(reference, "References");
  std::size_t sourceBytes{};
  const auto accountBytes = [&sourceBytes](const std::size_t bytes) {
    if (bytes > maxDraftBytes - std::min(sourceBytes, maxDraftBytes))
      throw std::invalid_argument("mail draft exceeds its source byte budget");
    sourceBytes += bytes;
  };
  accountBytes(draft.from.size());
  accountBytes(draft.subject.size());
  accountBytes(correlationId.size());
  accountBytes(draft.textBody.size());
  if (draft.htmlBody) accountBytes(draft.htmlBody->size());
  for (const auto &recipient : draft.recipients) {
    accountBytes(recipient.role.size());
    accountBytes(recipient.address.size());
  }
  if (draft.inReplyTo) accountBytes(draft.inReplyTo->size());
  for (const auto &reference : draft.references) accountBytes(reference.size());
  for (const auto &source : draft.attachments) {
    accountBytes(source.filename.size());
    accountBytes(source.contentType.size());
    accountBytes(source.bytes.size());
  }

  const auto from = parseMailbox(draft.from, "From");
  SerializedMailDraft serialized;
  serialized.envelopeFrom  = from.address;
  serialized.correlationId = correlationId;

  GRef<GMimeMessage> message(g_mime_message_new(true), g_object_unref);
  if (!message) throw std::runtime_error("GMime could not allocate a message");
  g_mime_message_add_mailbox(message.get(), GMIME_ADDRESS_TYPE_FROM,
                             from.name.c_str(), from.address.c_str());
  for (const auto &recipient : draft.recipients) {
    auto mailbox = parseMailbox(recipient.address, "recipient");
    serialized.envelopeRecipients.push_back(mailbox.address);
    if (recipient.role == "to")
      g_mime_message_add_mailbox(message.get(), GMIME_ADDRESS_TYPE_TO,
                                 mailbox.name.c_str(), mailbox.address.c_str());
    else if (recipient.role == "cc")
      g_mime_message_add_mailbox(message.get(), GMIME_ADDRESS_TYPE_CC,
                                 mailbox.name.c_str(), mailbox.address.c_str());
    else if (recipient.role != "bcc")
      throw std::invalid_argument("recipient role must be to, cc, or bcc");
  }
  if (serialized.envelopeRecipients.empty())
    throw std::invalid_argument("mail draft requires an envelope recipient");

  g_mime_message_set_subject(message.get(), draft.subject.c_str(), "UTF-8");
  GChar messageId(g_mime_utils_generate_message_id("localhost"), g_free);
  if (!messageId)
    throw std::runtime_error("GMime could not generate a message ID");
  serialized.messageId = messageId.get();
  g_mime_message_set_message_id(message.get(), serialized.messageId.c_str());
  if (draft.inReplyTo)
    g_mime_object_set_header(GMIME_OBJECT(message.get()), "In-Reply-To",
                             draft.inReplyTo->c_str(), "UTF-8");
  if (!draft.references.empty()) {
    std::string references;
    for (const auto &reference : draft.references) {
      if (!references.empty()) references.push_back(' ');
      references += reference;
    }
    g_mime_object_set_header(GMIME_OBJECT(message.get()), "References",
                             references.c_str(), "UTF-8");
  }

  auto body = textBody(draft);
  if (draft.attachments.empty()) {
    g_mime_message_set_mime_part(message.get(), body.get());
  } else {
    GRef<GMimeMultipart> mixed(g_mime_multipart_new_with_subtype("mixed"),
                               g_object_unref);
    if (!mixed)
      throw std::runtime_error("GMime could not allocate attachments");
    g_mime_multipart_add(mixed.get(), body.get());
    for (const auto &source : draft.attachments) {
      auto part = attachment(source);
      g_mime_multipart_add(mixed.get(), GMIME_OBJECT(part.get()));
    }
    g_mime_message_set_mime_part(message.get(), GMIME_OBJECT(mixed.get()));
  }

  std::unique_ptr<GMimeFormatOptions, decltype(&g_mime_format_options_free)>
      format(g_mime_format_options_new(), g_mime_format_options_free);
  GRef<GMimeStream> output(g_mime_stream_mem_new(), g_object_unref);
  if (!format || !output)
    throw std::runtime_error("GMime could not allocate a serialization stream");
  g_mime_format_options_set_newline_format(format.get(),
                                           GMIME_NEWLINE_FORMAT_DOS);
  if (g_mime_object_write_to_stream(GMIME_OBJECT(message.get()), format.get(),
                                    output.get()) < 0 ||
      g_mime_stream_flush(output.get()) < 0)
    throw std::runtime_error("GMime failed to serialize the mail draft");
  auto *bytes =
      g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(output.get()));
  if (!bytes || bytes->len == 0 || bytes->len > maxDraftBytes)
    throw std::invalid_argument(
        "serialized mail draft exceeds its byte budget");
  serialized.bytes.assign(reinterpret_cast<const char *>(bytes->data),
                          bytes->len);
  return serialized;
}

} // namespace xanadu::evidence

#else // !GLEDITOR_HAVE_GMIME -- the refusing stub

#include <stdexcept>
#include <string>

namespace xanadu::evidence {

SerializedMailDraft buildMimeDraft(const MailDraftInput & /*draft*/,
                                   const std::string /*correlationId*/) {
  throw std::invalid_argument(
      "mail-draft: this build has no GMime 3 -- rebuild with "
      "GLEDITOR_MAIL=1 (the Makefile probes pkg-config gmime-3.0)");
}

} // namespace xanadu::evidence

#endif // GLEDITOR_HAVE_GMIME
