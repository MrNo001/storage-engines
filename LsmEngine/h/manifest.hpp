#ifndef LSM_MANIFEST_HPP
#define LSM_MANIFEST_HPP

#include <cstdint>
#include <string>
#include <vector>

class Manifest {
public:
    Manifest(const std::string& path);

    Manifest(const Manifest&) = delete;
    Manifest& operator=(const Manifest&) = delete;

    std::vector<uint64_t> load() const;
    void persist(const std::vector<uint64_t>& table_ids);

private:
    std::string path_;
};

#endif
