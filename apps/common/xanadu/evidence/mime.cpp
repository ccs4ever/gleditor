#include "common/xanadu/evidence/mime.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <gmime/gmime.h>

namespace xanadu::evidence {
namespace {

template <typename T> using GObjectPtr = std::unique_ptr<T, void (*)(gpointer)>;

template <typename T> GObjectPtr<T> own(T *value) {
  return {value, [](gpointer pointer) { g_object_unref(pointer); }};
}

std::string copy(const char *value) { return value ? value : ""; }

std::string contentType(GMimeObject *object) {
  auto *type = g_mime_object_get_content_type(object);
  if (!type) return "application/octet-stream";
  char *value        = g_mime_content_type_get_mime_type(type);
  std::string result = copy(value);
  g_free(value);
  return result;
}

std::string decode(GMimeDataWrapper *wrapper, std::size_t remaining) {
  if (!wrapper) return {};
  auto stream = own(g_mime_stream_mem_new());
  const auto written =
      g_mime_data_wrapper_write_to_stream(wrapper, stream.get());
  if (written < 0 || static_cast<std::size_t>(written) > remaining)
    throw std::length_error(
        "MIME decoded content exceeds configured byte limit");
  auto *bytes =
      g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(stream.get()));
  return {reinterpret_cast<const char *>(bytes->data), bytes->len};
}

void appendHeaders(GMimeHeaderList *list,
                   std::vector<MailHeaderOccurrence> &headers,
                   const MimeParseLimits &limits, std::size_t &total) {
  if (!list) return;
  const int count = g_mime_header_list_get_count(list);
  if (count < 0 || total > limits.maxHeaders ||
      static_cast<std::size_t>(count) > limits.maxHeaders - total)
    throw std::length_error("MIME header count exceeds configured limit");
  for (int i = 0; i < count; ++i) {
    auto *header = g_mime_header_list_get_header_at(list, i);
    if (header) {
      headers.push_back({copy(g_mime_header_get_raw_name(header)),
                         copy(g_mime_header_get_value(header)),
                         copy(g_mime_header_get_raw_value(header))});
      ++total;
    }
  }
}

void appendRecipients(InternetAddressList *list, const char *role,
                      std::vector<MailRecipientOccurrence> &recipients,
                      const MimeParseLimits &limits) {
  if (!list) return;
  for (int i = 0; i < internet_address_list_length(list); ++i) {
    auto *address = internet_address_list_get_address(list, i);
    if (INTERNET_ADDRESS_IS_MAILBOX(address)) {
      recipients.push_back({role, copy(internet_address_mailbox_get_addr(
                                      INTERNET_ADDRESS_MAILBOX(address)))});
    } else if (INTERNET_ADDRESS_IS_GROUP(address)) {
      appendRecipients(
          internet_address_group_get_members(INTERNET_ADDRESS_GROUP(address)),
          role, recipients, limits);
    }
    if (recipients.size() > limits.maxRecipients)
      throw std::length_error("MIME recipient count exceeds configured limit");
  }
}

struct TreeWalker {
  const MimeParseLimits &limits;
  std::size_t decodedBytes{};
  std::size_t convertedTextBytes{};
  std::size_t headerCount{};
  std::vector<MailMimePart> &parts;

  void visit(GMimeObject *object, const std::string &parent,
             std::size_t depth) {
    if (!object) return;
    if (depth > limits.maxDepth)
      throw std::length_error("MIME nesting exceeds configured depth limit");
    if (parts.size() >= limits.maxParts)
      throw std::length_error("MIME part count exceeds configured limit");

    const auto key = std::to_string(parts.size() + 1);
    MailMimePart part;
    part.key = key;
    if (!parent.empty()) part.parentKey = parent;
    part.contentType = contentType(object);
    part.disposition = copy(g_mime_object_get_disposition(object));
    part.charset =
        copy(g_mime_object_get_content_type_parameter(object, "charset"));
    part.filename = copy(
        g_mime_object_get_content_disposition_parameter(object, "filename"));
    if (GMIME_IS_PART(object)) {
      auto *mimePart        = GMIME_PART(object);
      part.transferEncoding = copy(g_mime_content_encoding_to_string(
          g_mime_part_get_content_encoding(mimePart)));
      part.contentId        = copy(g_mime_part_get_content_id(mimePart));
      part.contentLocation  = copy(g_mime_part_get_content_location(mimePart));
      part.contentDescription =
          copy(g_mime_part_get_content_description(mimePart));
      auto decoded = decode(g_mime_part_get_content(mimePart),
                            limits.maxDecodedBytes - decodedBytes);
      decodedBytes += decoded.size();
      part.decodedBytes = decoded;
      if (GMIME_IS_TEXT_PART(object)) {
        char *text = g_mime_text_part_get_text(GMIME_TEXT_PART(object));
        if (text) {
          const auto size = std::strlen(text);
          if (size > limits.maxDecodedBytes - convertedTextBytes) {
            g_free(text);
            throw std::length_error(
                "MIME converted text exceeds configured byte limit");
          }
          convertedTextBytes += size;
          part.decodedText             = text;
          part.charsetConversionStatus = "available";
          g_free(text);
        } else {
          part.charsetConversionStatus = "unavailable";
        }
      }
    }
    appendHeaders(g_mime_object_get_header_list(object), part.headers, limits,
                  headerCount);
    if (GMIME_IS_MESSAGE_PART(object)) {
      auto *embedded =
          g_mime_message_part_get_message(GMIME_MESSAGE_PART(object));
      if (embedded)
        appendHeaders(g_mime_object_get_header_list(GMIME_OBJECT(embedded)),
                      part.embeddedMessageHeaders, limits, headerCount);
    }
    parts.push_back(std::move(part));

    if (GMIME_IS_MULTIPART(object)) {
      auto *multipart = GMIME_MULTIPART(object);
      const int count = g_mime_multipart_get_count(multipart);
      for (int i = 0; i < count; ++i)
        visit(g_mime_multipart_get_part(multipart, i), key, depth + 1);
    } else if (GMIME_IS_MESSAGE_PART(object)) {
      auto *message =
          g_mime_message_part_get_message(GMIME_MESSAGE_PART(object));
      if (message) visit(g_mime_message_get_mime_part(message), key, depth + 1);
    }
  }
};

} // namespace

MailMessageEvidence parseMimeMessage(std::string_view rawMessage,
                                     std::uint64_t uidValidity,
                                     std::uint32_t uid,
                                     std::uint32_t sequenceNumber,
                                     const MimeParseLimits &limits) {
  if (rawMessage.size() > limits.maxMessageBytes)
    throw std::length_error("MIME message exceeds configured byte limit");
  static std::once_flag initialized;
  std::call_once(initialized, [] { g_mime_init(); });

  auto stream = own(
      g_mime_stream_mem_new_with_buffer(rawMessage.data(), rawMessage.size()));
  auto parser = own(g_mime_parser_new_with_stream(stream.get()));
  auto options =
      std::unique_ptr<GMimeParserOptions, void (*)(GMimeParserOptions *)>(
          g_mime_parser_options_new(), g_mime_parser_options_free);
  auto message =
      own(g_mime_parser_construct_message(parser.get(), options.get()));
  if (!message) throw std::runtime_error("GMime could not parse the message");

  MailMessageEvidence result;
  result.uidValidity    = uidValidity;
  result.uid            = uid;
  result.sequenceNumber = sequenceNumber;
  result.rawMessage.assign(rawMessage.data(), rawMessage.size());
  std::size_t headerCount = 0;
  appendHeaders(g_mime_object_get_header_list(GMIME_OBJECT(message.get())),
                result.headers, limits, headerCount);
  result.messageId = copy(g_mime_message_get_message_id(message.get()));
  result.subject   = copy(g_mime_message_get_subject(message.get()));
  appendRecipients(g_mime_message_get_from(message.get()), "from",
                   result.recipients, limits);
  appendRecipients(g_mime_message_get_to(message.get()), "to",
                   result.recipients, limits);
  appendRecipients(g_mime_message_get_cc(message.get()), "cc",
                   result.recipients, limits);
  appendRecipients(g_mime_message_get_bcc(message.get()), "bcc",
                   result.recipients, limits);
  TreeWalker walker{limits, 0, 0, headerCount, result.parts};
  walker.visit(g_mime_message_get_mime_part(message.get()), {}, 0);
  return result;
}

} // namespace xanadu::evidence
