#include "hash_engine.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed: " + std::strerror(errno));
}

int durable_sync(int file_descriptor) {
    return fdatasync(file_descriptor);
}

void seek_to_fd(int file_descriptor, off_t offset) {
    if (::lseek(file_descriptor, offset, SEEK_SET) == static_cast<off_t>(-1)) {
        fail("lseek");
    }
}

off_t seek_to_end_fd(int file_descriptor) {
    off_t offset = ::lseek(file_descriptor, 0, SEEK_END);
    if (offset == static_cast<off_t>(-1)) {
        fail("lseek");
    }
    return offset;
}

void write_bytes_fd(int file_descriptor, const void* data, size_t size) {
    const char* ptr = static_cast<const char*>(data);
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t written = ::write(file_descriptor, ptr, remaining);
        if (written < 0) {
            fail("write");
        }
        ptr += written;
        remaining -= static_cast<size_t>(written);
    }
}

bool try_read_bytes_fd(int file_descriptor, void* data, size_t size) {
    char* ptr = static_cast<char*>(data);
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t count = ::read(file_descriptor, ptr, remaining);
        if (count < 0) {
            fail("read");
        }
        if (count == 0) {
            return false;
        }
        ptr += count;
        remaining -= static_cast<size_t>(count);
    }
    return true;
}

constexpr std::size_t kBlock = 4096;
constexpr std::size_t kHeader = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t);
constexpr uint32_t kEndMarker = 0xFFFFFFFFu;

off_t align_down(off_t value) {
    return value & ~static_cast<off_t>(kBlock - 1);
}

off_t align_up(off_t value) {
    return (value + static_cast<off_t>(kBlock - 1)) & ~static_cast<off_t>(kBlock - 1);
}

std::size_t pread_some(int file_descriptor, void* data, std::size_t size, off_t offset) {
    char* ptr = static_cast<char*>(data);
    std::size_t total = 0;

    while (size > 0) {
        const ssize_t count = ::pread(file_descriptor, ptr, size, offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("pread");
        }
        if (count == 0) {
            break;
        }
        ptr += count;
        size -= static_cast<std::size_t>(count);
        offset += count;
        total += static_cast<std::size_t>(count);
    }
    return total;
}

void pread_aligned(int file_descriptor, void* data, std::size_t size, off_t offset) {
    if (offset % static_cast<off_t>(kBlock) != 0 || size % kBlock != 0 ||
        reinterpret_cast<uintptr_t>(data) % kBlock != 0) {
        throw std::runtime_error("unaligned direct read");
    }

    char* ptr = static_cast<char*>(data);
    while (size > 0) {
        const ssize_t count = ::pread(file_descriptor, ptr, size, offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("pread");
        }
        if (count == 0) {
            throw std::runtime_error("pread failed: unexpected eof");
        }
        if (static_cast<std::size_t>(count) % kBlock != 0) {
            throw std::runtime_error("pread failed: short unaligned direct read");
        }
        ptr += count;
        size -= static_cast<std::size_t>(count);
        offset += count;
    }
}

void pwrite_all(int file_descriptor, const void* data, std::size_t size, off_t offset) {
    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t count = ::pwrite(file_descriptor, ptr, size, offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("pwrite");
        }
        if (count == 0) {
            throw std::runtime_error("pwrite failed: short write");
        }
        ptr += count;
        size -= static_cast<std::size_t>(count);
        offset += count;
    }
}

void pwrite_aligned(int file_descriptor, const void* data, std::size_t size, off_t offset) {
    if (offset % static_cast<off_t>(kBlock) != 0 || size % kBlock != 0 ||
        reinterpret_cast<uintptr_t>(data) % kBlock != 0) {
        throw std::runtime_error("unaligned direct write");
    }

    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t count = ::pwrite(file_descriptor, ptr, size, offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("pwrite");
        }
        if (count == 0 || static_cast<std::size_t>(count) % kBlock != 0) {
            throw std::runtime_error("pwrite failed: short unaligned direct write");
        }
        ptr += count;
        size -= static_cast<std::size_t>(count);
        offset += count;
    }
}

std::vector<uint8_t> encode_record(const std::string& key, const std::string& value, bool deleted) {
    const uint32_t key_size = static_cast<uint32_t>(key.size());
    const uint32_t value_size = static_cast<uint32_t>(value.size());
    const uint8_t deleted_flag = deleted ? 1 : 0;

    std::vector<uint8_t> bytes(kHeader + key.size() + value.size());
    std::memcpy(bytes.data(), &key_size, sizeof(key_size));
    std::memcpy(bytes.data() + sizeof(key_size), &value_size, sizeof(value_size));
    bytes[sizeof(key_size) + sizeof(value_size)] = deleted_flag;
    if (!key.empty()) {
        std::memcpy(bytes.data() + kHeader, key.data(), key.size());
    }
    if (!value.empty()) {
        std::memcpy(bytes.data() + kHeader + key.size(), value.data(), value.size());
    }
    return bytes;
}

}  // namespace

HashEngine::IoBuffer::~IoBuffer() {
    std::free(data);
}

uint8_t* HashEngine::ensure_io(std::size_t bytes) const {
    if (bytes <= io_.capacity) {
        return io_.data;
    }

    const std::size_t capacity = (bytes + kBlock - 1) & ~(kBlock - 1);
    void* raw = nullptr;
    const int rc = ::posix_memalign(&raw, kBlock, capacity);
    if (rc != 0) {
        throw std::runtime_error(std::string("posix_memalign failed: ") + std::strerror(rc));
    }
    std::free(io_.data);
    io_.data = static_cast<uint8_t*>(raw);
    io_.capacity = capacity;
    return io_.data;
}

int HashEngine::open_fd(const std::string& path, bool writable, bool trunc) const {
    int flags = writable ? (O_RDWR | O_CREAT) : O_RDONLY;
    if (trunc) {
        flags |= O_TRUNC;
    }
    if (direct()) {
        flags |= O_DIRECT;
    }

    const int opened = ::open(path.c_str(), flags, 0644);
    if (opened < 0) {
        fail("open");
    }
    return opened;
}

void HashEngine::note_file(const std::string& path, int file_descriptor) {
    struct stat st {};
    if (::fstat(file_descriptor, &st) < 0) {
        fail("fstat");
    }
    if (direct() && st.st_size % static_cast<off_t>(kBlock) != 0) {
        throw std::runtime_error("direct mode requires a block-aligned log file");
    }

    Segment& segment = segment_state_[path];
    segment.file_bytes = st.st_size;
    if (!direct()) {
        segment.logical_end = st.st_size;
    }
}

const HashEngine::Segment& HashEngine::segment_for(const std::string& path) const {
    const auto it = segment_state_.find(path);
    if (it == segment_state_.end()) {
        throw std::runtime_error("hash segment is not open: " + path);
    }
    return it->second;
}

HashEngine::HashEngine(const std::string& path, CachingApproach approach)
    : caching_approach_(approach),
      fd(-1),
      base_file_path(path),
      current_file_index(0) {

    std::vector<std::pair<uint64_t, std::string>> files = data_files();

    if (!files.empty()) {
        current_file_index = files.back().first;
    }

    active_file_path_ = data_file_path(current_file_index);
    try {
        for (const auto& file : files) {
            const bool writable = file.second == active_file_path_;
            const int opened = open_fd(file.second, writable, false);
            open_files_[file.second] = opened;
            note_file(file.second, opened);
            if (writable) {
                fd = opened;
            }
        }
        if (fd < 0) {
            open_log(active_file_path_);
        }
        rebuild_index();
    } catch (...) {
        for (auto& entry : open_files_) {
            if (entry.second >= 0) {
                ::close(entry.second);
            }
        }
        open_files_.clear();
        fd = -1;
        throw;
    }
}

HashEngine::~HashEngine() {
    close_all_files();
}

void HashEngine::insert(const std::string& key, const std::string& value) {
    RecordLocation location = append_record(key, value, false);
    index[key] = std::move(location);
    flush_to_disk();
}

void HashEngine::remove(const std::string& key) {
    if (index.find(key) == index.end()) {
        return;
    }

    append_record(key, "", true);
    index.erase(key);
    flush_to_disk();
}

std::optional<std::string> HashEngine::get(const std::string& key) const {
    auto it = index.find(key);
    if (it == index.end()) {
        return std::nullopt;
    }

    std::string stored_key;
    std::string value;
    bool deleted = false;
    read_record_at(it->second, stored_key, value, deleted);

    if (deleted || stored_key != key) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::pair<std::string, std::string>> HashEngine::rangeQuery(
    const std::string& start_key,
    const std::string& end_key) const {
    std::vector<std::pair<std::string, RecordLocation>> matches;
    for (const auto& entry : index) {
        if (entry.first >= start_key && entry.first <= end_key) {
            matches.emplace_back(entry.first, entry.second);
        }
    }

    std::sort(
        matches.begin(),
        matches.end(),
        [](const auto& left, const auto& right) {
            if (left.second.file_name != right.second.file_name) {
                return left.second.file_name < right.second.file_name;
            }
            return left.second.offset < right.second.offset;
        });

    std::vector<std::pair<std::string, std::string>> results;
    results.reserve(matches.size());

    for (const auto& match : matches) {
        std::string stored_key;
        std::string value;
        bool deleted = false;
        const Segment& segment = segment_for(match.second.file_name);
        std::size_t record_bytes = 0;
        if (!try_read_record(
                fd_for(match.second.file_name),
                match.second.offset,
                segment.logical_end,
                segment.file_bytes,
                stored_key,
                value,
                deleted,
                record_bytes)) {
            throw std::runtime_error("rangeQuery failed: truncated record");
        }
        if (deleted || stored_key != match.first) {
            continue;
        }
        results.emplace_back(match.first, std::move(value));
    }

    std::sort(results.begin(), results.end());
    return results;
}

void HashEngine::force_compact() {
    compact_persistent_storage();
}

size_t HashEngine::key_count() const {
    return index.size();
}

std::string& HashEngine::active_file_path() {
    return active_file_path_;
}

void HashEngine::open_log(const std::string& path) {
    auto it = open_files_.find(path);
    if (it != open_files_.end()) {
        fd = it->second;
        return;
    }

    fd = open_fd(path, true, false);
    open_files_[path] = fd;
    note_file(path, fd);
}

void HashEngine::close_file(const std::string& path) {
    auto it = open_files_.find(path);
    if (it == open_files_.end()) {
        return;
    }
    if (it->second == fd) {
        fd = -1;
    }
    if (it->second >= 0 && ::close(it->second) < 0) {
        open_files_.erase(it);
        segment_state_.erase(path);
        fail("close");
    }
    open_files_.erase(it);
    segment_state_.erase(path);
}

void HashEngine::close_all_files() {
    if (fd >= 0 && durable_sync(fd) < 0) {
        fail("fdatasync");
    }
    for (auto& entry : open_files_) {
        if (entry.second >= 0 && ::close(entry.second) < 0) {
            fd = -1;
            open_files_.clear();
            fail("close");
        }
        entry.second = -1;
    }
    open_files_.clear();
    fd = -1;
}

void HashEngine::close_log() {
    close_all_files();
}

int HashEngine::fd_for(const std::string& path) const {
    auto it = open_files_.find(path);
    if (it == open_files_.end() || it->second < 0) {
        throw std::runtime_error("hash segment is not open: " + path);
    }
    return it->second;
}

void HashEngine::rebuild_index() {
    index.clear();

    for (const auto& file : data_files()) {
        const int source_fd = fd_for(file.second);
        const auto segment_it = segment_state_.find(file.second);
        if (segment_it == segment_state_.end()) {
            throw std::runtime_error("hash segment is not open: " + file.second);
        }
        Segment& segment = segment_it->second;
        const off_t limit = segment.file_bytes;
        off_t offset = 0;
        while (offset < limit) {
            std::string key;
            std::string value;
            bool deleted = false;
            std::size_t record_bytes = 0;
            if (!try_read_record(
                    source_fd,
                    offset,
                    limit,
                    segment.file_bytes,
                    key,
                    value,
                    deleted,
                    record_bytes)) {
                break;
            }

            if (deleted) {
                index.erase(key);
            } else {
                index[key] = RecordLocation{offset, file.second};
            }
            offset += static_cast<off_t>(record_bytes);
        }
        if (direct()) {
            segment.logical_end = offset;
        }
    }
}

void HashEngine::flush_to_disk() {
    if (durable_sync(fd) < 0) {
        fail("fdatasync");
    }
}

void HashEngine::rotate_active_file() {
    flush_to_disk();
    ++current_file_index;
    active_file_path_ = data_file_path(current_file_index);
    open_log(active_file_path_);
}

void HashEngine::compact_persistent_storage() {
    flush_to_disk();

    std::vector<std::string> old_files = old_file_paths();
    if (old_files.empty()) {
        return;
    }

    std::vector<std::pair<std::string, std::string>> temporary_files;
    std::vector<std::pair<std::string, Segment>> sealed;
    std::vector<std::pair<std::string, RecordLocation>> relocated;
    CompactOutput output;

    try {
        for (const std::string& path : old_files) {
            const int source_fd = fd_for(path);
            const Segment segment = segment_for(path);
            off_t offset = 0;
            while (offset < segment.logical_end) {
                std::string key;
                std::string value;
                bool deleted = false;
                std::size_t record_bytes = 0;
                if (!try_read_record(
                        source_fd,
                        offset,
                        segment.logical_end,
                        segment.file_bytes,
                        key,
                        value,
                        deleted,
                        record_bytes)) {
                    break;
                }

                auto it = index.find(key);
                if (deleted ||
                    it == index.end() ||
                    it->second.file_name != path ||
                    it->second.offset != offset) {
                    offset += static_cast<off_t>(record_bytes);
                    continue;
                }

                RecordLocation new_location = write_compacted_record(
                    output,
                    temporary_files,
                    sealed,
                    key,
                    value);
                relocated.push_back({std::move(key), std::move(new_location)});
                offset += static_cast<off_t>(record_bytes);
            }
        }
        seal_output(output, sealed);
    } catch (...) {
        if (output.fd >= 0) {
            ::close(output.fd);
        }
        for (const auto& file : temporary_files) {
            ::unlink(file.first.c_str());
        }
        throw;
    }

    for (const std::string& path : old_files) {
        close_file(path);
        if (::unlink(path.c_str()) < 0 && errno != ENOENT) {
            fail("unlink");
        }
    }
    for (const auto& file : temporary_files) {
        if (::rename(file.first.c_str(), file.second.c_str()) < 0) {
            fail("rename");
        }
        const int opened = open_fd(file.second, false, false);
        open_files_[file.second] = opened;
    }
    for (const auto& segment : sealed) {
        segment_state_[segment.first] = segment.second;
    }
    for (auto& entry : relocated) {
        index[entry.first] = std::move(entry.second);
    }
}

std::vector<std::string> HashEngine::old_file_paths() const {
    std::vector<std::string> paths;
    for (const auto& file : data_files()) {
        if (file.first < current_file_index) {
            paths.push_back(file.second);
        }
    }
    return paths;
}

void HashEngine::seal_output(
    CompactOutput& output,
    std::vector<std::pair<std::string, Segment>>& sealed) {
    if (output.fd < 0) {
        return;
    }
    if (durable_sync(output.fd) < 0) {
        fail("fdatasync");
    }
    const int to_close = output.fd;
    output.fd = -1;
    if (::close(to_close) < 0) {
        fail("close");
    }
    sealed.push_back({
        data_file_path(output.id),
        Segment{output.logical_end, output.file_bytes}});
}

HashEngine::RecordLocation HashEngine::write_compacted_record(
    CompactOutput& output,
    std::vector<std::pair<std::string, std::string>>& temporary_files,
    std::vector<std::pair<std::string, Segment>>& sealed,
    const std::string& key,
    const std::string& value) {
    const uint64_t record_size =
        HEADER_SIZE + static_cast<uint64_t>(key.size()) +
        static_cast<uint64_t>(value.size());
    if (output.fd >= 0 &&
        output.logical_end > 0 &&
        static_cast<uint64_t>(output.logical_end) + record_size > MAX_FILE_SIZE) {
        seal_output(output, sealed);
        ++output.id;
        output.logical_end = 0;
        output.file_bytes = 0;
    }
    if (output.fd < 0) {
        const std::string final_path = data_file_path(output.id);
        const std::string temporary_path = final_path + ".compact.tmp";
        ::unlink(temporary_path.c_str());
        output.fd = open_fd(temporary_path, true, true);
        temporary_files.push_back({temporary_path, final_path});
    }

    const std::vector<uint8_t> bytes = encode_record(key, value, false);
    const off_t new_offset = append_bytes(
        output.fd,
        output.logical_end,
        output.file_bytes,
        bytes);
    return RecordLocation{new_offset, data_file_path(output.id)};
}

std::string HashEngine::data_file_path(uint64_t file_id) const {
    return base_file_path + "." + std::to_string(file_id);
}

std::vector<std::pair<uint64_t, std::string>> HashEngine::data_files() const {
    namespace fs = std::filesystem;

    fs::path base(base_file_path);
    fs::path directory = base.parent_path();
    if (directory.empty()) {
        directory = ".";
    }
    const std::string prefix = base.filename().string() + ".";

    std::vector<std::pair<uint64_t, std::string>> files;
    if (!fs::exists(directory)) {
        return files;
    }

    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        std::string name = entry.path().filename().string();
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }

        std::string suffix = name.substr(prefix.size());
        if (!std::all_of(
                suffix.begin(),
                suffix.end(),
                [](unsigned char character) { return std::isdigit(character) != 0; })) {
            continue;
        }

        try {
            unsigned long long id = std::stoull(suffix);
            if (id <= std::numeric_limits<uint64_t>::max()) {
                files.push_back({
                    static_cast<uint64_t>(id),
                    data_file_path(static_cast<uint64_t>(id))});
            }
        } catch (const std::exception&) {
            continue;
        }
    }

    std::sort(
        files.begin(),
        files.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
    return files;
}

off_t HashEngine::current_offset() const {
    off_t offset = ::lseek(fd, 0, SEEK_CUR);
    if (offset == static_cast<off_t>(-1)) {
        fail("lseek");
    }
    return offset;
}

void HashEngine::seek_to(off_t offset) const {
    seek_to_fd(fd, offset);
}

void HashEngine::seek_to_end() {
    seek_to_end_fd(fd);
}

void HashEngine::write_bytes(const void* data, size_t size) {
    write_bytes_fd(fd, data, size);
}

bool HashEngine::try_read_bytes(void* data, size_t size) const {
    return try_read_bytes_fd(fd, data, size);
}

void HashEngine::write_u32(uint32_t value) {
    write_bytes(&value, sizeof(value));
}

void HashEngine::write_u8(uint8_t value) {
    write_bytes(&value, sizeof(value));
}

HashEngine::RecordLocation HashEngine::append_record(
    const std::string& key,
    const std::string& value,
    bool deleted) {
    static_assert(HEADER_SIZE == kHeader);

    if (key.size() >= kEndMarker || value.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("record key or value is too large");
    }

    const uint64_t record_size =
        HEADER_SIZE + static_cast<uint64_t>(key.size()) +
        static_cast<uint64_t>(value.size());
    if (segment_for(active_file_path_).logical_end > 0 &&
        static_cast<uint64_t>(segment_for(active_file_path_).logical_end) + record_size > MAX_FILE_SIZE) {
        rotate_active_file();
    }

    Segment& segment = segment_state_[active_file_path_];
    const std::vector<uint8_t> bytes = encode_record(key, value, deleted);
    const off_t offset = append_bytes(fd, segment.logical_end, segment.file_bytes, bytes);
    return RecordLocation{offset, active_file_path_};
}

void HashEngine::read_record_at(
    const RecordLocation& location,
    std::string& key,
    std::string& value,
    bool& deleted) const {
    const Segment& segment = segment_for(location.file_name);
    std::size_t record_bytes = 0;
    if (!try_read_record(
            fd_for(location.file_name),
            location.offset,
            segment.logical_end,
            segment.file_bytes,
            key,
            value,
            deleted,
            record_bytes)) {
        throw std::runtime_error("read_record_at failed: truncated record");
    }
}

bool HashEngine::try_read_record(
    int file_descriptor,
    off_t offset,
    off_t limit,
    off_t file_bytes,
    std::string& key,
    std::string& value,
    bool& deleted,
    std::size_t& record_bytes) const {
    if (offset >= limit) {
        return false;
    }

    const uint64_t remaining = static_cast<uint64_t>(limit - offset);
    if (remaining < sizeof(uint32_t)) {
        return false;
    }

    const std::size_t want = remaining < kBlock ? static_cast<std::size_t>(remaining) : kBlock;
    std::vector<uint8_t> window(want);

    if (direct()) {
        const off_t block_off = align_down(offset);
        const std::size_t skip = static_cast<std::size_t>(offset - block_off);
        const off_t end = align_up(offset + static_cast<off_t>(want));
        if (end > file_bytes) {
            return false;
        }
        const std::size_t span = static_cast<std::size_t>(end - block_off);
        uint8_t* buf = ensure_io(span);
        pread_aligned(file_descriptor, buf, span, block_off);
        std::memcpy(window.data(), buf + skip, want);
    } else {
        const std::size_t got = pread_some(file_descriptor, window.data(), want, offset);
        if (got == 0) {
            return false;
        }
        window.resize(got);
    }

    if (window.size() < sizeof(uint32_t)) {
        return false;
    }

    uint32_t key_size = 0;
    std::memcpy(&key_size, window.data(), sizeof(key_size));
    if (key_size == kEndMarker) {
        return false;
    }
    if (window.size() < kHeader) {
        return false;
    }

    uint32_t value_size = 0;
    uint8_t deleted_flag = 0;
    std::memcpy(&value_size, window.data() + sizeof(key_size), sizeof(value_size));
    std::memcpy(
        &deleted_flag,
        window.data() + sizeof(key_size) + sizeof(value_size),
        sizeof(deleted_flag));

    const uint64_t total64 = static_cast<uint64_t>(kHeader) + key_size + value_size;
    if (total64 > remaining || total64 > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    const std::size_t total = static_cast<std::size_t>(total64);

    const uint8_t* bytes = window.data();
    std::vector<uint8_t> record;
    if (window.size() < total) {
        record.resize(total);
        std::memcpy(record.data(), window.data(), window.size());
        const std::size_t tail = total - window.size();
        const off_t tail_off = offset + static_cast<off_t>(window.size());
        if (direct()) {
            const off_t block_off = align_down(tail_off);
            const std::size_t skip = static_cast<std::size_t>(tail_off - block_off);
            const off_t end = align_up(tail_off + static_cast<off_t>(tail));
            if (end > file_bytes) {
                return false;
            }
            const std::size_t span = static_cast<std::size_t>(end - block_off);
            uint8_t* buf = ensure_io(span);
            pread_aligned(file_descriptor, buf, span, block_off);
            std::memcpy(record.data() + window.size(), buf + skip, tail);
        } else if (pread_some(file_descriptor, record.data() + window.size(), tail, tail_off) != tail) {
            return false;
        }
        bytes = record.data();
    }

    key.assign(reinterpret_cast<const char*>(bytes + kHeader), key_size);
    value.assign(reinterpret_cast<const char*>(bytes + kHeader + key_size), value_size);
    deleted = deleted_flag != 0;
    record_bytes = total;
    return true;
}

off_t HashEngine::append_bytes(
    int file_descriptor,
    off_t& logical_end,
    off_t& file_bytes,
    const std::vector<uint8_t>& bytes) {
    const off_t offset = logical_end;
    if (!direct()) {
        pwrite_all(file_descriptor, bytes.data(), bytes.size(), logical_end);
        logical_end += static_cast<off_t>(bytes.size());
        file_bytes = logical_end;
        return offset;
    }

    const off_t start = align_down(logical_end);
    const std::size_t prefix = static_cast<std::size_t>(logical_end - start);
    const uint64_t payload = static_cast<uint64_t>(prefix) + bytes.size() + sizeof(kEndMarker);
    if (payload > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        throw std::length_error("record is too large");
    }
    const std::size_t span = static_cast<std::size_t>(align_up(static_cast<off_t>(payload)));
    uint8_t* buf = ensure_io(span);
    std::memset(buf, 0, span);
    if (prefix > 0) {
        pread_aligned(file_descriptor, buf, kBlock, start);
    }
    std::memcpy(buf + prefix, bytes.data(), bytes.size());
    const uint32_t marker = kEndMarker;
    std::memcpy(buf + prefix + bytes.size(), &marker, sizeof(marker));
    pwrite_aligned(file_descriptor, buf, span, start);
    logical_end += static_cast<off_t>(bytes.size());
    file_bytes = start + static_cast<off_t>(span);
    return offset;
}
