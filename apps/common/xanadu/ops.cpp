#include "ops.hpp"

#include <algorithm>
#include <string>

namespace xanadu {

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
  case OpKind::PageBreak:
    return "pagebreak";
  case OpKind::Structure:
    return "structure";
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
  case LinkType::Format:
    return "format";
  case LinkType::Dimension:
    return "dimension";
  }
  return "other";
}

LinkType linkTypeFromName(const std::string &name) {
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
  if ("format" == name) {
    return LinkType::Format;
  }
  if ("dimension" == name) {
    return LinkType::Dimension;
  }
  return LinkType::Other;
}

const char *prominenceTierName(const ProminenceTier tier) {
  switch (tier) {
  case ProminenceTier::Author:
    return "author";
  case ProminenceTier::Curated:
    return "curated";
  case ProminenceTier::Public:
    return "public";
  }
  return "author";
}

ProminenceTier prominenceTierFromName(const std::string &name) {
  if ("curated" == name) {
    return ProminenceTier::Curated;
  }
  if ("public" == name) {
    return ProminenceTier::Public;
  }
  // Author is the default a Link is constructed with, so it is also what an
  // unrecognised tier reads as: a link whose prominence cannot be established
  // is the reader's own, not somebody else's curation.
  return ProminenceTier::Author;
}

bool Link::touches(const PrimediaSpan &span) const {
  const auto meets = [&span](const PrimediaSpan &end) {
    return !end.intersect(span).empty();
  };
  return std::ranges::any_of(left, meets) || std::ranges::any_of(right, meets);
}

} // namespace xanadu
