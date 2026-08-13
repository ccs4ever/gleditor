/**
 * @file yaml.hpp
 * @brief The small corner of YAML this program writes, and reads back.
 *
 * Two files here are YAML rather than the bencode everything else uses, and
 * for the same reason: both are meant to be read and edited by a person. The
 * authorship record is what somebody checks before believing a document, and
 * the configuration file is what they set their name in. Neither is a
 * transport format, and neither needs a parser that can do anchors, block
 * scalars or flow mappings.
 *
 * So this is deliberately not a YAML parser. It reads `key: value` at the top
 * level and `  - value` lists under a key, and it writes exactly that shape --
 * every scalar double-quoted, because a name with a colon in it is ordinary
 * and a rule with no exceptions is one nobody has to remember. Anything more
 * elaborate is not misread: it is refused, or ignored, and never guessed at.
 *
 * It is shared rather than written twice because the alternative was two
 * readers that had to agree about quoting, and a disagreement between them
 * would show up as somebody's name coming back wrong in a signed document.
 */
#ifndef XUDU_YAML_H
#define XUDU_YAML_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xudu::yaml {

/// A double-quoted scalar, with the characters that would end it escaped.
[[nodiscard]] std::string quote(std::string_view text);

/// The inverse. Text that is not quoted comes back as it was, since a hand
/// edit that dropped the quotes still means what it says.
[[nodiscard]] std::string unquote(std::string_view text);

/// One line that carried something.
struct Entry {
  /// The key, or the key the list this item belongs to was introduced by.
  std::string key;
  std::string value;
  /// Whether this was a `- item` line rather than a `key: value` one.
  bool listItem{};
};

/**
 * @brief Read the entries out of @p text.
 *
 * Comments and blank lines are skipped. A line that is neither a comment, a
 * list item, nor `key: value` ends the read: the file is not of this shape,
 * and half-understanding it would be worse than not reading it.
 *
 * @return Nothing when the text is not of that shape. An empty list when it
 *         had nothing in it, which is a different answer.
 */
[[nodiscard]] std::optional<std::vector<Entry>> read(std::string_view text);

/// Append `key: "value"`, or nothing at all when the value is empty: a record
/// should say only what somebody meant to say.
void write(std::string &out, std::string_view key, std::string_view value);

/// Append `key:` and then one `  - "value"` per item. Nothing when empty.
void writeList(std::string &out, std::string_view key,
               const std::vector<std::string> &values);

} // namespace xudu::yaml

#endif // XUDU_YAML_H
// vi: set sw=2 sts=2 ts=2 et:
