#ifndef COMMON_UI_IDENTITY_FIELDS_HPP
#define COMMON_UI_IDENTITY_FIELDS_HPP

#include <algorithm>
#include <gleditor/form.hpp>
#include <string>
#include <utility>
#include <vector>

namespace xanadu {
inline gleditor::Form::Field keyParts(const std::string &label,
                                      const std::string &key) {
  // Preserve the existing numbered identity format in compact form fields.
  constexpr std::size_t partCharacters = 32;
  std::vector<std::string> parts;
  for (std::size_t start = 0; start < key.size();) {
    auto end = std::min(start + partCharacters, key.size());
    while (end > start && end < key.size() &&
           (static_cast<unsigned char>(key[end]) & 0xc0) == 0x80)
      --end;
    if (end == start) ++end;
    parts.push_back(std::to_string(parts.size() + 1) + ": " +
                    key.substr(start, end - start));
    start = end;
  }
  gleditor::Form::Field field{.label   = label,
                              .kind    = gleditor::Form::Kind::Choice,
                              .options = std::move(parts)};
  field.optionDescriptions.assign(field.options.size(), key);
  return field;
}

} // namespace xanadu
#endif
