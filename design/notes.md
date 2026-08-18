Part 1: LMDB Storage Configuration & Architecture

LMDB (Lightning Memory-Mapped Database) is ideal for Xanadulogical data structures due to its zero-copy read architecture (mmap), MVCC concurrency (lock-free readers never blocking writers), and B+ tree indexing.

1. Architectural Layout & Database Schema

For gleditor and xudu, storage is cleanly split between raw verified piece payloads and lightweight virtual address lookup stores:

    pieces: Stores raw immutable verified piece payloads indexed by canonical content address (InfoHash, piece_index) with an embedded TextHeader tracking active references (ref_count) and byte length. Reads map directly to const char* without heap allocations.

    vspans: Stores virtual Xanadoc primedia scroll lookup records mapping (scroll_id, start, length) to lightweight descriptors VSpanRecord { hash, piece_index, chunk_offset, length }. Sub-spans point into existing piece entries without duplicating text.

    ext_spans: Stores external content references mapping stream coordinates (InfoHash, stream_offset, length) to VSpanRecord descriptors.

    meta: Stores document roots, dimensional metadata, and user workspace states.

       +-------------------------------------------------------------+
       |                     LMDB Environment                        |
       |  (Single File / Virtual Memory Map: e.g., 256GB max_size)   |
       +-------------------------------------------------------------+
               |                       |                      |
       +---------------+       +---------------+      +---------------+
       | DBI: "pieces" |       | DBI: "vspans" |      | DBI: "meta"   |
       +---------------+       |DBI:"ext_spans"|      +---------------+
       |Key: (Hash,Idx)|       +---------------+      | Key: DocID    |
       |Val: TextHeader|       |Key: VirtualPos|      | Val: RootCell |
       |     + RawText |       |Val: VSpanRec  |      +---------------+
       +---------------+       +---------------+

C++

// lmdb_storage.hpp
#pragma once

#include <lmdb.h>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <cstring>
#include <filesystem>

struct SpanKey {
    uint64_t doc_id;
    uint64_t span_id;
};

struct ZZCellLink {
    uint64_t pos_cell_id;
    uint64_t neg_cell_id;
    uint32_t dimension_id;
    uint32_t flags;
};

struct ZZCellRecord {
    uint64_t cell_id;
    uint64_t span_id;       // Reference to text span in 'spans' db
    uint32_t link_count;
    ZZCellLink links[8];    // Static buffer for primary dimensions, extend as needed
};

class LMDBStorage {
public:
    LMDBStorage(const std::filesystem::path& path, size_t map_size_bytes = 10ULL * 1024 * 1024 * 1024) {
        if (int rc = mdb_env_create(&env_); rc != MDB_SUCCESS) {
            throw std::runtime_error("mdb_env_create failed: " + std::string(mdb_strerror(rc)));
        }

        // Configure environment limits before opening
        mdb_env_set_maxdbs(env_, 8);
        mdb_env_set_maxreaders(env_, 126);
        mdb_env_set_mapsize(env_, map_size_bytes);

        // Flags:
        // - MDB_NOTLS: Multi-threaded reader access per thread
        // - MDB_NORDAHEAD: Disable OS sequential readahead for sparse random graph traversal
        // - MDB_NOSYNC / MDB_NOMETASYNC: Fast flushes (trade off sync durability if desired)
        unsigned int env_flags = MDB_NOTLS | MDB_NORDAHEAD;

        std::filesystem::create_directories(path);
        if (int rc = mdb_env_open(env_, path.c_str(), env_flags, 0644); rc != MDB_SUCCESS) {
            mdb_env_close(env_);
            throw std::runtime_error("mdb_env_open failed: " + std::string(mdb_strerror(rc)));
        }

        open_databases();
    }

    ~LMDBStorage() {
        if (env_) {
            mdb_env_close(env_);
        }
    }

    // Zero-copy read for text spans directly from the mmap page cache
    bool get_span(const SpanKey& key, std::string_view& out_text) {
        MDB_txn* txn = nullptr;
        if (int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn); rc != MDB_SUCCESS) {
            return false;
        }

        MDB_val k{ sizeof(SpanKey), const_cast<SpanKey*>(&key) };
        MDB_val v{};
        int rc = mdb_get(txn, dbi_spans_, &k, &v);
        if (rc == MDB_SUCCESS) {
            out_text = std::string_view(reinterpret_cast<const char*>(v.mv_data), v.mv_size);
            mdb_txn_abort(txn); // Read-only txns abort cleanly
            return true;
        }

        mdb_txn_abort(txn);
        return false;
    }

    // Write text span
    bool put_span(const SpanKey& key, std::string_view text) {
        MDB_txn* txn = nullptr;
        if (int rc = mdb_txn_begin(env_, nullptr, 0, &txn); rc != MDB_SUCCESS) {
            return false;
        }

        MDB_val k{ sizeof(SpanKey), const_cast<SpanKey*>(&key) };
        MDB_val v{ text.size(), const_cast<char*>(text.data()) };

        if (int rc = mdb_put(txn, dbi_spans_, &k, &v, 0); rc != MDB_SUCCESS) {
            mdb_txn_abort(txn);
            return false;
        }

        return mdb_txn_commit(txn) == MDB_SUCCESS;
    }

    // Fetch ZZCell record
    bool get_cell(uint64_t cell_id, ZZCellRecord& out_cell) {
        MDB_txn* txn = nullptr;
        if (int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn); rc != MDB_SUCCESS) {
            return false;
        }

        MDB_val k{ sizeof(uint64_t), &cell_id };
        MDB_val v{};
        int rc = mdb_get(txn, dbi_zzcells_, &k, &v);
        if (rc == MDB_SUCCESS && v.mv_size == sizeof(ZZCellRecord)) {
            std::memcpy(&out_cell, v.mv_data, sizeof(ZZCellRecord));
            mdb_txn_abort(txn);
            return true;
        }

        mdb_txn_abort(txn);
        return false;
    }

    // Insert or update ZZCell record
    bool put_cell(const ZZCellRecord& cell) {
        MDB_txn* txn = nullptr;
        if (int rc = mdb_txn_begin(env_, nullptr, 0, &txn); rc != MDB_SUCCESS) {
            return false;
        }

        MDB_val k{ sizeof(uint64_t), const_cast<uint64_t*>(&cell.cell_id) };
        MDB_val v{ sizeof(ZZCellRecord), const_cast<ZZCellRecord*>(&cell) };

        if (int rc = mdb_put(txn, dbi_zzcells_, &k, &v, 0); rc != MDB_SUCCESS) {
            mdb_txn_abort(txn);
            return false;
        }

        return mdb_txn_commit(txn) == MDB_SUCCESS;
    }

    void sync() {
        if (env_) {
            mdb_env_sync(env_, 1);
        }
    }

private:
    void open_databases() {
        MDB_txn* txn = nullptr;
        if (int rc = mdb_txn_begin(env_, nullptr, 0, &txn); rc != MDB_SUCCESS) {
            throw std::runtime_error("Failed to begin setup transaction: " + std::string(mdb_strerror(rc)));
        }

        mdb_dbi_open(txn, "spans", MDB_CREATE, &dbi_spans_);
        mdb_dbi_open(txn, "zzcells", MDB_CREATE, &dbi_zzcells_);
        mdb_dbi_open(txn, "meta", MDB_CREATE, &dbi_meta_);

        mdb_txn_commit(txn);
    }

    MDB_env* env_{ nullptr };
    MDB_dbi dbi_spans_{ 0 };
    MDB_dbi dbi_zzcells_{ 0 };
    MDB_dbi dbi_meta_{ 0 };
};

1. The Ring Buffer Uploader (src/render/opengl/stream_buffer.hpp)

This utility provides a lock-free, zero-copy staging area for both Vertex Buffer Objects (instance data) and Pixel Unpack Buffers (PBOs for the glyph atlas). By mapping the buffer unsynchronized, you promise the OpenGL driver that you will not overwrite data the GPU is currently reading, which we manage by tracking chunks with glFenceSync.
C++

#pragma once

#include <glad/glad.h>
#include <deque>
#include <cstdint>
#include <cstring>
#include <stdexcept>

struct SyncPoint {
    GLsync sync;
    size_t size;
};

class GLStreamBuffer {
public:
    GLStreamBuffer(GLenum target, size_t capacity_bytes) 
        : target_(target), capacity_(capacity_bytes), head_(0) 
    {
        glGenBuffers(1, &buffer_id_);
        glBindBuffer(target_, buffer_id_);
        
        // Allocate immutable data store (GL_DYNAMIC_DRAW since it changes frequently)
        glBufferData(target_, capacity_, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(target_, 0);
    }

    ~GLStreamBuffer() {
        wait_all_syncs();
        glDeleteBuffers(1, &buffer_id_);
    }

    // Returns a mapped pointer for the CPU to write into, and the GPU offset
    std::pair<void*, size_t> allocate_chunk(size_t size) {
        size_t alignment = 64; 
        head_ = (head_ + alignment - 1) & ~(alignment - 1);

        if (head_ + size > capacity_) {
            head_ = 0; // Wrap buffer
        }

        wait_for_space(size);

        glBindBuffer(target_, buffer_id_);
        // Unsynchronized map: Driver won't stall the CPU even if GPU is reading elsewhere
        void* ptr = glMapBufferRange(target_, head_, size, 
            GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
        
        if (!ptr) throw std::runtime_error("Failed to map stream buffer chunk");

        size_t offset = head_;
        head_ += size;
        return {ptr, offset};
    }

    void flush_and_unmap(size_t offset, size_t size) {
        glBindBuffer(target_, buffer_id_);
        glFlushMappedBufferRange(target_, offset, size);
        glUnmapBuffer(target_);

        // Insert fence to track when GPU finishes with this chunk
        syncs_.push_back({glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), size});
    }

    GLuint get_id() const { return buffer_id_; }

private:
    void wait_for_space(size_t requested_size) {
        while (!syncs_.empty() && (capacity_ - (head_ - syncs_.front().size)) < requested_size) {
            glClientWaitSync(syncs_.front().sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(syncs_.front().sync);
            syncs_.pop_front();
        }
    }

    void wait_all_syncs() {
        for (auto& s : syncs_) {
            glClientWaitSync(s.sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s.sync);
        }
        syncs_.clear();
    }

    GLenum target_;
    GLuint buffer_id_;
    size_t capacity_;
    size_t head_;
    std::deque<SyncPoint> syncs_;
};

2. Integration with the OpenGL Backend (src/render/opengl/render_device_gl.cpp)

Integrate GLStreamBuffer into the OpenGL backend implementation of your render::RenderDevice interface. You will need two stream buffers: one for streaming new glyphs into the atlas, and one for streaming layout instances (positions, dimension flags, etc.).
C++

#include "stream_buffer.hpp"
#include "gleditor/render/render_device.hpp"

class RenderDeviceGL : public render::RenderDevice {
    std::unique_ptr<GLStreamBuffer> pbo_stream_;
    std::unique_ptr<GLStreamBuffer> instance_stream_;
    GLuint glyph_atlas_tex_;

public:
    void init() override {
        // 16MB for streaming glyphs, 32MB for layout instances
        pbo_stream_ = std::make_unique<GLStreamBuffer>(GL_PIXEL_UNPACK_BUFFER, 16 * 1024 * 1024);
        instance_stream_ = std::make_unique<GLStreamBuffer>(GL_ARRAY_BUFFER, 32 * 1024 * 1024);

        glGenTextures(1, &glyph_atlas_tex_);
        glBindTexture(GL_TEXTURE_2D, glyph_atlas_tex_);
        // GL_RED for single-channel coverage
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 4096, 4096, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }

    // Async upload from FreeType/Cairo cache directly to GPU Atlas
    void upload_glyph(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* data) override {
        size_t size = w * h; // Single channel = 1 byte per pixel
        auto [ptr, offset] = pbo_stream_->allocate_chunk(size);
        
        std::memcpy(ptr, data, size);
        pbo_stream_->flush_and_unmap(offset, size);

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_stream_->get_id());
        glBindTexture(GL_TEXTURE_2D, glyph_atlas_tex_);
        
        // PBO offset used as the data pointer
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RED, GL_UNSIGNED_BYTE, (const void*)offset);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    // Bind buffer offset instead of using base-instance rendering
    void draw_instances(size_t instance_count, const InstanceData* data) override {
        size_t size = instance_count * sizeof(InstanceData);
        auto [ptr, offset] = instance_stream_->allocate_chunk(size);
        
        std::memcpy(ptr, data, size);
        instance_stream_->flush_and_unmap(offset, size);

        glBindBuffer(GL_ARRAY_BUFFER, instance_stream_->get_id());
        
        // Setup vertex attributes with the stream buffer offset. 
        // This is crucial for OpenGL ES 3.0 compatibility as it lacks glDrawArraysInstancedBaseInstance.
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(InstanceData), (const void*)(offset + offsetof(InstanceData, position)));
        glVertexAttribDivisor(1, 1); 

        // Draw instanced quads
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, instance_count);
    }
};

3. Unified Shader Architecture (assets/shaders/quad.glsl)

Since gleditor relies on a single source of truth for shaders across API backends, you can use preprocessor macros to handle the syntax differences between OpenGL 3.3 Core and OpenGL ES 3.0.

Because the CPU is now mapping memory via the stream buffer and providing offsets directly to the vertex attributes, the shader doesn't need SSBOs or complex buffer logic. It simply reads instanced attributes.
OpenGL Shading Language

// assets/shaders/quad.vert.glsl
#version 330 core
#ifdef GL_ES
precision highp float;
#endif

// Quad geometry (0 to 3)
layout(location = 0) in vec2 in_quad_vertex;

// Instanced attributes updated via GLStreamBuffer
layout(location = 1) in vec4 in_instance_pos_scale;
layout(location = 2) in vec4 in_glyph_uv_rect;
layout(location = 3) in vec4 in_anim_params; // xyz = color/dimension flags, w = alpha

out vec2 v_uv;
out vec4 v_color;

uniform mat4 u_view_proj;
uniform float u_current_time;

void main() {
    // 1. Unpack instance layout
    vec2 local_pos = in_quad_vertex * in_instance_pos_scale.zw;
    vec2 world_pos = in_instance_pos_scale.xy + local_pos;

    // 2. Map quad vertices to glyph atlas UVs
    // in_quad_vertex is assumed to be (0,0), (1,0), (0,1), (1,1)
    v_uv = in_glyph_uv_rect.xy + (in_quad_vertex * in_glyph_uv_rect.zw);
    
    // 3. Pass through dynamic alpha / dimension color
    v_color = vec4(in_anim_params.xyz, in_anim_params.w);

    gl_Position = u_view_proj * vec4(world_pos, 0.0, 1.0);
}

OpenGL Shading Language

// assets/shaders/quad.frag.glsl
#version 330 core
#ifdef GL_ES
precision highp float;
#endif

in vec2 v_uv;
in vec4 v_color;

out vec4 out_color;

uniform sampler2D u_glyph_atlas;

void main() {
    // Single-channel coverage read from the atlas (GL_RED)
    float coverage = texture(u_glyph_atlas, v_uv).r;
    
    if (coverage < 0.01) discard;

    out_color = vec4(v_color.rgb, v_color.a * coverage);
}


