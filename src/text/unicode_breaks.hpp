#pragma once

#include <cstdint>
#include <graphemebreak.h>
#include <hb.h>
#include <linebreak.h>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

namespace gleditor::text::detail {

inline void initializeUnicodeBreaks() {
  // Pagination workers and UI fitting share libunibreak's global tables.
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    init_linebreak();
    init_graphemebreak();
  });
}

struct UnicodeCharacter {
  std::uint32_t value{};
  std::size_t byte{};
};

template <typename Character>
std::vector<Character> decodeCharacters(const std::string_view text) {
  std::vector<Character> out;
  for (std::size_t i = 0; i < text.size();) {
    const auto start      = i;
    const auto first      = static_cast<unsigned char>(text[i++]);
    std::uint32_t value   = first;
    std::size_t remaining = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      value     = first & 0x1FU;
      remaining = 1;
    } else if (first >= 0xE0 && first <= 0xEF) {
      value     = first & 0x0FU;
      remaining = 2;
    } else if (first >= 0xF0 && first <= 0xF4) {
      value     = first & 0x07U;
      remaining = 3;
    } else if (first >= 0x80) {
      value = 0xFFFDU;
    }
    if (remaining != 0) {
      const auto length = remaining + 1;
      bool valid        = i + remaining <= text.size();
      for (std::size_t j = 0; valid && j < remaining; j++) {
        const auto next = static_cast<unsigned char>(text[i + j]);
        valid           = (next & 0xC0U) == 0x80U;
        value           = (value << 6U) | (next & 0x3FU);
      }
      valid = valid && value <= 0x10FFFFU &&
              !(value >= 0xD800U && value <= 0xDFFFU) &&
              (length != 2 || value >= 0x80U) &&
              (length != 3 || value >= 0x800U) &&
              (length != 4 || value >= 0x10000U);
      if (valid) {
        i += remaining;
      } else {
        value = 0xFFFDU;
      }
    }
    out.push_back(Character{.value = value, .byte = start});
  }
  return out;
}

inline bool letter(const hb_unicode_general_category_t category) {
  return category == HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER ||
         category == HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER ||
         category == HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER ||
         category == HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER ||
         category == HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER;
}

template <typename Characters>
void retainConjuncts(const Characters &characters, std::span<char> graphemes) {
  auto *unicode = hb_unicode_funcs_get_default();
  // Older libunibreak releases lack UAX #29's Indic conjunct rule GB9c.
  // Conservatively keeping same-script virama-linked letters together also
  // avoids cutting a conjunct on systems with older Unicode tables.
  for (std::size_t i = 1; i < characters.size(); i++) {
    const auto value = characters[i].value;
    if ((characters[i].byte != 0 &&
         graphemes[characters[i].byte - 1] != GRAPHEMEBREAK_BREAK) ||
        !letter(hb_unicode_general_category(unicode, value))) {
      continue;
    }
    bool linker   = false;
    auto previous = i;
    while (previous > 0) {
      const auto candidate = characters[--previous].value;
      const auto category  = hb_unicode_general_category(unicode, candidate);
      linker = linker || hb_unicode_combining_class(unicode, candidate) ==
                             HB_UNICODE_COMBINING_CLASS_VIRAMA;
      if (category == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK ||
          category == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK ||
          category == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK ||
          candidate == 0x200DU) {
        continue;
      }
      if (linker && letter(category) &&
          hb_unicode_script(unicode, candidate) ==
              hb_unicode_script(unicode, value)) {
        graphemes[characters[i].byte - 1] = GRAPHEMEBREAK_NOBREAK;
      }
      break;
    }
  }
}

} // namespace gleditor::text::detail
