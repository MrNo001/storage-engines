#ifndef LSM_WAL_HPP
#define LSM_WAL_HPP

#include "utils.hpp"

#include <fcntl.h>
#include <functional>
#include <string>
#include <unistd.h>

// Write-ahead log: every insert/remove is appended and synced here before it reaches
// the memtable, so the memtable can be rebuilt with replay() after a restart.
class WAL {
public:
    WAL(const std::string& path);
    ~WAL();

    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    void open();
    void append(const std::string& key, const std::string& value, bool deleted);
    void truncate();
    void replay(const std::function<void(const std::string& key, const std::string& value, bool deleted)>& handler);

    std::string& path();
    const std::string& path() const;

private:
    void close();

    std::string path_;
    int fd_;
};

inline WAL::WAL(const std::string& path) : path_(path), fd_(-1) {}

inline WAL::~WAL() {
    close();
}

inline std::string& WAL::path() {
    return path_;
}

inline const std::string& WAL::path() const {
    return path_;
}

inline void WAL::open() {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        utils::fail("open");
    }
    utils::fsync_directory(utils::parent_directory(path_));
}

inline void WAL::close() {
    if (fd_ < 0) {
        return;
    }
    utils::durable_sync(fd_);
    ::close(fd_);
    fd_ = -1;
}

inline void WAL::replay(const std::function<void(const std::string& key, const std::string& value, bool deleted)>& handler) {
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

inline void WAL::truncate() {
    if (::ftruncate(fd_, 0) < 0) {
        utils::fail("ftruncate");
    }
    utils::seek_to(fd_, 0);
    if (utils::durable_sync(fd_) < 0) {
        utils::fail("fsync");
    }
}

inline void WAL::append(const std::string& key, const std::string& value, bool deleted) {
    utils::seek_to_end(fd_);
    utils::write_record(fd_, key, value, deleted);
    if (utils::durable_sync(fd_) < 0) {
        utils::fail("fsync");
    }
}

#endif
