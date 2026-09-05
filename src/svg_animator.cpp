/**
 * @file svg_animator.cpp
 * @brief Vector animation and animated SVG (SMIL) playback engine via ThorVG.
 */
#include <gleditor/svg_animator.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef GLEDITOR_HAVE_SVG_THORVG
#include <thorvg.h>
#endif

namespace gleditor {

#ifdef GLEDITOR_HAVE_SVG_THORVG

namespace {

std::mutex initMutex;
int initRefCount = 0;

void thorvgRef() {
  const std::lock_guard<std::mutex> lock(initMutex);
  if (0 == initRefCount++) {
    tvg::Initializer::init();
  }
}

void thorvgUnref() {
  const std::lock_guard<std::mutex> lock(initMutex);
  if (0 == --initRefCount) {
    tvg::Initializer::term();
  }
}

float parseTimeSeconds(std::string_view str) {
  while (!str.empty() &&
         std::isspace(static_cast<unsigned char>(str.front()))) {
    str.remove_prefix(1);
  }
  while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
    str.remove_suffix(1);
  }
  if (str.empty()) {
    return 0.0F;
  }
  float multiplier = 1.0F;
  if (str.ends_with("ms")) {
    multiplier = 0.001F;
    str.remove_suffix(2);
  } else if (str.ends_with('s')) {
    multiplier = 1.0F;
    str.remove_suffix(1);
  } else if (str.ends_with("min")) {
    multiplier = 60.0F;
    str.remove_suffix(3);
  } else if (str.ends_with('h')) {
    multiplier = 3600.0F;
    str.remove_suffix(1);
  }
  try {
    return std::stof(std::string(str)) * multiplier;
  } catch (...) {
    return 0.0F;
  }
}

std::vector<float> parseNumbers(std::string_view str) {
  std::vector<float> nums;
  const char *p   = str.data();
  const char *end = p + str.size();
  while (p < end) {
    while (p < end &&
           (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) {
      ++p;
    }
    if (p >= end) {
      break;
    }
    char *next      = nullptr;
    const float val = std::strtof(p, &next);
    if (next == p) {
      break;
    }
    nums.push_back(val);
    p = next;
  }
  return nums;
}

std::vector<std::string> splitSemicolons(std::string_view str) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start < str.size()) {
    const std::size_t semi = str.find(';', start);
    if (semi == std::string_view::npos) {
      parts.emplace_back(str.substr(start));
      break;
    }
    parts.emplace_back(str.substr(start, semi - start));
    start = semi + 1;
  }
  for (auto &part : parts) {
    while (!part.empty() &&
           std::isspace(static_cast<unsigned char>(part.front()))) {
      part.erase(part.begin());
    }
    while (!part.empty() &&
           std::isspace(static_cast<unsigned char>(part.back()))) {
      part.pop_back();
    }
  }
  return parts;
}

bool parseHexColor(std::string_view str, float &r, float &g, float &b) {
  if (str.starts_with('#')) {
    str.remove_prefix(1);
  } else {
    return false;
  }
  if (str.size() == 3) {
    try {
      const int rInt = std::stoi(std::string(1, str[0]), nullptr, 16);
      const int gInt = std::stoi(std::string(1, str[1]), nullptr, 16);
      const int bInt = std::stoi(std::string(1, str[2]), nullptr, 16);
      r              = static_cast<float>(rInt * 17) / 255.0F;
      g              = static_cast<float>(gInt * 17) / 255.0F;
      b              = static_cast<float>(bInt * 17) / 255.0F;
      return true;
    } catch (...) {
      return false;
    }
  }
  if (str.size() == 6) {
    try {
      const int rInt = std::stoi(std::string(str.substr(0, 2)), nullptr, 16);
      const int gInt = std::stoi(std::string(str.substr(2, 2)), nullptr, 16);
      const int bInt = std::stoi(std::string(str.substr(4, 2)), nullptr, 16);
      r              = static_cast<float>(rInt) / 255.0F;
      g              = static_cast<float>(gInt) / 255.0F;
      b              = static_cast<float>(bInt) / 255.0F;
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
}

std::string formatHexColor(float r, float g, float b) {
  const int rInt = std::clamp(static_cast<int>(std::round(r * 255.0F)), 0, 255);
  const int gInt = std::clamp(static_cast<int>(std::round(g * 255.0F)), 0, 255);
  const int bInt = std::clamp(static_cast<int>(std::round(b * 255.0F)), 0, 255);
  std::ostringstream ss;
  ss << '#' << std::hex << std::setfill('0') << std::setw(2) << rInt
     << std::setw(2) << gInt << std::setw(2) << bInt;
  return ss.str();
}

std::string interpolateValue(std::string_view fromStr, std::string_view toStr,
                             float progress) {
  float r1 = 0.0F, g1 = 0.0F, b1 = 0.0F;
  float r2 = 0.0F, g2 = 0.0F, b2 = 0.0F;
  if (parseHexColor(fromStr, r1, g1, b1) && parseHexColor(toStr, r2, g2, b2)) {
    return formatHexColor(r1 + (r2 - r1) * progress, g1 + (g2 - g1) * progress,
                          b1 + (b2 - b1) * progress);
  }

  const auto nums1 = parseNumbers(fromStr);
  const auto nums2 = parseNumbers(toStr);
  if (!nums1.empty() && nums1.size() == nums2.size()) {
    std::ostringstream ss;
    for (std::size_t i = 0; i < nums1.size(); ++i) {
      if (i > 0) {
        ss << ' ';
      }
      const float v = nums1[i] + (nums2[i] - nums1[i]) * progress;
      ss << v;
    }
    return ss.str();
  }

  return (progress >= 1.0F) ? std::string(toStr) : std::string(fromStr);
}

// DOM model for SMIL SVG elements
struct SvgNode {
  std::string tag;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<std::unique_ptr<SvgNode>> children;
  SvgNode *parent{nullptr};
  std::string id;

  [[nodiscard]] std::string getAttr(std::string_view name) const {
    for (const auto &a : attrs) {
      if (a.first == name) {
        return a.second;
      }
    }
    return "";
  }

  void setAttr(std::string_view name, std::string_view value) {
    for (auto &a : attrs) {
      if (a.first == name) {
        a.second = value;
        return;
      }
    }
    attrs.emplace_back(std::string(name), std::string(value));
  }
};

struct AnimationDesc {
  std::string targetId;
  SvgNode *targetNode{nullptr};
  std::string tag; // "animate", "animateTransform", "set"
  std::string attributeName;
  std::string transformType; // for animateTransform: rotate, translate, scale,
                             // skewX, skewY
  float dur{1.0F};
  float begin{0.0F};
  bool indefinite{false};
  float repeatCount{1.0F};
  bool freeze{false};
  bool additiveSum{false};
  std::string from;
  std::string to;
  std::vector<std::string> values;
  std::vector<float> keyTimes;
};

void collectNodesById(SvgNode *node,
                      std::unordered_map<std::string, SvgNode *> &map) {
  if (!node) {
    return;
  }
  if (!node->id.empty()) {
    map[node->id] = node;
  }
  for (const auto &child : node->children) {
    collectNodesById(child.get(), map);
  }
}

void collectAnimations(
    SvgNode *node, std::vector<AnimationDesc> &animations,
    const std::unordered_map<std::string, SvgNode *> &idMap) {
  if (!node) {
    return;
  }
  for (const auto &child : node->children) {
    if (child->tag == "animate" || child->tag == "animateTransform" ||
        child->tag == "set" || child->tag == "animateMotion") {
      AnimationDesc desc;
      desc.tag           = child->tag;
      desc.attributeName = child->getAttr("attributeName");
      desc.transformType = child->getAttr("type");
      if (desc.transformType.empty() && child->tag == "animateTransform") {
        desc.transformType = "translate";
      }

      std::string targetHref = child->getAttr("href");
      if (targetHref.empty()) {
        targetHref = child->getAttr("xlink:href");
      }
      if (targetHref.starts_with('#')) {
        desc.targetId = targetHref.substr(1);
        const auto it = idMap.find(desc.targetId);
        if (it != idMap.end()) {
          desc.targetNode = it->second;
        }
      }
      if (!desc.targetNode) {
        desc.targetNode = node;
      }

      const std::string durStr = child->getAttr("dur");
      desc.dur = durStr.empty() ? 1.0F : parseTimeSeconds(durStr);
      if (desc.dur <= 0.0F) {
        desc.dur = 1.0F;
      }

      const std::string beginStr = child->getAttr("begin");
      desc.begin = beginStr.empty() ? 0.0F : parseTimeSeconds(beginStr);

      const std::string repeatStr = child->getAttr("repeatCount");
      if (repeatStr == "indefinite") {
        desc.indefinite = true;
      } else if (!repeatStr.empty()) {
        try {
          desc.repeatCount = std::stof(repeatStr);
        } catch (...) {
          desc.repeatCount = 1.0F;
        }
      }

      desc.freeze      = (child->getAttr("fill") == "freeze");
      desc.additiveSum = (child->getAttr("additive") == "sum");

      desc.from = child->getAttr("from");
      desc.to   = child->getAttr("to");

      const std::string valuesStr = child->getAttr("values");
      if (!valuesStr.empty()) {
        desc.values = splitSemicolons(valuesStr);
      }

      const std::string keyTimesStr = child->getAttr("keyTimes");
      if (!keyTimesStr.empty()) {
        desc.keyTimes = parseNumbers(keyTimesStr);
      }

      animations.push_back(std::move(desc));
    }
    collectAnimations(child.get(), animations, idMap);
  }
}

std::unique_ptr<SvgNode> parseXml(std::string_view xml) {
  std::size_t i         = 0;
  const std::size_t len = xml.size();
  auto skipWs           = [&]() {
    while (i < len && std::isspace(static_cast<unsigned char>(xml[i]))) {
      ++i;
    }
  };
  std::unique_ptr<SvgNode> root = nullptr;
  SvgNode *current              = nullptr;

  while (i < len) {
    skipWs();
    if (i >= len) {
      break;
    }
    if (xml[i] == '<') {
      if (i + 1 < len && xml[i + 1] == '!') {
        if (i + 3 < len && xml[i + 2] == '-' && xml[i + 3] == '-') {
          // <!-- comment -->
          i += 4;
          while (i + 2 < len &&
                 !(xml[i] == '-' && xml[i + 1] == '-' && xml[i + 2] == '>')) {
            ++i;
          }
          i = std::min(i + 3, len);
          continue;
        }
        // <!DOCTYPE ...>
        while (i < len && xml[i] != '>') {
          ++i;
        }
        if (i < len) {
          ++i;
        }
        continue;
      }
      if (i + 1 < len && xml[i + 1] == '?') {
        // <?xml ... ?>
        while (i + 1 < len && !(xml[i] == '?' && xml[i + 1] == '>')) {
          ++i;
        }
        i = std::min(i + 2, len);
        continue;
      }
      if (i + 1 < len && xml[i + 1] == '/') {
        // </tag>
        i += 2;
        skipWs();
        while (i < len && xml[i] != '>') {
          ++i;
        }
        if (i < len) {
          ++i;
        }
        if (current) {
          current = current->parent;
        }
        continue;
      }

      // <tag
      ++i;
      skipWs();
      const std::size_t tagStart = i;
      while (i < len && !std::isspace(static_cast<unsigned char>(xml[i])) &&
             xml[i] != '/' && xml[i] != '>') {
        ++i;
      }
      auto node    = std::make_unique<SvgNode>();
      node->tag    = std::string(xml.substr(tagStart, i - tagStart));
      node->parent = current;

      while (i < len) {
        skipWs();
        if (i >= len || xml[i] == '/' || xml[i] == '>') {
          break;
        }
        const std::size_t nameStart = i;
        while (i < len && !std::isspace(static_cast<unsigned char>(xml[i])) &&
               xml[i] != '=' && xml[i] != '/' && xml[i] != '>') {
          ++i;
        }
        std::string attrName(xml.substr(nameStart, i - nameStart));
        skipWs();
        std::string attrVal;
        if (i < len && xml[i] == '=') {
          ++i;
          skipWs();
          if (i < len && (xml[i] == '"' || xml[i] == '\'')) {
            const char quote           = xml[i++];
            const std::size_t valStart = i;
            while (i < len && xml[i] != quote) {
              ++i;
            }
            attrVal = std::string(xml.substr(valStart, i - valStart));
            if (i < len) {
              ++i;
            }
          } else {
            const std::size_t valStart = i;
            while (i < len &&
                   !std::isspace(static_cast<unsigned char>(xml[i])) &&
                   xml[i] != '>' && xml[i] != '/') {
              ++i;
            }
            attrVal = std::string(xml.substr(valStart, i - valStart));
          }
        }
        if (attrName == "id") {
          node->id = attrVal;
        }
        node->attrs.emplace_back(std::move(attrName), std::move(attrVal));
      }
      skipWs();
      bool selfClosing = false;
      if (i < len && xml[i] == '/') {
        selfClosing = true;
        ++i;
        skipWs();
      }
      if (i < len && xml[i] == '>') {
        ++i;
      }

      SvgNode *rawNode = node.get();
      if (!root) {
        root    = std::move(node);
        current = root.get();
      } else if (current) {
        current->children.push_back(std::move(node));
        if (!selfClosing) {
          current = rawNode;
        }
      }
      if (selfClosing && current == rawNode) {
        current = current->parent;
      }
    } else {
      while (i < len && xml[i] != '<') {
        ++i;
      }
    }
  }
  return root;
}

std::string serializeNode(const SvgNode *node) {
  if (!node) {
    return "";
  }
  std::string out = "<" + node->tag;
  for (const auto &a : node->attrs) {
    out += " " + a.first + "=\"" + a.second + "\"";
  }
  if (node->children.empty()) {
    out += "/>";
  } else {
    out += ">";
    for (const auto &child : node->children) {
      if (child->tag != "animate" && child->tag != "animateTransform" &&
          child->tag != "set" && child->tag != "animateMotion") {
        out += serializeNode(child.get());
      }
    }
    out += "</" + node->tag + ">";
  }
  return out;
}

class SvgSmilAnimator final : public SvgAnimator {
public:
  SvgSmilAnimator(std::unique_ptr<SvgNode> root, int width, int height,
                  float duration, std::vector<AnimationDesc> animations)
      : root_(std::move(root)), width_(width), height_(height),
        duration_(duration), animations_(std::move(animations)) {
    thorvgRef();
  }

  ~SvgSmilAnimator() override { thorvgUnref(); }

  [[nodiscard]] int width() const override { return width_; }
  [[nodiscard]] int height() const override { return height_; }
  [[nodiscard]] float duration() const override { return duration_; }

  bool renderFrame(float seconds,
                   std::vector<std::uint32_t> &rgbaOut) override {
    if (width_ <= 0 || height_ <= 0 || !root_) {
      return false;
    }

    // Apply animation attributes at timestamp
    std::unordered_map<SvgNode *, std::string> transforms;

    for (const auto &anim : animations_) {
      if (!anim.targetNode) {
        continue;
      }
      const float tLocal = seconds - anim.begin;
      float progress     = 0.0F;
      if (tLocal <= 0.0F) {
        progress = 0.0F;
      } else if (anim.indefinite) {
        const float tCycle = std::fmod(tLocal, anim.dur);
        progress           = tCycle / anim.dur;
      } else {
        const float totalActive = anim.dur * anim.repeatCount;
        if (tLocal >= totalActive) {
          progress = anim.freeze ? 1.0F : 0.0F;
        } else {
          const float tCycle = std::fmod(tLocal, anim.dur);
          progress           = tCycle / anim.dur;
        }
      }

      std::string interpolated;
      if (!anim.values.empty()) {
        if (anim.values.size() == 1) {
          interpolated = anim.values[0];
        } else {
          const std::size_t count     = anim.values.size();
          const std::size_t intervals = count - 1;
          const float scaled = progress * static_cast<float>(intervals);
          const auto idx     = std::clamp(static_cast<std::size_t>(scaled),
                                          std::size_t{0}, intervals - 1);
          const float localP = scaled - static_cast<float>(idx);
          interpolated =
              interpolateValue(anim.values[idx], anim.values[idx + 1], localP);
        }
      } else {
        interpolated = interpolateValue(anim.from, anim.to, progress);
      }

      if (anim.tag == "animateTransform") {
        std::string xform = anim.transformType + "(" + interpolated + ")";
        if (anim.additiveSum) {
          if (!transforms[anim.targetNode].empty()) {
            transforms[anim.targetNode] += " ";
          }
          transforms[anim.targetNode] += xform;
        } else {
          transforms[anim.targetNode] = xform;
        }
      } else {
        anim.targetNode->setAttr(anim.attributeName, interpolated);
      }
    }

    for (const auto &entry : transforms) {
      entry.first->setAttr("transform", entry.second);
    }

    const std::string xml = serializeNode(root_.get());

    auto *picture = tvg::Picture::gen();
    if (!picture) {
      return false;
    }
    const auto loadRes =
        picture->load(xml.data(), static_cast<std::uint32_t>(xml.size()),
                      "svg+xml", nullptr, true);
    if (tvg::Result::Success != loadRes) {
      picture->unref();
      return false;
    }
    picture->size(static_cast<float>(width_), static_cast<float>(height_));

    auto *canvas = tvg::SwCanvas::gen();
    if (!canvas) {
      picture->unref();
      return false;
    }

    rgbaOut.resize(static_cast<std::size_t>(width_) * height_);
    std::fill(rgbaOut.begin(), rgbaOut.end(), 0U);

    const auto targetRes = canvas->target(
        rgbaOut.data(), static_cast<std::uint32_t>(width_),
        static_cast<std::uint32_t>(width_), static_cast<std::uint32_t>(height_),
        tvg::ColorSpace::ABGR8888S);
    if (tvg::Result::Success != targetRes ||
        tvg::Result::Success != canvas->add(picture)) {
      delete canvas;
      picture->unref();
      return false;
    }

    canvas->draw();
    canvas->sync();
    delete canvas;
    return true;
  }

private:
  std::unique_ptr<SvgNode> root_;
  int width_{0};
  int height_{0};
  float duration_{1.0F};
  std::vector<AnimationDesc> animations_;
};

class LottieAnimator final : public SvgAnimator {
public:
  LottieAnimator(tvg::Animation *anim, int width, int height)
      : anim_(anim), width_(width), height_(height) {
    thorvgRef();
  }

  ~LottieAnimator() override {
    delete anim_;
    thorvgUnref();
  }

  [[nodiscard]] int width() const override { return width_; }
  [[nodiscard]] int height() const override { return height_; }
  [[nodiscard]] float duration() const override {
    if (!anim_) {
      return 1.0F;
    }
    const float d = anim_->duration();
    return (d > 0.0F) ? d : 1.0F;
  }

  bool renderFrame(float seconds,
                   std::vector<std::uint32_t> &rgbaOut) override {
    if (!anim_ || width_ <= 0 || height_ <= 0) {
      return false;
    }
    const float dur      = duration();
    const float tClamped = std::fmod(std::max(0.0F, seconds), dur);
    const float total    = anim_->totalFrame();
    const float frameNo =
        (dur > 0.0F && total > 0.0F) ? (tClamped / dur) * (total - 1.0F) : 0.0F;
    anim_->frame(frameNo);

    auto *canvas = tvg::SwCanvas::gen();
    if (!canvas) {
      return false;
    }
    rgbaOut.resize(static_cast<std::size_t>(width_) * height_);
    std::fill(rgbaOut.begin(), rgbaOut.end(), 0U);

    canvas->target(rgbaOut.data(), static_cast<std::uint32_t>(width_),
                   static_cast<std::uint32_t>(width_),
                   static_cast<std::uint32_t>(height_),
                   tvg::ColorSpace::ABGR8888S);
    canvas->add(anim_->picture());
    canvas->draw();
    canvas->sync();
    delete canvas;
    return true;
  }

private:
  tvg::Animation *anim_{nullptr};
  int width_{0};
  int height_{0};
};

} // namespace

#endif // GLEDITOR_HAVE_SVG_THORVG

bool SvgAnimator::isAnimated(std::span<const std::uint8_t> bytes) {
#ifdef GLEDITOR_HAVE_SVG_THORVG
  if (bytes.empty()) {
    return false;
  }

  // Evaluate vector asset directly using ThorVG's Animation context:
  // Query duration() and totalFrame(); static files return 0.0 duration and <=
  // 1 frames.
  thorvgRef();
  auto *anim = tvg::Animation::gen();
  if (anim) {
    auto res = anim->picture()->load(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<std::uint32_t>(bytes.size()), "", nullptr, true);
    if (res != tvg::Result::Success) {
      res = anim->picture()->load(reinterpret_cast<const char *>(bytes.data()),
                                  static_cast<std::uint32_t>(bytes.size()),
                                  "lottie", nullptr, true);
    }
    const bool animated = (tvg::Result::Success == res &&
                           (anim->duration() > 0.0F || anim->totalFrame() > 1));
    delete anim;
    thorvgUnref();
    if (animated) {
      return true;
    }
  } else {
    thorvgUnref();
  }

  // SMIL SVG path: check for XML SMIL animation elements (<animate>,
  // <animateTransform>, etc.)
  const std::string_view sv(reinterpret_cast<const char *>(bytes.data()),
                            bytes.size());
  if (sv.find("<svg") != std::string_view::npos ||
      sv.find("<SVG") != std::string_view::npos) {
    if (sv.find("<animate") != std::string_view::npos ||
        sv.find("<animateTransform") != std::string_view::npos ||
        sv.find("<animateMotion") != std::string_view::npos ||
        sv.find("<animateColor") != std::string_view::npos ||
        sv.find("<set ") != std::string_view::npos ||
        sv.find("<set\t") != std::string_view::npos ||
        sv.find("<set\n") != std::string_view::npos) {
      return true;
    }
  }
#else
  (void)bytes;
#endif
  return false;
}

std::unique_ptr<SvgAnimator>
SvgAnimator::load(std::span<const std::uint8_t> bytes) {
#ifdef GLEDITOR_HAVE_SVG_THORVG
  if (bytes.empty()) {
    return nullptr;
  }

  // 1. ThorVG Animation context strategy
  thorvgRef();
  auto *anim = tvg::Animation::gen();
  if (anim) {
    auto res = anim->picture()->load(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<std::uint32_t>(bytes.size()), "", nullptr, true);
    if (res != tvg::Result::Success) {
      res = anim->picture()->load(reinterpret_cast<const char *>(bytes.data()),
                                  static_cast<std::uint32_t>(bytes.size()),
                                  "lottie", nullptr, true);
    }
    if (tvg::Result::Success == res &&
        (anim->duration() > 0.0F || anim->totalFrame() > 1)) {
      float w = 0.0F;
      float h = 0.0F;
      anim->picture()->size(&w, &h);
      int width  = static_cast<int>(std::round(w));
      int height = static_cast<int>(std::round(h));
      if (width <= 0) {
        width = 512;
      }
      if (height <= 0) {
        height = 512;
      }
      thorvgUnref();
      return std::make_unique<LottieAnimator>(anim, width, height);
    }
    delete anim;
  }
  thorvgUnref();

  const std::string_view sv(reinterpret_cast<const char *>(bytes.data()),
                            bytes.size());

  // SMIL SVG path
  auto root = parseXml(sv);
  if (!root) {
    return nullptr;
  }

  std::unordered_map<std::string, SvgNode *> idMap;
  collectNodesById(root.get(), idMap);

  std::vector<AnimationDesc> animations;
  collectAnimations(root.get(), animations, idMap);

  if (animations.empty()) {
    return nullptr;
  }

  float maxDur = 0.0F;
  for (const auto &anim : animations) {
    if (anim.indefinite) {
      maxDur = std::max(maxDur, anim.dur);
    } else {
      maxDur = std::max(maxDur, anim.begin + anim.dur * anim.repeatCount);
    }
  }
  if (maxDur <= 0.0F) {
    maxDur = 1.0F;
  }

  // Extract dimensions via ThorVG Picture or attributes
  thorvgRef();
  int width     = 0;
  int height    = 0;
  auto *picture = tvg::Picture::gen();
  if (picture) {
    const auto res = picture->load(reinterpret_cast<const char *>(bytes.data()),
                                   static_cast<std::uint32_t>(bytes.size()),
                                   "svg+xml", nullptr, true);
    if (tvg::Result::Success == res) {
      float w = 0.0F, h = 0.0F;
      picture->size(&w, &h);
      width  = static_cast<int>(std::round(w));
      height = static_cast<int>(std::round(h));
    }
    picture->unref();
  }
  thorvgUnref();

  if (width <= 0 || height <= 0) {
    const std::string wStr = root->getAttr("width");
    const std::string hStr = root->getAttr("height");
    if (!wStr.empty() && !hStr.empty()) {
      try {
        width  = std::stoi(wStr);
        height = std::stoi(hStr);
      } catch (...) {
      }
    }
    if (width <= 0 || height <= 0) {
      const std::string vb = root->getAttr("viewBox");
      const auto nums      = parseNumbers(vb);
      if (nums.size() >= 4 && nums[2] > 0.0F && nums[3] > 0.0F) {
        width  = static_cast<int>(std::round(nums[2]));
        height = static_cast<int>(std::round(nums[3]));
      }
    }
  }
  if (width <= 0) width = 512;
  if (height <= 0) height = 512;

  return std::make_unique<SvgSmilAnimator>(std::move(root), width, height,
                                           maxDur, std::move(animations));
#else
  (void)bytes;
  return nullptr;
#endif
}

} // namespace gleditor
