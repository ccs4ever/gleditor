#ifndef XANADU_EVIDENCE_MIME_HPP
#define XANADU_EVIDENCE_MIME_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/xanadu/evidence/communications.hpp"

namespace xanadu::evidence {

struct MimeParseLimits {
  std::size_t maxMessageBytes{8 * 1024 * 1024};
  std::size_t maxParts{256};
  std::size_t maxDepth{24};
  std::size_t maxDecodedBytes{8 * 1024 * 1024};
  std::size_t maxHeaders{4096};
  std::size_t maxRecipients{1024};
};

[[nodiscard]] MailMessageEvidence
parseMimeMessage(std::string_view rawMessage, std::uint64_t uidValidity,
                 std::uint32_t uid, std::uint32_t sequenceNumber,
                 const MimeParseLimits &limits = {});

} // namespace xanadu::evidence

#endif
