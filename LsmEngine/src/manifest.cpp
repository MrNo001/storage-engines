#include "manifest.hpp"
#include "utils.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

Manifest::Manifest(const std::string& path) : path_(path) {}

std::vector<uint64_t> Manifest::load() const {
    std::vector<uint64_t> table_ids;

    int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return table_ids;
        }
        utils::fail("open");
    }

    uint32_t count = 0;
    if (!utils::try_read_u32(fd, count)) {
        ::close(fd);
        return table_ids;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint64_t id = 0;
        if (!utils::try_read_u64(fd, id)) {
            ::close(fd);
            throw std::runtime_error("load_manifest failed: truncated manifest");
        }
        table_ids.push_back(id);
    }

    ::close(fd);
    return table_ids;
}

void Manifest::persist(const std::vector<uint64_t>& table_ids) {
    const std::string tmp_path = path_ + ".tmp";

    int fd = ::open(tmp_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        utils::fail("open");
    }

    utils::write_u32(fd, static_cast<uint32_t>(table_ids.size()));
    for (uint64_t id : table_ids) {
        utils::write_u64(fd, id);
    }

    if (utils::durable_sync(fd) < 0) {
        utils::fail("fsync");
    }
    ::close(fd);

    if (::rename(tmp_path.c_str(), path_.c_str()) < 0) {
        utils::fail("rename");
    }
    utils::fsync_directory(utils::parent_directory(path_));
}
