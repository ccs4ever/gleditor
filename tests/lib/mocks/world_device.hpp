#pragma once
#include "device.hpp"
#include <algorithm>
#include <cstring>
#include <gleditor/doc.hpp>
#include <map>
#include <vector>

class WorldRecordingDevice : public testing::NiceMock<MockRenderDevice> {
public:
  std::map<std::uint32_t, std::vector<std::byte>> buffers;
  std::vector<Doc::VBORow> drawn;
  std::vector<std::uint32_t> identities;
  std::uint32_t nextBuffer{1};
  std::size_t uploads{}, pipelines{};

  WorldRecordingDevice() {
    ON_CALL(*this, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(*this, createTextureArray)
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    ON_CALL(*this, createBuffer)
        .WillByDefault([this](render::BufferKind, std::size_t size) {
          const auto id = nextBuffer++;
          buffers[id].resize(size);
          return render::BufferHandle{id};
        });
    ON_CALL(*this, resizeBuffer)
        .WillByDefault([this](render::BufferHandle buffer, std::size_t size) {
          buffers[buffer.id].resize(size);
          return buffer;
        });
    ON_CALL(*this, updateBuffer)
        .WillByDefault([this](render::BufferHandle buffer, std::size_t offset,
                              std::span<const std::byte> bytes) {
          uploads += bytes.size();
          auto &storage = buffers[buffer.id];
          storage.resize(std::max(storage.size(), offset + bytes.size()));
          std::memcpy(storage.data() + offset, bytes.data(), bytes.size());
        });
    ON_CALL(*this, createPipeline)
        .WillByDefault([this](const render::PipelineDesc &) {
          return render::PipelineHandle{
              static_cast<std::uint32_t>(++pipelines)};
        });
    ON_CALL(*this, drawGlyphs)
        .WillByDefault([this](const render::DrawUniforms &uniforms,
                              render::BufferHandle buffer, std::size_t offset,
                              std::uint32_t count) {
          const auto &storage = buffers.at(buffer.id);
          ASSERT_LE(offset + count * sizeof(Doc::VBORow), storage.size());
          for (std::uint32_t index = 0; index < count; ++index) {
            Doc::VBORow row{};
            std::memcpy(&row, storage.data() + offset + index * sizeof(row),
                        sizeof(row));
            drawn.push_back(row);
            identities.push_back(uniforms.identity);
          }
        });
  }
};
