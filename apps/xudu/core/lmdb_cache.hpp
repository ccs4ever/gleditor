#include <lmdb.h>
#include <string>
#include <filesystem>
#include <vector>
#include <stdexcept>
#include <cstring>
#include "spool.hpp" // for PrimediaSpan

namespace xudu {

struct SpanCacheKey {
    uint32_t scroll_id;
    uint32_t padding; // Ensure 8-byte alignment
    uint64_t start;
    uint64_t length;
};

class LMDBContentCache {
public:
    LMDBContentCache(const std::filesystem::path& cache_dir) {
        // Force unique cache for testing/ephemeral environments
        std::filesystem::path actual_dir = cache_dir;
        if (std::filesystem::exists(actual_dir)) {
            // Note: In a production ephemeral scenario, 
            // you might want to ensure this is cleaned up on exit.
            // For now, allow it to exist to support existing test structure.
        } else {
            std::filesystem::create_directories(actual_dir);
        }

        if (int rc = mdb_env_create(&env_); rc != MDB_SUCCESS) {
            throw std::runtime_error("mdb_env_create failed: " + std::string(mdb_strerror(rc)));
        }

        mdb_env_set_maxdbs(env_, 1);
        mdb_env_set_mapsize(env_, 1024ULL * 1024 * 1024); // 1GB
        
        unsigned int env_flags = MDB_NOTLS | MDB_NOSYNC | MDB_NOMETASYNC;

        if (int rc = mdb_env_open(env_, cache_dir.c_str(), env_flags, 0644); rc != MDB_SUCCESS) {
            mdb_env_close(env_);
            throw std::runtime_error("mdb_env_open failed: " + std::string(mdb_strerror(rc)));
        }

        MDB_txn* txn = nullptr;
        if (int rc = mdb_txn_begin(env_, nullptr, 0, &txn); rc == MDB_SUCCESS) {
            mdb_dbi_open(txn, "spans", MDB_CREATE, &dbi_);
            mdb_txn_commit(txn);
        }
    }

    ~LMDBContentCache() {
        if (env_) mdb_env_close(env_);
    }

    bool get(const PrimediaSpan& span, std::string& out) {
        MDB_txn* txn = nullptr;
        if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) return false;

        SpanCacheKey key{static_cast<uint32_t>(span.scroll), 0, span.start, span.length};
        MDB_val k{sizeof(key), &key};
        MDB_val v{};

        int rc = mdb_get(txn, dbi_, &k, &v);
        bool found = (rc == MDB_SUCCESS);
        if (found) {
            out.assign(static_cast<const char*>(v.mv_data), v.mv_size);
        }
        
        mdb_txn_abort(txn);
        return found;
    }

    void put(const PrimediaSpan& span, const std::string& text) {
        if (text.empty()) return; // Don't cache empty results
        MDB_txn* txn = nullptr;
        if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) return;

        SpanCacheKey key{static_cast<uint32_t>(span.scroll), 0, span.start, span.length};
        MDB_val k{sizeof(key), &key};
        
        MDB_val v{text.size(), const_cast<char*>(text.data())};
        mdb_put(txn, dbi_, &k, &v, 0);
        mdb_txn_commit(txn);
    }

private:
    MDB_env* env_{nullptr};
    MDB_dbi dbi_{0};
};

} // namespace xudu
