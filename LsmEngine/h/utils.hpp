#ifndef LSM_UTILS_HPP
#define LSM_UTILS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>

namespace utils {

void fail(const char* operation);
int durable_sync(int fd);
void ensure_directory(const std::string& path);
std::string parent_directory(const std::string& path);
void fsync_directory(const std::string& path);

off_t current_offset(int fd);
void seek_to(int fd, off_t offset);
void seek_to_end(int fd);

void write_bytes(int fd, const void* data, size_t size);
bool try_read_bytes(int fd, void* data, size_t size);
bool try_pread_bytes(int fd, off_t offset, void* data, size_t size);

void write_u8(int fd, uint8_t value);
void write_u32(int fd, uint32_t value);
void write_u64(int fd, uint64_t value);

bool try_read_u8(int fd, uint8_t& value);
bool try_read_u32(int fd, uint32_t& value);
bool try_read_u64(int fd, uint64_t& value);

void write_record(int fd, const std::string& key, const std::string& value, bool deleted);
bool try_read_record(int fd, std::string& key, std::string& value, bool& deleted);

}  // namespace utils

#endif
