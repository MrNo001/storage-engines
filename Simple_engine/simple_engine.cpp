#include "simple_engine.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::size_t kBlock = 4096;
constexpr std::size_t kHeader = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t);
constexpr uint32_t kEndMarker = 0xFFFFFFFFu;

void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed: " + std::strerror(errno));
}

off_t align_down(off_t value) {
    return value & ~static_cast<off_t>(kBlock - 1);
}

off_t align_up(off_t value) {
    return (value + static_cast<off_t>(kBlock - 1)) & ~static_cast<off_t>(kBlock - 1);
}

std::size_t pread_some(int fd, void* data, std::size_t size, off_t offset) {
    char* ptr = static_cast<char*>(data);
    std::size_t total = 0;

    while (size > 0) {
        const ssize_t n = ::pread(fd, ptr, size, offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("pread");
        }
        if (n == 0) {
            break;
        }
        ptr += n;
        size -= static_cast<std::size_t>(n);
        offset += n;
        total += static_cast<std::size_t>(n);
    }
    return total;
}

void pread_aligned(int fd, void* data, std::size_t size, off_t offset) {
    if (offset % static_cast<off_t>(kBlock) != 0 || size % kBlock != 0 ||
        reinterpret_cast<uintptr_t>(data) % kBlock != 0) {
        throw std::runtime_error("unaligned direct read");
    }

    char* ptr = static_cast<char*>(data);
    while (size > 0) {
        const ssize_t n = ::pread(fd, ptr, size, offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("pread");
        }
        if (n == 0) {
            throw std::runtime_error("pread failed: unexpected eof");
        }
        if (static_cast<std::size_t>(n) % kBlock != 0) {
            throw std::runtime_error("pread failed: short unaligned direct read");
        }
        ptr += n;
        size -= static_cast<std::size_t>(n);
        offset += n;
    }
}

void pwrite_all(int fd, const void* data, std::size_t size, off_t offset) {
    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t n = ::pwrite(fd, ptr, size, offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("pwrite");
        }
        if (n == 0) {
            throw std::runtime_error("pwrite failed: short write");
        }
        ptr += n;
        size -= static_cast<std::size_t>(n);
        offset += n;
    }
}

void pwrite_aligned(int fd, const void* data, std::size_t size, off_t offset) {
    if (offset % static_cast<off_t>(kBlock) != 0 || size % kBlock != 0 ||
        reinterpret_cast<uintptr_t>(data) % kBlock != 0) {
        throw std::runtime_error("unaligned direct write");
    }

    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t n = ::pwrite(fd, ptr, size, offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("pwrite");
        }
        if (n == 0 || static_cast<std::size_t>(n) % kBlock != 0) {
            throw std::runtime_error("pwrite failed: short unaligned direct write");
        }
        ptr += n;
        size -= static_cast<std::size_t>(n);
        offset += n;
    }
}

void fsync_parent_directory(const std::string& file_path) {
    std::string dir = ".";
    const auto slash = file_path.find_last_of('/');
    if (slash != std::string::npos) {
        dir = file_path.substr(0, slash);
        if (dir.empty()) {
            dir = "/";
        }
    }

    int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd < 0) {
        fail("open directory");
    }
    if (::fsync(dfd) < 0) {
        int saved = errno;
        ::close(dfd);
        errno = saved;
        fail("fsync directory");
    }
    ::close(dfd);
}

std::vector<uint8_t> encode_record(const std::string& key, const std::string& value, bool deleted) {
    const uint32_t key_len = static_cast<uint32_t>(key.size());
    const uint32_t val_len = static_cast<uint32_t>(value.size());
    const uint8_t deleted_flag = deleted ? 1 : 0;

    std::vector<uint8_t> bytes(kHeader + key.size() + value.size());
    std::memcpy(bytes.data(), &key_len, sizeof(key_len));
    std::memcpy(bytes.data() + sizeof(key_len), &val_len, sizeof(val_len));
    bytes[sizeof(key_len) + sizeof(val_len)] = deleted_flag;
    if (!key.empty()) {
        std::memcpy(bytes.data() + kHeader, key.data(), key.size());
    }
    if (!value.empty()) {
        std::memcpy(bytes.data() + kHeader + key.size(), value.data(), value.size());
    }
    return bytes;
}

}  // namespace

SimpleEngine::IoBuffer::~IoBuffer() {
    std::free(data);
}

uint8_t* SimpleEngine::ensure_io(std::size_t bytes) const {
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

SimpleEngine::SimpleEngine(const std::string& dir_path, CachingApproach approach)
    : _caching_approach(approach),
      path(dir_path) {
    int flags = O_RDWR | O_CREAT;
    if (direct()) {
        flags |= O_DIRECT;
    }

    fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        fail("open");
    }

    try {
        struct stat st {};
        if (::fstat(fd, &st) < 0) {
            fail("fstat");
        }
        file_bytes_ = st.st_size;
        if (direct()) {
            if (file_bytes_ % static_cast<off_t>(kBlock) != 0) {
                throw std::runtime_error("direct mode requires a block-aligned log file");
            }
            logical_end_ = recover_logical_end();
        } else {
            logical_end_ = file_bytes_;
        }
        fsync_parent_directory(path);
    } catch (...) {
        ::close(fd);
        fd = -1;
        throw;
    }
}

SimpleEngine::~SimpleEngine() {
    if (fd < 0) {
        return;
    }
    ::fdatasync(fd);
    ::close(fd);
    fd = -1;
}

void SimpleEngine::insert(const std::string& key, const std::string& value) {
    append_record(key, value, false);
}

void SimpleEngine::remove(const std::string& key) {
    append_record(key, "", true);
}

std::optional<std::string> SimpleEngine::get(const std::string& key) const {
    std::optional<std::string> latest;
    off_t offset = 0;
    while (offset < logical_end_) {
        std::string rec_key;
        std::string rec_value;
        bool deleted = false;
        std::size_t record_bytes = 0;
        if (!try_read_record(offset, logical_end_, rec_key, rec_value, deleted, record_bytes)) {
            break;
        }
        offset += static_cast<off_t>(record_bytes);

        if (rec_key != key) {
            continue;
        }
        if (deleted) {
            latest.reset();
        } else {
            latest = std::move(rec_value);
        }
    }
    return latest;
}

std::vector<std::pair<std::string, std::string>> SimpleEngine::rangeQuery(
    const std::string& start_key,
    const std::string& end_key) const {
    std::map<std::string, std::string> latest;
    off_t offset = 0;
    while (offset < logical_end_) {
        std::string rec_key;
        std::string rec_value;
        bool deleted = false;
        std::size_t record_bytes = 0;
        if (!try_read_record(offset, logical_end_, rec_key, rec_value, deleted, record_bytes)) {
            break;
        }
        offset += static_cast<off_t>(record_bytes);

        if (rec_key < start_key || rec_key > end_key) {
            continue;
        }
        if (deleted) {
            latest.erase(rec_key);
        } else {
            latest[rec_key] = std::move(rec_value);
        }
    }
    return std::vector<std::pair<std::string, std::string>>(latest.begin(), latest.end());
}

off_t SimpleEngine::recover_logical_end() const {
    off_t offset = 0;
    while (offset < file_bytes_) {
        std::string key;
        std::string value;
        bool deleted = false;
        std::size_t record_bytes = 0;
        if (!try_read_record(offset, file_bytes_, key, value, deleted, record_bytes)) {
            break;
        }
        offset += static_cast<off_t>(record_bytes);
    }
    return offset;
}

bool SimpleEngine::try_read_record(
    off_t offset,
    off_t limit,
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
        if (end > file_bytes_) {
            return false;
        }
        const std::size_t span = static_cast<std::size_t>(end - block_off);
        uint8_t* buf = ensure_io(span);
        pread_aligned(fd, buf, span, block_off);
        std::memcpy(window.data(), buf + skip, want);
    } else {
        const std::size_t got = pread_some(fd, window.data(), want, offset);
        if (got == 0) {
            return false;
        }
        window.resize(got);
    }

    if (window.size() < sizeof(uint32_t)) {
        return false;
    }

    uint32_t key_len = 0;
    std::memcpy(&key_len, window.data(), sizeof(key_len));
    if (key_len == kEndMarker) {
        return false;
    }
    if (window.size() < kHeader) {
        return false;
    }

    uint32_t val_len = 0;
    uint8_t deleted_flag = 0;
    std::memcpy(&val_len, window.data() + sizeof(key_len), sizeof(val_len));
    std::memcpy(&deleted_flag, window.data() + sizeof(key_len) + sizeof(val_len), sizeof(deleted_flag));

    const uint64_t total64 = static_cast<uint64_t>(kHeader) + key_len + val_len;
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
            if (end > file_bytes_) {
                return false;
            }
            const std::size_t span = static_cast<std::size_t>(end - block_off);
            uint8_t* buf = ensure_io(span);
            pread_aligned(fd, buf, span, block_off);
            std::memcpy(record.data() + window.size(), buf + skip, tail);
        } else if (pread_some(fd, record.data() + window.size(), tail, tail_off) != tail) {
            return false;
        }
        bytes = record.data();
    }

    key.assign(reinterpret_cast<const char*>(bytes + kHeader), key_len);
    value.assign(reinterpret_cast<const char*>(bytes + kHeader + key_len), val_len);
    deleted = deleted_flag != 0;
    record_bytes = total;
    return true;
}

void SimpleEngine::append_record(const std::string& key, const std::string& value, bool deleted) {
    if (key.size() >= kEndMarker || value.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("record key or value is too large");
    }

    const std::vector<uint8_t> bytes = encode_record(key, value, deleted);
    if (!direct()) {
        pwrite_all(fd, bytes.data(), bytes.size(), logical_end_);
        logical_end_ += static_cast<off_t>(bytes.size());
        file_bytes_ = logical_end_;
    } else {
        const off_t start = align_down(logical_end_);
        const std::size_t prefix = static_cast<std::size_t>(logical_end_ - start);
        const uint64_t payload = static_cast<uint64_t>(prefix) + bytes.size() + sizeof(kEndMarker);
        if (payload > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
            throw std::length_error("record is too large");
        }
        const std::size_t span = static_cast<std::size_t>(align_up(static_cast<off_t>(payload)));
        uint8_t* buf = ensure_io(span);
        std::memset(buf, 0, span);
        if (prefix > 0) {
            pread_aligned(fd, buf, kBlock, start);
        }
        std::memcpy(buf + prefix, bytes.data(), bytes.size());
        const uint32_t marker = kEndMarker;
        std::memcpy(buf + prefix + bytes.size(), &marker, sizeof(marker));
        pwrite_aligned(fd, buf, span, start);
        logical_end_ += static_cast<off_t>(bytes.size());
        file_bytes_ = start + static_cast<off_t>(span);
    }

    if (::fdatasync(fd) < 0) {
        fail("fdatasync");
    }
}
