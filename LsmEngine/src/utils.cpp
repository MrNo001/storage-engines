#include "utils.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace utils {

void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed: " + std::strerror(errno));
}

int durable_sync(int fd) {
    return ::fdatasync(fd);
}

void ensure_directory(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) < 0 && errno != EEXIST) {
        fail("mkdir");
    }
}

std::string parent_directory(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

void fsync_directory(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }
    durable_sync(fd);
    ::close(fd);
}

off_t current_offset(int fd) {
    off_t offset = ::lseek(fd, 0, SEEK_CUR);
    if (offset == static_cast<off_t>(-1)) {
        fail("lseek");
    }
    return offset;
}

void seek_to(int fd, off_t offset) {
    if (::lseek(fd, offset, SEEK_SET) == static_cast<off_t>(-1)) {
        fail("lseek");
    }
}

void seek_to_end(int fd) {
    if (::lseek(fd, 0, SEEK_END) == static_cast<off_t>(-1)) {
        fail("lseek");
    }
}

void write_bytes(int fd, const void* data, size_t size) {
    const char* ptr = static_cast<const char*>(data);
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t written = ::write(fd, ptr, remaining);
        if (written < 0) {
            fail("write");
        }
        ptr += written;
        remaining -= static_cast<size_t>(written);
    }
}

bool try_read_bytes(int fd, void* data, size_t size) {
    char* ptr = static_cast<char*>(data);
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t n = ::read(fd, ptr, remaining);
        if (n < 0) {
            fail("read");
        }
        if (n == 0) {
            return false;
        }
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

bool try_pread_bytes(int fd, off_t offset, void* data, size_t size) {
    char* ptr = static_cast<char*>(data);
    size_t remaining = size;
    off_t cursor = offset;

    while (remaining > 0) {
        ssize_t n = ::pread(fd, ptr, remaining, cursor);
        if (n < 0) {
            fail("pread");
        }
        if (n == 0) {
            return false;
        }
        ptr += n;
        remaining -= static_cast<size_t>(n);
        cursor += n;
    }
    return true;
}

void write_u8(int fd, uint8_t value) {
    write_bytes(fd, &value, sizeof(value));
}

void write_u32(int fd, uint32_t value) {
    write_bytes(fd, &value, sizeof(value));
}

void write_u64(int fd, uint64_t value) {
    write_bytes(fd, &value, sizeof(value));
}

bool try_read_u8(int fd, uint8_t& value) {
    return try_read_bytes(fd, &value, sizeof(value));
}

bool try_read_u32(int fd, uint32_t& value) {
    return try_read_bytes(fd, &value, sizeof(value));
}

bool try_read_u64(int fd, uint64_t& value) {
    return try_read_bytes(fd, &value, sizeof(value));
}

void write_record(int fd, const std::string& key, const std::string& value, bool deleted) {
    write_u32(fd, static_cast<uint32_t>(key.size()));
    write_u32(fd, static_cast<uint32_t>(value.size()));
    write_u8(fd, deleted ? 1 : 0);
    write_bytes(fd, key.data(), key.size());
    write_bytes(fd, value.data(), value.size());
}

bool try_read_record(int fd, std::string& key, std::string& value, bool& deleted) {
    uint32_t key_size = 0;
    uint32_t value_size = 0;
    uint8_t deleted_flag = 0;

    if (!try_read_u32(fd, key_size)) {
        return false;
    }
    if (!try_read_u32(fd, value_size)) {
        return false;
    }
    if (!try_read_u8(fd, deleted_flag)) {
        return false;
    }

    key.resize(key_size);
    if (key_size > 0 && !try_read_bytes(fd, key.data(), key_size)) {
        return false;
    }

    value.resize(value_size);
    if (value_size > 0 && !try_read_bytes(fd, value.data(), value_size)) {
        return false;
    }

    deleted = deleted_flag != 0;
    return true;
}

}  // namespace utils
