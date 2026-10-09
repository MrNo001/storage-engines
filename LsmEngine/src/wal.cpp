#include "wal.hpp"
#include "utils.hpp"

#include <fcntl.h>
#include <unistd.h>

WAL::WAL(const std::string& path) : path_(path), fd_(-1) {}

WAL::~WAL() {
    close();
}

std::string& WAL::path() {
    return path_;
}

const std::string& WAL::path() const {
    return path_;
}

void WAL::open() {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        utils::fail("open");
    }
    utils::fsync_directory(utils::parent_directory(path_));
}

void WAL::close() {
    if (fd_ < 0) {
        return;
    }
    utils::durable_sync(fd_);
    ::close(fd_);
    fd_ = -1;
}

void WAL::replay(const std::function<void(const std::string& key, const std::string& value, bool deleted)>& handler) {
    utils::seek_to(fd_, 0);

    for (;;) {
        std::string key;
        std::string value;
        bool deleted = false;
        if (!utils::try_read_record(fd_, key, value, deleted)) {
            break;
        }
        handler(key, value, deleted);
    }
}

void WAL::truncate() {
    if (::ftruncate(fd_, 0) < 0) {
        utils::fail("ftruncate");
    }
    utils::seek_to(fd_, 0);
    if (utils::durable_sync(fd_) < 0) {
        utils::fail("fsync");
    }
}

void WAL::append(const std::string& key, const std::string& value, bool deleted) {
    utils::seek_to_end(fd_);
    utils::write_record(fd_, key, value, deleted);
    if (utils::durable_sync(fd_) < 0) {
        utils::fail("fsync");
    }
}
