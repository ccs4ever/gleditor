/**
 * @file link_package.cpp
 * @brief Implementation of standalone, signed link packages.
 */
#include "link_package.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "bencode.hpp"
#include "scroll_codec.hpp"
#include "store.hpp"
#include "swarm.hpp"
#include "torrent.hpp"

namespace xanadu {

namespace {

constexpr auto keyCurator   = "curator";
constexpr auto keyLinks     = "links";
constexpr auto keySalt      = "salt";
constexpr auto keyScrolls   = "scrolls";
constexpr auto keySequence  = "seq";
constexpr auto keySignature = "sig";
constexpr auto keyTime      = "time";
constexpr auto keyTitle     = "title";

std::string rawBytes(const PublicKey &key) {
  return std::string{reinterpret_cast<const char *>(key.bytes.data()),
                     key.bytes.size()};
}

std::string rawBytes(const Signature &sig) {
  return std::string{reinterpret_cast<const char *>(sig.bytes.data()),
                     sig.bytes.size()};
}

bencode::Value encodeSpan(const GlobalSpan &span) {
  return bencode::Value::dict({
      {"len", bencode::Value::integer(static_cast<std::int64_t>(span.length))},
      {"scroll", bencode::Value::string(span.scroll)},
      {"start", bencode::Value::integer(static_cast<std::int64_t>(span.start))},
  });
}

std::optional<GlobalSpan> decodeSpan(const bencode::Value &value) {
  const auto scroll = value.find("scroll");
  const auto start  = value.find("start");
  const auto length = value.find("len");
  if (!scroll || !scroll->isString() || !start || !start->isInteger() ||
      !length || !length->isInteger()) {
    return std::nullopt;
  }
  if (start->asInteger() < 0 || length->asInteger() < 0) {
    return std::nullopt;
  }
  return GlobalSpan{.scroll = scroll->asString(),
                    .start  = static_cast<std::uint64_t>(start->asInteger()),
                    .length = static_cast<std::uint64_t>(length->asInteger())};
}

bencode::Value encodeSpans(const std::vector<GlobalSpan> &spans) {
  bencode::List out;
  out.reserve(spans.size());
  for (const auto &span : spans) {
    out.push_back(encodeSpan(span));
  }
  return bencode::Value::list(std::move(out));
}

std::optional<std::vector<GlobalSpan>>
decodeSpans(const bencode::Value &value) {
  if (!value.isList()) {
    return std::nullopt;
  }
  std::vector<GlobalSpan> out;
  out.reserve(value.asList().size());
  for (const auto &item : value.asList()) {
    auto span = decodeSpan(item);
    if (!span) {
      return std::nullopt;
    }
    out.push_back(*span);
  }
  return out;
}

bencode::Value encodeScroll(const Scroll &scroll) {
  bencode::List segmentList;
  segmentList.reserve(scroll.segments.size());
  for (const auto &segment : scroll.segments) {
    segmentList.push_back(encodeSegment(segment));
  }
  bencode::Dict dict = {
      {"segments", bencode::Value::list(std::move(segmentList))},
  };
  if (scroll.isNamed()) {
    dict.emplace("publisher",
                 bencode::Value::string(rawBytes(scroll.publisher)));
  }
  if (!scroll.salt.empty()) {
    dict.emplace("salt", bencode::Value::string(scroll.salt));
  }
  return bencode::Value::dict(std::move(dict));
}

std::optional<Scroll> decodeScroll(const bencode::Value &value) {
  const auto segments = value.find("segments");
  if (!segments || !segments->isList()) {
    return std::nullopt;
  }
  Scroll scroll;
  const auto publisher = value.find("publisher");
  if (publisher.has_value() && publisher->isString() &&
      32 == publisher->asString().size()) {
    std::copy(publisher->asString().begin(), publisher->asString().end(),
              scroll.publisher.bytes.begin());
  }
  const auto salt = value.find("salt");
  if (salt.has_value() && salt->isString()) {
    scroll.salt = salt->asString();
  }
  for (const auto &item : segments->asList()) {
    auto segment = decodeSegment(item);
    if (!segment) {
      return std::nullopt;
    }
    scroll.segments.push_back(*segment);
  }
  return scroll;
}

bencode::Value encodeLink(const GlobalLink &link) {
  bencode::Dict dict = {
      {"left", encodeSpans(link.left)},
      {"owner", bencode::Value::string(link.owner)},
      {"right", encodeSpans(link.right)},
      {"tier", bencode::Value::string(prominenceTierName(link.tier))},
      {"type", bencode::Value::string(linkTypeName(link.type))},
  };
  if (!link.curator.empty()) {
    dict.emplace("curator", bencode::Value::string(link.curator));
  }
  return bencode::Value::dict(std::move(dict));
}

std::optional<GlobalLink> decodeLink(const bencode::Value &value) {
  const auto type    = value.find("type");
  const auto owner   = value.find("owner");
  const auto left    = value.find("left");
  const auto right   = value.find("right");
  const auto tier    = value.find("tier");
  const auto curator = value.find("curator");
  if (!type || !type->isString() || !owner || !owner->isString() || !left ||
      !right) {
    return std::nullopt;
  }
  auto lefts  = decodeSpans(*left);
  auto rights = decodeSpans(*right);
  if (!lefts || !rights) {
    return std::nullopt;
  }
  GlobalLink link;
  link.type  = linkTypeFromName(type->asString());
  link.owner = owner->asString();
  link.left  = std::move(*lefts);
  link.right = std::move(*rights);
  if (tier.has_value() && tier->isString()) {
    if ("curated" == tier->asString()) {
      link.tier = ProminenceTier::Curated;
    } else if ("public" == tier->asString()) {
      link.tier = ProminenceTier::Public;
    } else {
      link.tier = ProminenceTier::Author;
    }
  } else {
    link.tier = ProminenceTier::Curated;
  }
  if (curator.has_value() && curator->isString()) {
    link.curator = curator->asString();
  }
  return link;
}

bencode::Dict manifestOf(const LinkPackage &pkg, const bool includeSignature) {
  bencode::List linksList;
  linksList.reserve(pkg.links.size());
  for (const auto &link : pkg.links) {
    linksList.push_back(encodeLink(link));
  }

  bencode::Dict scrollsDict;
  for (const auto &[key, scroll] : pkg.scrolls) {
    scrollsDict.emplace(key, encodeScroll(scroll));
  }

  bencode::List publications;
  for (const auto &pin : pkg.publications)
    publications.push_back(bencode::Value::dict(
        {{"publisher", bencode::Value::string(pin.publisher.hex())},
         {"salt", bencode::Value::string(pin.salt)},
         {"hash", bencode::Value::string(pin.hash.hex())},
         {"sequence", bencode::Value::integer(pin.sequence)},
         {"version", bencode::Value::string(pin.version.str())},
         {"title", bencode::Value::string(pin.title)}}));
  bencode::Dict dict = {
      {"format", bencode::Value::integer(1)},
      {"publications", bencode::Value::list(std::move(publications))},
      {keyCurator, bencode::Value::string(rawBytes(pkg.curator))},
      {keyLinks, bencode::Value::list(std::move(linksList))},
      {keySalt, bencode::Value::string(pkg.salt)},
      {keyScrolls, bencode::Value::dict(std::move(scrollsDict))},
      {keySequence, bencode::Value::integer(pkg.sequence)},
      {keyTime,
       bencode::Value::integer(static_cast<std::int64_t>(pkg.published))},
      {keyTitle, bencode::Value::string(pkg.title)},
  };

  if (includeSignature) {
    dict.emplace(keySignature, bencode::Value::string(rawBytes(pkg.signature)));
  }
  return dict;
}

} // namespace

DhtTarget LinkPackage::name() const {
  MutableLink link;
  link.key  = curator;
  link.salt = salt;
  return link.target();
}

std::string LinkPackage::uri() const {
  MutableLink link;
  link.key  = curator;
  link.salt = salt;
  return link.uri();
}

std::string LinkPackage::packageKey() const {
  return scrollKeyFor(curator, salt);
}

std::string LinkPackage::describe() const {
  return std::format("LinkPackage \"{}\" by {}… seq {}, {} links", title,
                     curator.hex().substr(0, 8), sequence, links.size());
}

std::string linkPackageSigningBuffer(const LinkPackage &pkg) {
  return bencode::Value::dict(manifestOf(pkg, false)).encode();
}

std::string encodeLinkPackage(const LinkPackage &pkg) {
  auto bytes = bencode::Value::dict(manifestOf(pkg, true)).encode();
  if (bytes.size() > maximumLinkPackageBytes)
    throw LinkPackageUnreadable("Link package format 1: byte limit exceeded");
  return bytes;
}

bool verifyLinkPackage(const LinkPackage &pkg) {
  return verifyMutableItem(linkPackageSigningBuffer(pkg), pkg.signature,
                           pkg.curator);
}

std::optional<LinkPackage> decodeLinkPackage(const std::string_view encoded) {
  if (encoded.size() > maximumLinkPackageBytes) return std::nullopt;
  bencode::Value root;
  try {
    root = bencode::decode(encoded);
  } catch (const std::exception &) {
    return std::nullopt;
  }
  if (!root.isDict()) {
    return std::nullopt;
  }

  const auto format = root.find("format");
  if (!format || !format->isInteger() || format->asInteger() != 1)
    throw LinkPackageUnreadable("Link package version 1 expected, got " +
                                (format && format->isInteger()
                                     ? std::to_string(format->asInteger())
                                     : "missing"));
  if (root.asDict().size() != 10) return std::nullopt;
  const auto pins = root.find("publications");
  if (!pins || !pins->isList() || pins->asList().size() > 128)
    return std::nullopt;
  const auto curator   = root.find(keyCurator);
  const auto salt      = root.find(keySalt);
  const auto title     = root.find(keyTitle);
  const auto sequence  = root.find(keySequence);
  const auto time      = root.find(keyTime);
  const auto links     = root.find(keyLinks);
  const auto scrolls   = root.find(keyScrolls);
  const auto signature = root.find(keySignature);
  if (!curator || !curator->isString() || curator->asString().size() != 32 ||
      !salt || !salt->isString() || !title || !title->isString() || !sequence ||
      !sequence->isInteger() || !time || !time->isInteger() || !links ||
      !links->isList() || !scrolls || !scrolls->isDict() || !signature ||
      !signature->isString() || signature->asString().size() != 64) {
    return std::nullopt;
  }

  LinkPackage pkg;
  std::copy(curator->asString().begin(), curator->asString().end(),
            pkg.curator.bytes.begin());
  pkg.salt      = salt->asString();
  pkg.title     = title->asString();
  pkg.sequence  = sequence->asInteger();
  pkg.published = static_cast<std::uint64_t>(time->asInteger());
  std::copy(signature->asString().begin(), signature->asString().end(),
            pkg.signature.bytes.begin());

  for (const auto &item : links->asList()) {
    auto link = decodeLink(item);
    if (!link) {
      return std::nullopt;
    }
    pkg.links.push_back(std::move(*link));
  }

  for (const auto &[key, value] : scrolls->asDict()) {
    auto scroll = decodeScroll(value);
    if (!scroll) {
      return std::nullopt;
    }
    pkg.scrolls.emplace(key, std::move(*scroll));
  }

  try {
    for (const auto &value : pins->asList()) {
      if (!value.isDict() || value.asDict().size() != 6) return std::nullopt;
      const auto field = [&](std::string_view name) -> const bencode::Value & {
        return value.asDict().at(std::string(name));
      };
      const auto key  = PublicKey::parseHex(field("publisher").asString());
      const auto hash = InfoHash::parseHex(field("hash").asString());
      if (!key || !hash) return std::nullopt;
      pkg.publications.push_back(
          {*key, field("salt").asString(), *hash, field("sequence").asInteger(),
           MicroversionId::parse(field("version").asString()),
           field("title").asString()});
    }
    if (encodeLinkPackage(pkg) != encoded) return std::nullopt;
  } catch (const std::exception &) {
    return std::nullopt;
  }
  return pkg;
}

LinkPackage publishLinkPackage(const MutableKeys &keys, std::string salt,
                               std::string title, const std::int64_t sequence,
                               const std::uint64_t published,
                               std::vector<GlobalLink> links,
                               std::map<std::string, Scroll> scrolls,
                               std::vector<PublicationPin> publications) {
  LinkPackage pkg;
  pkg.curator      = keys.publicKey;
  pkg.salt         = std::move(salt);
  pkg.title        = std::move(title);
  pkg.sequence     = sequence;
  pkg.published    = published;
  pkg.links        = std::move(links);
  pkg.scrolls      = std::move(scrolls);
  pkg.publications = std::move(publications);

  pkg.signature = signMutableItem(linkPackageSigningBuffer(pkg), keys);
  return pkg;
}

std::vector<std::string> linkPackageScrollKeys(const LinkPackage &pkg) {
  std::set<std::string> keys;
  for (const auto &link : pkg.links) {
    for (const auto &span : link.left) keys.insert(span.scroll);
    for (const auto &span : link.right) keys.insert(span.scroll);
  }
  return {keys.begin(), keys.end()};
}
void reviewLinkPackage(const LinkPackage &pkg) {
  if (pkg.curator.isZero() || pkg.salt.empty() || pkg.salt.size() > 64 ||
      pkg.salt == "catalog" || pkg.title.empty() || pkg.title.size() > 1024 ||
      pkg.sequence < 0 ||
      pkg.published > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()) ||
      pkg.links.empty() || pkg.links.size() > 4096 ||
      pkg.scrolls.size() > 256 || pkg.publications.size() > 128 ||
      !verifyLinkPackage(pkg))
    throw LinkPackageUnreadable(
        "Link package format 1: invalid signed package or limits");
  for (const auto &[key, scroll] : pkg.scrolls) {
    if (key != scrollKey(scroll) || scroll.segments.empty() ||
        scroll.segments.size() > 4096)
      throw LinkPackageUnreadable(
          "Link package format 1: scroll identity/deployment mismatch");
    std::uint64_t end = 0;
    for (const auto &segment : scroll.segments) {
      const std::filesystem::path path(segment.path);
      if (segment.torrent.isZero() || segment.path.empty() ||
          path.has_root_path() ||
          segment.path.find_first_of("\\\0", 0, 2) != std::string::npos ||
          std::ranges::any_of(
              path,
              [](const auto &part) { return part == "." || part == ".."; }) ||
          segment.streamOffset >
              std::numeric_limits<std::uint64_t>::max() - segment.length ||
          !segment.length || segment.at < end ||
          segment.at >
              std::numeric_limits<std::uint64_t>::max() - segment.length)
        throw LinkPackageUnreadable("Link package format 1: invalid or "
                                    "overlapping immutable scroll segments");
      end = segment.at + segment.length;
    }
  }
  for (const auto &link : pkg.links) {
    if (link.left.empty() || link.right.empty() || link.left.size() > 4096 ||
        link.right.size() > 4096)
      throw LinkPackageUnreadable(
          "Link package format 1: empty or oversized endset");
    for (const auto *ends : {&link.left, &link.right})
      for (const auto &span : *ends) {
        const auto found = pkg.scrolls.find(span.scroll);
        if (span.empty() ||
            span.start >
                std::numeric_limits<std::uint64_t>::max() - span.length ||
            found == pkg.scrolls.end())
          throw LinkPackageUnreadable(
              "Link package format 1: undeclared or invalid endpoint");
        auto covered = span.start;
        for (const auto &segment : found->second.segments) {
          if (segment.torrent.isZero() || segment.path.empty() ||
              segment.at >
                  std::numeric_limits<std::uint64_t>::max() - segment.length)
            throw LinkPackageUnreadable(
                "Link package format 1: invalid immutable scroll segment");
          if (segment.at > covered) break;
          if (segment.at + segment.length > covered)
            covered = segment.at + segment.length;
          if (covered >= span.end()) break;
        }
        if (covered < span.end())
          throw LinkPackageUnreadable(
              "Link package format 1: endpoint exceeds declared scroll ranges");
      }
  }
  std::set<std::string> pins;
  for (const auto &pin : pkg.publications) {
    if (pin.publisher.isZero() || pin.salt.empty() || pin.salt.size() > 64 ||
        pin.hash.isZero() || pin.sequence < 0 || pin.title.empty() ||
        pin.title.size() > 1024 ||
        !pins.insert(scrollKeyFor(pin.publisher, pin.salt)).second)
      throw LinkPackageUnreadable(
          "Link package format 1: invalid or duplicate publication citation");
  }
  (void)encodeLinkPackage(pkg);
}

AdoptedLinksResult adoptLinkPackage(Store &store, const LinkPackage &pkg,
                                    const ProminenceTier tier) {
  if (!verifyLinkPackage(pkg)) {
    throw std::runtime_error(
        "link package signature does not verify for curator " +
        pkg.curator.hex());
  }

  AdoptedLinksResult result;
  for (const auto &[key, scroll] : pkg.scrolls) {
    store.addScroll(scroll);
    result.scrollsAdded++;
  }

  for (const auto &glink : pkg.links) {
    Link link;
    link.type    = glink.type;
    link.tier    = tier;
    link.owner   = glink.owner.empty() ? pkg.title : glink.owner;
    link.curator = pkg.curator.hex();

    for (const auto &span : glink.left) {
      if (const auto local = localise(store, span, pkg.scrolls)) {
        link.left.push_back(*local);
      }
    }
    for (const auto &span : glink.right) {
      if (const auto local = localise(store, span, pkg.scrolls)) {
        link.right.push_back(*local);
      }
    }

    if (!link.left.empty() && !link.right.empty()) {
      store.addLink(store.latest(), link);
      result.linksAdopted++;
    }
  }

  return result;
}

DhtTarget linkPackageRendezvousTarget(const std::string &scrollKey) {
  return DhtTarget{sha1("xanalinks:" + scrollKey)};
}

} // namespace xanadu
