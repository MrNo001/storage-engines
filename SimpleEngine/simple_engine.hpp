#ifndef SIMPLE_ENGINE_HPP
#define SIMPLE_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

class SimpleEngine {
public:
    // CACHED uses buffered I/O. DIRECT opens the log with O_DIRECT so reads
    // and writes bypass the page cache.
    enum CachingApproach {
        DIRECT,
        CACHED
    };

    SimpleEngine(const std::string& dir_path, CachingApproach approach = CACHED);
    ~SimpleEngine();

    SimpleEngine(const SimpleEngine&) = delete;
    SimpleEngine& operator=(const SimpleEngine&) = delete;

    void insert(const std::string& key, const std::string& value);
    void remove(const std::string& key);
    std::optional<std::string> get(const std::string& key) const;
    std::vector<std::pair<std::string, std::string>> rangeQuery(const std::string& start_key, const std::string& end_key) const;

private:
    struct IoBuffer {
        uint8_t* data = nullptr;
        std::size_t capacity = 0;

        IoBuffer() = default;
        ~IoBuffer();
        IoBuffer(const IoBuffer&) = delete;
        IoBuffer& operator=(const IoBuffer&) = delete;
    };

    bool direct() const { return _caching_approach == DIRECT; }

    uint8_t* ensure_io(std::size_t bytes) const;
    off_t recover_logical_end() const;
    bool try_read_record(
        off_t offset,
        off_t limit,
        std::string& key,
        std::string& value,
        bool& deleted,
        std::size_t& record_bytes) const;
    void append_record(const std::string& key, const std::string& value, bool deleted);

    CachingApproach _caching_approach = CACHED;
    int fd = -1;
    std::string path;
    off_t logical_end_ = 0;
    off_t file_bytes_ = 0;
    mutable IoBuffer io_;
};

#endif
