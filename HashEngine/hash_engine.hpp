#ifndef HASH_ENGINE_HPP
#define HASH_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <utility>
#include <vector>

class HashEngine {
public:
    // CACHED uses buffered I/O. DIRECT opens every segment with O_DIRECT so
    // reads and writes bypass the page cache.
    enum CachingApproach {
        DIRECT,
        CACHED
    };

    HashEngine(const std::string& path, CachingApproach approach = CACHED);
    ~HashEngine();

    HashEngine(const HashEngine&) = delete;
    HashEngine& operator=(const HashEngine&) = delete;

    void insert(const std::string& key, const std::string& value);
    void remove(const std::string& key);
    std::optional<std::string> get(const std::string& key) const;
    std::vector<std::pair<std::string, std::string>> rangeQuery(
        const std::string& start_key,
        const std::string& end_key) const;

    void force_compact();

    size_t key_count() const;
    std::string& active_file_path();

private:
    struct RecordLocation {
        off_t offset = 0;
        std::string file_name;
    };

    struct Segment {
        off_t logical_end = 0;
        off_t file_bytes = 0;
    };

    struct CompactOutput {
        int fd = -1;
        uint64_t id = 0;
        off_t logical_end = 0;
        off_t file_bytes = 0;
    };

    struct IoBuffer {
        uint8_t* data = nullptr;
        std::size_t capacity = 0;

        IoBuffer() = default;
        ~IoBuffer();
        IoBuffer(const IoBuffer&) = delete;
        IoBuffer& operator=(const IoBuffer&) = delete;
    };

    CachingApproach caching_approach_ = CACHED;
    mutable int fd;
    std::unordered_map<std::string, RecordLocation> index;
    std::unordered_map<std::string, int> open_files_;
    std::unordered_map<std::string, Segment> segment_state_;
    std::string base_file_path;
    std::string active_file_path_;
    uint64_t current_file_index;
    mutable IoBuffer io_;

    bool direct() const { return caching_approach_ == DIRECT; }

    uint8_t* ensure_io(std::size_t bytes) const;
    int open_fd(const std::string& path, bool writable, bool trunc) const;
    void note_file(const std::string& path, int file_descriptor);
    const Segment& segment_for(const std::string& path) const;
    bool try_read_record(
        int file_descriptor,
        off_t offset,
        off_t limit,
        off_t file_bytes,
        std::string& key,
        std::string& value,
        bool& deleted,
        std::size_t& record_bytes) const;
    off_t append_bytes(
        int file_descriptor,
        off_t& logical_end,
        off_t& file_bytes,
        const std::vector<uint8_t>& bytes);
    void seal_output(
        CompactOutput& output,
        std::vector<std::pair<std::string, Segment>>& sealed);

    void open_log(const std::string& path);
    void close_log();
    void close_file(const std::string& path);
    void close_all_files();
    int fd_for(const std::string& path) const;
    void rebuild_index();
    void flush_to_disk();

    void rotate_active_file();
    void compact_persistent_storage();
    std::vector<std::string> old_file_paths() const;
    RecordLocation write_compacted_record(
        CompactOutput& output,
        std::vector<std::pair<std::string, std::string>>& temporary_files,
        std::vector<std::pair<std::string, Segment>>& sealed,
        const std::string& key,
        const std::string& value);

    std::string data_file_path(uint64_t file_id) const;
    std::vector<std::pair<uint64_t, std::string>> data_files() const;

    off_t current_offset() const;
    void seek_to(off_t offset) const;
    void seek_to_end();

    void write_bytes(const void* data, size_t size);
    bool try_read_bytes(void* data, size_t size) const;

    void write_u32(uint32_t value);
    void write_u8(uint8_t value);

    RecordLocation append_record(const std::string& key, const std::string& value, bool deleted);
    void read_record_at(const RecordLocation& location, std::string& key, std::string& value, bool& deleted) const;


    static constexpr uint64_t MAX_FILE_SIZE = 1024 * 1024 * 1000; // 64MB
    static constexpr uint64_t HEADER_SIZE = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t);

};

#endif
