#ifndef LSM_WAL_HPP
#define LSM_WAL_HPP

#include <functional>
#include <string>

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

#endif
