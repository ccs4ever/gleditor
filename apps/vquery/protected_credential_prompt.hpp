#ifndef VQUERY_PROTECTED_CREDENTIAL_PROMPT_HPP
#define VQUERY_PROTECTED_CREDENTIAL_PROMPT_HPP

#include <iosfwd>
#include <string_view>

namespace xanadu::evidence {
class EvidenceController;
}

namespace vquery {

[[nodiscard]] bool isMailCredentialCommand(std::string_view line);
void handleMailCredentialCommand(xanadu::evidence::EvidenceController &,
                                 std::string_view line, std::ostream &output,
                                 std::ostream &errors);

} // namespace vquery

#endif
