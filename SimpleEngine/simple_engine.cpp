#include "simple_engine.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::size_t kHeader = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t);

void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed: " + std::strerror(errno));
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

SimpleEngine::SimpleEngine(const std::string& file_path)
    : path(file_path) {
    fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        fail("open");
    }

    try {
        struct stat st {};
        if (::fstat(fd, &st) < 0) {
            fail("fstat");
        }
        file_size_ = st.st_size;
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
    while (offset < file_size_) {
        std::string rec_key;
        std::string rec_value;
        bool deleted = false;
        std::size_t record_bytes = 0;
        if (!try_read_record(offset, rec_key, rec_value, deleted, record_bytes)) {
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
    while (offset < file_size_) {
        std::string rec_key;
        std::string rec_value;
        bool deleted = false;
        std::size_t record_bytes = 0;
        if (!try_read_record(offset, rec_key, rec_value, deleted, record_bytes)) {
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

bool SimpleEngine::try_read_record(
    off_t offset,
    std::string& key,
    std::string& value,
    bool& deleted,
    std::size_t& record_bytes) const {
    if (offset >= file_size_) {
        return false;
    }
    const uint64_t remaining = static_cast<uint64_t>(file_size_ - offset);

    uint8_t header[kHeader];
    if (remaining < kHeader || pread_some(fd, header, kHeader, offset) != kHeader) {
        return false;
    }

    uint32_t key_len = 0;
    uint32_t val_len = 0;
    std::memcpy(&key_len, header, sizeof(key_len));
    std::memcpy(&val_len, header + sizeof(key_len), sizeof(val_len));
    const uint8_t deleted_flag = header[sizeof(key_len) + sizeof(val_len)];

    const uint64_t total = static_cast<uint64_t>(kHeader) + key_len + val_len;
    if (total > remaining) {
        return false;
    }

    key.resize(key_len);
    value.resize(val_len);
    const off_t key_offset = offset + static_cast<off_t>(kHeader);
    const off_t value_offset = key_offset + static_cast<off_t>(key_len);
    if (pread_some(fd, key.data(), key.size(), key_offset) != key.size() ||
        pread_some(fd, value.data(), value.size(), value_offset) != value.size()) {
        return false;
    }

    deleted = deleted_flag != 0;
    record_bytes = static_cast<std::size_t>(total);
    return true;
}

void SimpleEngine::append_record(const std::string& key, const std::string& value, bool deleted) {
    if (key.size() > std::numeric_limits<uint32_t>::max() || value.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("record key or value is too large");
    }

    const std::vector<uint8_t> bytes = encode_record(key, value, deleted);
    pwrite_all(fd, bytes.data(), bytes.size(), file_size_);
    file_size_ += static_cast<off_t>(bytes.size());

    if (::fdatasync(fd) < 0) {
        fail("fdatasync");
    }
}
