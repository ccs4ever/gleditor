/**
 * @file image_cache.cpp
 * @brief Implementation of image decoding, caching, and GPU RGBA8 texture
 * atlas.
 */
#include <gleditor/image_cache.hpp>
#include <gleditor/log.hpp>
#include <gleditor/sdl_compat.hpp>

#ifdef GLEDITOR_HAVE_SDL_IMAGE
#if GLEDITOR_SDL_MAJOR == 3
#include <SDL3_image/SDL_image.h>
#else
#include <SDL2/SDL_image.h>
#endif
#endif

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>

namespace gleditor {

namespace {

DecodedImage surfaceToDecodedImage(SDL_Surface *surface) {
  if (nullptr == surface) {
    return {};
  }

  SDL_Surface *rgbaSurface = sdl::convertSurfaceToRgba32(surface);
  if (nullptr == rgbaSurface) {
    SDL_DestroySurface(surface);
    return {};
  }

  DecodedImage result;
  result.width  = rgbaSurface->w;
  result.height = rgbaSurface->h;
  result.rgba.resize(
      static_cast<std::size_t>(result.width * result.height * 4));

  const auto *const srcPixels =
      static_cast<const std::uint8_t *>(rgbaSurface->pixels);
  for (int y = 0; y < result.height; ++y) {
    std::memcpy(result.rgba.data() +
                    (static_cast<std::size_t>(y * result.width) * 4),
                srcPixels + (y * rgbaSurface->pitch),
                static_cast<std::size_t>(result.width) * 4);
  }

  SDL_DestroySurface(rgbaSurface);
  if (rgbaSurface != surface) {
    SDL_DestroySurface(surface);
  }

  return result;
}

} // namespace

DecodedImage decodeImageBuffer(const std::span<const std::uint8_t> bytes,
                               const MimeType & /*mime*/) {
  if (bytes.empty()) {
    return {};
  }

  SDL_Surface *surface = nullptr;

#if GLEDITOR_SDL_MAJOR == 3
  SDL_IOStream *const io = SDL_IOFromConstMem(bytes.data(), bytes.size());
  if (nullptr != io) {
#ifdef GLEDITOR_HAVE_SDL_IMAGE
    surface = IMG_Load_IO(io, true);
#else
    surface = SDL_LoadBMP_IO(io, true);
#endif
  }
#else
  SDL_RWops *const rw =
      SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size()));
  if (nullptr != rw) {
#ifdef GLEDITOR_HAVE_SDL_IMAGE
    surface = IMG_Load_RW(rw, 1);
#else
    surface = SDL_LoadBMP_RW(rw, 1);
#endif
  }
#endif

  return surfaceToDecodedImage(surface);
}

DecodedImage decodeImageBuffer(const std::string_view bytes,
                               const MimeType &mime) {
  return decodeImageBuffer(
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()),
      mime);
}

DecodedImage decodeImageFile(const std::string &filePath) {
  SDL_Surface *surface = nullptr;
#ifdef GLEDITOR_HAVE_SDL_IMAGE
  surface = IMG_Load(filePath.c_str());
#else
  surface = SDL_LoadBMP(filePath.c_str());
#endif
  return surfaceToDecodedImage(surface);
}

ImageCache::ImageCache(render::RenderDevice *const device, const int atlasSize,
                       const int maxLayers)
    : device_(device), atlasSize_(atlasSize), maxLayers_(maxLayers) {
  ensureAtlasAllocated();
}

ImageCache::~ImageCache() {
  if (atlasHandle_.valid() && nullptr != device_) {
    device_->destroyTexture(atlasHandle_);
    atlasHandle_ = {};
  }
}

void ImageCache::ensureAtlasAllocated() {
  if (!atlasHandle_.valid() && nullptr != device_) {
    atlasHandle_ = device_->createTextureArray(atlasSize_, maxLayers_,
                                               render::TextureFormat::RGBA8, 1);
  }
}

ImageResource ImageCache::put(const std::string &id,
                              const DecodedImage &image) {
  if (!image.valid()) {
    return {};
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (const auto it = cache_.find(id); it != cache_.end()) {
    return it->second;
  }

  ensureAtlasAllocated();

  const int imgW = std::min(image.width, atlasSize_);
  const int imgH = std::min(image.height, atlasSize_);

  int chosenLayer = -1;
  int posX        = 0;
  int posY        = 0;

  for (auto &layer : layers_) {
    if (layer.currentX + imgW <= atlasSize_ &&
        layer.currentY + imgH <= atlasSize_) {
      chosenLayer = layer.layerIndex;
      posX        = layer.currentX;
      posY        = layer.currentY;
      layer.currentX += imgW + 2;
      layer.rowHeight = std::max(layer.rowHeight, imgH + 2);
      break;
    }
    if (layer.currentY + layer.rowHeight + imgH <= atlasSize_) {
      layer.currentY += layer.rowHeight;
      layer.currentX  = 0;
      layer.rowHeight = imgH + 2;
      chosenLayer     = layer.layerIndex;
      posX            = layer.currentX;
      posY            = layer.currentY;
      layer.currentX += imgW + 2;
      break;
    }
  }

  if (chosenLayer < 0 && static_cast<int>(layers_.size()) < maxLayers_) {
    const int nextLayer = static_cast<int>(layers_.size());
    layers_.push_back(AtlasLayer{
        .layerIndex = nextLayer,
        .currentX   = imgW + 2,
        .currentY   = 0,
        .rowHeight  = imgH + 2,
    });
    chosenLayer      = nextLayer;
    posX             = 0;
    posY             = 0;
    allocatedLayers_ = static_cast<int>(layers_.size());
  }

  if (chosenLayer < 0) {
    // Atlas full; return fallback resource
    return {};
  }

  if (nullptr != device_ && atlasHandle_.valid()) {
    device_->updateTextureLayer(
        atlasHandle_, chosenLayer, posX, posY, imgW, imgH,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(image.rgba.data()),
            static_cast<std::size_t>(imgW * imgH * 4)));
  }

  const float atlasF = static_cast<float>(atlasSize_);
  ImageResource res{
      .id      = id,
      .width   = image.width,
      .height  = image.height,
      .layer   = chosenLayer,
      .u0      = static_cast<float>(posX) / atlasF,
      .v0      = static_cast<float>(posY) / atlasF,
      .u1      = static_cast<float>(posX + imgW) / atlasF,
      .v1      = static_cast<float>(posY + imgH) / atlasF,
      .texture = atlasHandle_,
  };

  cache_.emplace(id, res);
  return res;
}

std::optional<ImageResource> ImageCache::loadFile(const std::string &filePath) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = cache_.find(filePath); it != cache_.end()) {
      return it->second;
    }
  }

  const DecodedImage decoded = decodeImageFile(filePath);
  if (!decoded.valid()) {
    return std::nullopt;
  }

  return put(filePath, decoded);
}

std::optional<ImageResource>
ImageCache::loadBuffer(const std::string &id,
                       const std::span<const std::uint8_t> bytes,
                       const MimeType &mime) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = cache_.find(id); it != cache_.end()) {
      return it->second;
    }
  }

  const DecodedImage decoded = decodeImageBuffer(bytes, mime);
  if (!decoded.valid()) {
    return std::nullopt;
  }

  return put(id, decoded);
}

std::optional<ImageResource> ImageCache::find(const std::string &id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (const auto it = cache_.find(id); it != cache_.end()) {
    return it->second;
  }
  return std::nullopt;
}

} // namespace gleditor
