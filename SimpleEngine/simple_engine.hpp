#ifndef SIMPLE_ENGINE_HPP
#define SIMPLE_ENGINE_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

// Append-only log: insert and remove append a record to the end of the file,
// get and rangeQuery read through the whole file and keep the latest record per key.
class SimpleEngine {
public:
     SimpleEngine(const std::string& file_path);
    ~SimpleEngine();

    SimpleEngine(const SimpleEngine&) = delete;
    SimpleEngine& operator=(const SimpleEngine&) = delete;

    void insert(const std::string& key, const std::string& value);
    void remove(const std::string& key);
    std::optional<std::string> get(const std::string& key) const;
    std::vector<std::pair<std::string, std::string>> rangeQuery(const std::string& start_key, const std::string& end_key) const;

private:
    bool try_read_record(
        off_t offset,
        std::string& key,
        std::string& value,
        bool& deleted,
        std::size_t& record_bytes) const;
    void append_record(const std::string& key, const std::string& value, bool deleted);

    int fd = -1;
    std::string path;
    off_t file_size_ = 0;
};

#endif
