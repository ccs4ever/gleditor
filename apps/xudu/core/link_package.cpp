/**
 * @file link_package.cpp
 * @brief Implementation of standalone, signed link packages.
 */
#include "link_package.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

#include "bencode.hpp"
#include "store.hpp"
#include "swarm.hpp"
#include "torrent.hpp"

namespace xudu {

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
  const auto *scroll = value.find("scroll");
  const auto *start  = value.find("start");
  const auto *length = value.find("len");
  if (nullptr == scroll || !scroll->isString() || nullptr == start ||
      !start->isInteger() || nullptr == length || !length->isInteger()) {
    return std::nullopt;
  }
  if (start->asInteger() < 0 || length->asInteger() < 0) {
    return std::nullopt;
  }
  return GlobalSpan{scroll->asString(),
                    static_cast<std::uint64_t>(start->asInteger()),
                    static_cast<std::uint64_t>(length->asInteger())};
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

bencode::Value encodeSegment(const ScrollSegment &segment) {
  return bencode::Value::dict({
      {"at", bencode::Value::integer(static_cast<std::int64_t>(segment.at))},
      {"file", bencode::Value::integer(segment.fileIndex)},
      {"len",
       bencode::Value::integer(static_cast<std::int64_t>(segment.length))},
      {"path", bencode::Value::string(segment.path)},
      {"stream", bencode::Value::integer(
                     static_cast<std::int64_t>(segment.streamOffset))},
      {"torrent",
       bencode::Value::string(std::string{
           reinterpret_cast<const char *>(segment.torrent.bytes.data()),
           segment.torrent.bytes.size()})},
  });
}

std::optional<ScrollSegment> decodeSegment(const bencode::Value &value) {
  const auto *at      = value.find("at");
  const auto *length  = value.find("len");
  const auto *torrent = value.find("torrent");
  const auto *stream  = value.find("stream");
  const auto *file    = value.find("file");
  const auto *path    = value.find("path");
  if (nullptr == at || !at->isInteger() || nullptr == length ||
      !length->isInteger() || nullptr == torrent || !torrent->isString() ||
      torrent->asString().size() != 20 || nullptr == stream ||
      !stream->isInteger() || nullptr == file || !file->isInteger() ||
      nullptr == path || !path->isString()) {
    return std::nullopt;
  }
  ScrollSegment segment;
  segment.at           = static_cast<std::uint64_t>(at->asInteger());
  segment.length       = static_cast<std::uint64_t>(length->asInteger());
  segment.streamOffset = static_cast<std::uint64_t>(stream->asInteger());
  segment.fileIndex    = static_cast<std::uint32_t>(file->asInteger());
  segment.path         = path->asString();
  std::copy(torrent->asString().begin(), torrent->asString().end(),
            segment.torrent.bytes.begin());
  return segment;
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
  const auto *segments = value.find("segments");
  if (nullptr == segments || !segments->isList()) {
    return std::nullopt;
  }
  Scroll scroll;
  const auto *publisher = value.find("publisher");
  if (nullptr != publisher && publisher->isString() &&
      32 == publisher->asString().size()) {
    std::copy(publisher->asString().begin(), publisher->asString().end(),
              scroll.publisher.bytes.begin());
  }
  const auto *salt = value.find("salt");
  if (nullptr != salt && salt->isString()) {
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
  const auto *type    = value.find("type");
  const auto *owner   = value.find("owner");
  const auto *left    = value.find("left");
  const auto *right   = value.find("right");
  const auto *tier    = value.find("tier");
  const auto *curator = value.find("curator");
  if (nullptr == type || !type->isString() || nullptr == owner ||
      !owner->isString() || nullptr == left || nullptr == right) {
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
  if (nullptr != tier && tier->isString()) {
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
  if (nullptr != curator && curator->isString()) {
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

  bencode::Dict dict = {
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
  return bencode::Value::dict(manifestOf(pkg, true)).encode();
}

bool verifyLinkPackage(const LinkPackage &pkg) {
  return verifyMutableItem(linkPackageSigningBuffer(pkg), pkg.signature,
                           pkg.curator);
}

std::optional<LinkPackage> decodeLinkPackage(const std::string_view encoded) {
  bencode::Value root;
  try {
    root = bencode::decode(encoded);
  } catch (const std::exception &) {
    return std::nullopt;
  }
  if (!root.isDict()) {
    return std::nullopt;
  }

  const auto *curator   = root.find(keyCurator);
  const auto *salt      = root.find(keySalt);
  const auto *title     = root.find(keyTitle);
  const auto *sequence  = root.find(keySequence);
  const auto *time      = root.find(keyTime);
  const auto *links     = root.find(keyLinks);
  const auto *scrolls   = root.find(keyScrolls);
  const auto *signature = root.find(keySignature);
  if (nullptr == curator || !curator->isString() ||
      curator->asString().size() != 32 || nullptr == salt ||
      !salt->isString() || nullptr == title || !title->isString() ||
      nullptr == sequence || !sequence->isInteger() || nullptr == time ||
      !time->isInteger() || nullptr == links || !links->isList() ||
      nullptr == scrolls || !scrolls->isDict() || nullptr == signature ||
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

  return pkg;
}

LinkPackage publishLinkPackage(const MutableKeys &keys, std::string salt,
                               std::string title, const std::int64_t sequence,
                               const std::uint64_t published,
                               std::vector<GlobalLink> links,
                               std::map<std::string, Scroll> scrolls) {
  LinkPackage pkg;
  pkg.curator   = keys.publicKey;
  pkg.salt      = std::move(salt);
  pkg.title     = std::move(title);
  pkg.sequence  = sequence;
  pkg.published = published;
  pkg.links     = std::move(links);
  pkg.scrolls   = std::move(scrolls);

  pkg.signature = signMutableItem(linkPackageSigningBuffer(pkg), keys);
  return pkg;
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

} // namespace xudu
