#include "ops.hpp"

#include <algorithm>
#include <string>

namespace xudu {

const char *opKindName(const OpKind kind) {
  switch (kind) {
  case OpKind::Insert:
    return "insert";
  case OpKind::Delete:
    return "delete";
  case OpKind::Rearrange:
    return "rearrange";
  case OpKind::Transclude:
    return "transclude";
  case OpKind::Link:
    return "link";
  }
  return "insert";
}

const char *linkTypeName(const LinkType type) {
  switch (type) {
  case LinkType::Comment:
    return "comment";
  case LinkType::Illustration:
    return "illustration";
  case LinkType::Disagreement:
    return "disagreement";
  case LinkType::Authorship:
    return "authorship";
  case LinkType::Quotation:
    return "quotation";
  case LinkType::Other:
    return "other";
  }
  return "other";
}

LinkType linkTypeFromName(const std::string &name) {
  // Unrecognised types become Other rather than an error: a store written by
  // something that knows a type this build does not is still readable, and
  // losing the name of a link is better than losing the link.
  if ("comment" == name) {
    return LinkType::Comment;
  }
  if ("illustration" == name) {
    return LinkType::Illustration;
  }
  if ("disagreement" == name) {
    return LinkType::Disagreement;
  }
  if ("authorship" == name) {
    return LinkType::Authorship;
  }
  if ("quotation" == name) {
    return LinkType::Quotation;
  }
  return LinkType::Other;
}

bool Link::touches(const PrimediaSpan &span) const {
  const auto meets = [&span](const PrimediaSpan &end) {
    return !end.intersect(span).empty();
  };
  return std::ranges::any_of(left, meets) || std::ranges::any_of(right, meets);
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:
