#ifndef LSM_ENGINE_HPP
#define LSM_ENGINE_HPP

#include "manifest.hpp"
#include "wal.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

class LsmEngine {
public:
    LsmEngine(const std::string& dir_path);
    ~LsmEngine();

    LsmEngine(const LsmEngine&) = delete;
    LsmEngine& operator=(const LsmEngine&) = delete;

    void insert(const std::string& key, const std::string& value);
    void remove(const std::string& key);
    std::optional<std::string> get(const std::string& key) const;
    std::vector<std::pair<std::string, std::string>> rangeQuery(const std::string& start_key, const std::string& end_key) const;

    void force_compact();

    size_t key_count();
    std::string& active_file_path();

private:
    struct Entry {
        std::string value;
        bool deleted = false;
    };

    struct Record {
        std::string key;
        std::string value;
        bool deleted = false;
    };

    struct SSTable {
        uint64_t id = 0;
        std::string path;
        mutable int fd = -1;
        off_t data_end = 0;
        std::vector<uint8_t> bloom_bits;
        uint32_t bloom_hash_count = 0;
        std::vector<std::pair<std::string, off_t>> sparse_index;
    };



    void insert_entry_memtable(const std::string& key, const std::string& value, bool deleted);
    void maybe_flush();
    void flush_memtable();

    void load_manifest();
    void persist_manifest();

    std::string sstable_path(uint64_t id) const;
    SSTable create_sstable(uint64_t id, const std::vector<Record>& records);
    SSTable open_sstable(uint64_t id) const;
    void close_sstable(SSTable& table) const;
    std::vector<Record> read_sstable_records(const SSTable& table) const;
    std::optional<Record> find_in_sstable(const SSTable& table, const std::string& key) const;

    void maybe_compact();
    void compact_tables();
    std::vector<Record> merge_tables() const;

    static constexpr size_t MEMTABLE_MAX_BYTES = 2 * 1024 * 1024; // 64KB
    static constexpr size_t COMPACTION_TRIGGER = 8;
    static constexpr size_t SPARSE_INTERVAL = 16;
    static constexpr size_t BLOOM_BITS_PER_KEY = 8;
    static constexpr uint32_t BLOOM_HASH_COUNT = 4;
    static constexpr uint32_t SST_MAGIC = 0x4C534D31;
    static constexpr size_t SST_FOOTER_SIZE = 32;

    std::string dir;
    WAL wal;
    Manifest manifest;
    std::map<std::string, Entry> memtable;
    size_t memtable_bytes;
    std::vector<SSTable> tables;
    uint64_t next_sst_id;
};

#endif


