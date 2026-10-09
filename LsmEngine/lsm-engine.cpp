#include "lsm-engine.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unordered_set>
#include <unistd.h>
#include <sys/stat.h>

namespace {

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

constexpr size_t SST_READ_BLOCK = 4096;

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

struct SstBlockReader {
    int fd = -1;
    off_t cursor = 0;
    off_t data_end = 0;
    std::vector<char> buf;
    off_t buf_file = 0;
    size_t buf_len = 0;

    SstBlockReader(int file, off_t start, off_t end)
        : fd(file), cursor(start), data_end(end) {}

    bool ensure(size_t n) {
        if (n == 0) {
            return true;
        }
        if (cursor < 0 || cursor > data_end) {
            return false;
        }
        if (data_end - cursor < static_cast<off_t>(n)) {
            return false;
        }

        size_t have = 0;
        if (cursor >= buf_file && cursor < buf_file + static_cast<off_t>(buf_len)) {
            have = buf_len - static_cast<size_t>(cursor - buf_file);
            if (have >= n) {
                return true;
            }
            if (have > 0) {
                std::memmove(buf.data(), buf.data() + (cursor - buf_file), have);
            }
            buf_file = cursor;
            buf_len = have;
        } else {
            buf_file = cursor;
            buf_len = 0;
            have = 0;
        }

        size_t want = std::max(n, SST_READ_BLOCK);
        const off_t remaining_file = data_end - cursor;
        if (static_cast<off_t>(want) > remaining_file) {
            want = static_cast<size_t>(remaining_file);
        }
        if (want < n) {
            return false;
        }
        if (buf.size() < want) {
            buf.resize(want);
        }

        const size_t to_read = want - have;
        if (to_read > 0 &&
            !try_pread_bytes(fd, cursor + static_cast<off_t>(have), buf.data() + have, to_read)) {
            return false;
        }
        buf_len = have + to_read;
        return true;
    }

    bool read_bytes(void* dest, size_t n) {
        if (!ensure(n)) {
            return false;
        }
        std::memcpy(dest, buf.data() + (cursor - buf_file), n);
        cursor += static_cast<off_t>(n);
        return true;
    }

    bool next_record(std::string& key, std::string& value, bool& deleted) {
        uint32_t key_size = 0;
        uint32_t value_size = 0;
        uint8_t deleted_flag = 0;
        if (!read_bytes(&key_size, sizeof(key_size)) ||
            !read_bytes(&value_size, sizeof(value_size)) ||
            !read_bytes(&deleted_flag, sizeof(deleted_flag))) {
            return false;
        }

        const uint64_t payload =
            static_cast<uint64_t>(key_size) + static_cast<uint64_t>(value_size);
        if (payload > static_cast<uint64_t>(data_end - cursor)) {
            return false;
        }

        key.resize(key_size);
        if (key_size > 0 && !read_bytes(key.data(), key_size)) {
            return false;
        }
        value.resize(value_size);
        if (value_size > 0 && !read_bytes(value.data(), value_size)) {
            return false;
        }
        deleted = deleted_flag != 0;
        return true;
    }
};

uint64_t fnv1a64(const std::string& key, uint64_t seed) {
    uint64_t hash = 14695981039346656037ull ^ seed;
    for (unsigned char byte : key) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

void bloom_add(std::vector<uint8_t>& bits, uint32_t hash_count, const std::string& key) {
    const size_t nbits = bits.size() * 8;
    if (nbits == 0) {
        return;
    }

    uint64_t h1 = fnv1a64(key, 0);
    uint64_t h2 = fnv1a64(key, 0x9e3779b97f4a7c15ull);
    if (h2 == 0) {
        h2 = 1;
    }

    for (uint32_t i = 0; i < hash_count; i++) {
        size_t bit = static_cast<size_t>((h1 + static_cast<uint64_t>(i) * h2) % nbits);
        bits[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    }
}

off_t sparse_seek_offset(
    const std::vector<std::pair<std::string, off_t>>& index,
    const std::string& key) {
    off_t start = 0;
    auto it = std::upper_bound(
        index.begin(),
        index.end(),
        key,
        [](const std::string& needle, const std::pair<std::string, off_t>& entry) {
            return needle < entry.first;
        });
    if (it != index.begin()) {
        --it;
        start = it->second;
    }
    return start;
}

bool bloom_maybe_contains(const std::vector<uint8_t>& bits, uint32_t hash_count, const std::string& key) {
    const size_t nbits = bits.size() * 8;
    if (nbits == 0) {
        return false;
    }

    uint64_t h1 = fnv1a64(key, 0);
    uint64_t h2 = fnv1a64(key, 0x9e3779b97f4a7c15ull);
    if (h2 == 0) {
        h2 = 1;
    }

    for (uint32_t i = 0; i < hash_count; i++) {
        size_t bit = static_cast<size_t>((h1 + static_cast<uint64_t>(i) * h2) % nbits);
        if ((bits[bit / 8] & static_cast<uint8_t>(1u << (bit % 8))) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

LsmEngine::LsmEngine(const std::string& dir_path)
    : dir(dir_path),
      wal(dir_path + "/wal.log"),
      memtable_bytes(0),
      next_sst_id(1) {
    ensure_directory(dir);
    fsync_directory(dir);
    wal.open();
    load_manifest();
    wal.replay([this](const std::string& key, const std::string& value, bool deleted) {
        insert_entry_memtable(key, value, deleted);
    });
    maybe_flush();
}

LsmEngine::~LsmEngine() {
    for (SSTable& table : tables) {
        close_sstable(table);
    }
}

void LsmEngine::insert(const std::string& key, const std::string& value) {
    wal.append(key, value, false);
    insert_entry_memtable(key, value, false);
    maybe_flush();
}

void LsmEngine::remove(const std::string& key) {
    wal.append(key, "", true);
    insert_entry_memtable(key, "", true);
    maybe_flush();
}

std::optional<std::string> LsmEngine::get(const std::string& key) const {
    auto it = memtable.find(key);
    if (it != memtable.end()) {
        if (it->second.deleted) {
            return std::nullopt;
        }
        return it->second.value;
    }

    for (auto table = tables.rbegin(); table != tables.rend(); ++table) {
        std::optional<Record> rec = find_in_sstable(*table, key);
        if (!rec.has_value()) {
            continue;
        }
        if (rec->deleted) {
            return std::nullopt;
        }
        return rec->value;
    }

    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> LsmEngine::rangeQuery(
    const std::string& start_key,
    const std::string& end_key) const {
    std::unordered_set<std::string> seen;
    std::map<std::string, std::string> live;

    auto consider = [&](const std::string& key, const std::string& value, bool deleted) {
        if (key < start_key || key > end_key) {
            return;
        }
        if (!seen.insert(key).second) {
            return;
        }
        if (!deleted) {
            live.emplace(key, value);
        }
    };

    for (auto it = memtable.lower_bound(start_key); it != memtable.end(); ++it) {
        if (it->first > end_key) {
            break;
        }
        consider(it->first, it->second.value, it->second.deleted);
    }

    for (auto table = tables.rbegin(); table != tables.rend(); ++table) {
        SstBlockReader reader(
            table->fd,
            sparse_seek_offset(table->sparse_index, start_key),
            table->data_end);
        std::string key;
        std::string value;
        bool deleted = false;
        while (reader.next_record(key, value, deleted)) {
            if (key < start_key) {
                continue;
            }
            if (key > end_key) {
                break;
            }
            consider(key, value, deleted);
        }
    }

    return std::vector<std::pair<std::string, std::string>>(live.begin(), live.end());
}

void LsmEngine::force_compact() {
    flush_memtable();
    compact_tables();
}

size_t LsmEngine::key_count() {
    std::unordered_set<std::string> seen;
    size_t live = 0;

    auto consider = [&](const std::string& key, bool deleted) {
        if (!seen.insert(key).second) {
            return;
        }
        if (!deleted) {
            live++;
        }
    };

    for (const auto& entry : memtable) {
        consider(entry.first, entry.second.deleted);
    }

    for (auto table = tables.rbegin(); table != tables.rend(); ++table) {
        std::vector<Record> records = read_sstable_records(*table);
        for (const Record& rec : records) {
            consider(rec.key, rec.deleted);
        }
    }

    return live;
}

std::string& LsmEngine::active_file_path() {
    return wal.path();
}

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
        fail("open");
    }

    auto slash = path_.find_last_of('/');
    if (slash != std::string::npos) {
        fsync_directory(path_.substr(0, slash));
    }
}

void WAL::close() {
    if (fd_ < 0) {
        return;
    }
    durable_sync(fd_);
    ::close(fd_);
    fd_ = -1;
}

void WAL::replay(const std::function<void(const std::string& key, const std::string& value, bool deleted)>& handler) {
    seek_to(fd_, 0);

    for (;;) {
        std::string key;
        std::string value;
        bool deleted = false;
        if (!try_read_record(fd_, key, value, deleted)) {
            break;
        }
        handler(key, value, deleted);
    }
}

void WAL::truncate() {
    if (::ftruncate(fd_, 0) < 0) {
        fail("ftruncate");
    }
    seek_to(fd_, 0);
    if (durable_sync(fd_) < 0) {
        fail("fsync");
    }
}

void WAL::append(const std::string& key, const std::string& value, bool deleted) {
    seek_to_end(fd_);
    write_record(fd_, key, value, deleted);
    if (durable_sync(fd_) < 0) {
        fail("fsync");
    }
}

void LsmEngine::insert_entry_memtable(const std::string& key, const std::string& value, bool deleted) {
    auto it = memtable.find(key);
    if (it != memtable.end()) {
        memtable_bytes -= it->first.size() + it->second.value.size();
        it->second = Entry{value, deleted};
    } else {
        memtable.emplace(key, Entry{value, deleted});
    }
    memtable_bytes += key.size() + value.size();
}

void LsmEngine::maybe_flush() {
    if (memtable_bytes >= MEMTABLE_MAX_BYTES) {
        flush_memtable();
        maybe_compact();
    }
}

void LsmEngine::flush_memtable() {
    if (memtable.empty()) {
        return;
    }

    std::vector<Record> records;
    records.reserve(memtable.size());
    for (const auto& entry : memtable) {
        records.push_back(Record{entry.first, entry.second.value, entry.second.deleted});
    }

    uint64_t id = next_sst_id++;
    tables.push_back(create_sstable(id, records));
    fsync_directory(dir);
    persist_manifest();
    wal.truncate();

    memtable.clear();
    memtable_bytes = 0;
}

void LsmEngine::load_manifest() {
    std::string path = dir + "/manifest";
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return;
        }
        fail("open");
    }

    uint32_t count = 0;
    if (!try_read_u32(fd, count)) {
        ::close(fd);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint64_t id = 0;
        if (!try_read_u64(fd, id)) {
            ::close(fd);
            throw std::runtime_error("load_manifest failed: truncated manifest");
        }
        tables.push_back(open_sstable(id));
        if (id + 1 > next_sst_id) {
            next_sst_id = id + 1;
        }
    }

    ::close(fd);
}

void LsmEngine::persist_manifest() {
    std::string tmp_path = dir + "/manifest.tmp";
    std::string path = dir + "/manifest";

    int fd = ::open(tmp_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("open");
    }

    write_u32(fd, static_cast<uint32_t>(tables.size()));
    for (const SSTable& table : tables) {
        write_u64(fd, table.id);
    }

    if (durable_sync(fd) < 0) {
        fail("fsync");
    }
    ::close(fd);

    if (::rename(tmp_path.c_str(), path.c_str()) < 0) {
        fail("rename");
    }
    fsync_directory(dir);
}

std::string LsmEngine::sstable_path(uint64_t id) const {
    char name[32];
    std::snprintf(name, sizeof(name), "sst-%06llu.sst", static_cast<unsigned long long>(id));
    return dir + "/" + name;
}

LsmEngine::SSTable LsmEngine::create_sstable(uint64_t id, const std::vector<Record>& records) {
    SSTable table;
    table.id = id;
    table.path = sstable_path(id);

    table.fd = ::open(table.path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (table.fd < 0) {
        fail("open");
    }

    size_t nbits = std::max<size_t>(64, records.size() * BLOOM_BITS_PER_KEY);
    table.bloom_bits.assign((nbits + 7) / 8, 0);
    table.bloom_hash_count = BLOOM_HASH_COUNT;

    for (size_t i = 0; i < records.size(); i++) {
        off_t offset = current_offset(table.fd);
        if (i % SPARSE_INTERVAL == 0) {
            table.sparse_index.push_back({records[i].key, offset});
        }
        bloom_add(table.bloom_bits, table.bloom_hash_count, records[i].key);
        write_record(table.fd, records[i].key, records[i].value, records[i].deleted);
    }

    table.data_end = current_offset(table.fd);

    off_t bloom_offset = table.data_end;
    write_u32(table.fd, table.bloom_hash_count);
    write_u32(table.fd, static_cast<uint32_t>(table.bloom_bits.size()));
    write_bytes(table.fd, table.bloom_bits.data(), table.bloom_bits.size());

    off_t index_offset = current_offset(table.fd);
    write_u32(table.fd, static_cast<uint32_t>(table.sparse_index.size()));
    for (const auto& entry : table.sparse_index) {
        write_u32(table.fd, static_cast<uint32_t>(entry.first.size()));
        write_u64(table.fd, static_cast<uint64_t>(entry.second));
        write_bytes(table.fd, entry.first.data(), entry.first.size());
    }

    write_u64(table.fd, static_cast<uint64_t>(bloom_offset));
    write_u64(table.fd, static_cast<uint64_t>(index_offset));
    write_u64(table.fd, static_cast<uint64_t>(table.data_end));
    write_u32(table.fd, static_cast<uint32_t>(records.size()));
    write_u32(table.fd, SST_MAGIC);

    if (durable_sync(table.fd) < 0) {
        fail("fsync");
    }
    return table;
}

LsmEngine::SSTable LsmEngine::open_sstable(uint64_t id) const {
    SSTable table;
    table.id = id;
    table.path = sstable_path(id);

    table.fd = ::open(table.path.c_str(), O_RDWR);
    if (table.fd < 0) {
        fail("open");
    }

    seek_to_end(table.fd);
    off_t end = current_offset(table.fd);
    if (end < static_cast<off_t>(SST_FOOTER_SIZE)) {
        throw std::runtime_error("open_sstable failed: truncated footer");
    }

    seek_to(table.fd, end - static_cast<off_t>(SST_FOOTER_SIZE));

    uint64_t bloom_offset = 0;
    uint64_t index_offset = 0;
    uint64_t data_end = 0;
    uint32_t record_count = 0;
    uint32_t magic = 0;
    if (!try_read_u64(table.fd, bloom_offset) ||
        !try_read_u64(table.fd, index_offset) ||
        !try_read_u64(table.fd, data_end) ||
        !try_read_u32(table.fd, record_count) ||
        !try_read_u32(table.fd, magic) ||
        magic != SST_MAGIC) {
        throw std::runtime_error("open_sstable failed: bad footer");
    }

    table.data_end = static_cast<off_t>(data_end);
    (void)record_count;

    seek_to(table.fd, static_cast<off_t>(bloom_offset));
    uint32_t bit_bytes = 0;
    if (!try_read_u32(table.fd, table.bloom_hash_count) ||
        !try_read_u32(table.fd, bit_bytes)) {
        throw std::runtime_error("open_sstable failed: truncated bloom");
    }
    table.bloom_bits.resize(bit_bytes);
    if (bit_bytes > 0 && !try_read_bytes(table.fd, table.bloom_bits.data(), bit_bytes)) {
        throw std::runtime_error("open_sstable failed: truncated bloom");
    }

    seek_to(table.fd, static_cast<off_t>(index_offset));
    uint32_t index_count = 0;
    if (!try_read_u32(table.fd, index_count)) {
        throw std::runtime_error("open_sstable failed: truncated index");
    }
    table.sparse_index.reserve(index_count);
    for (uint32_t i = 0; i < index_count; i++) {
        uint32_t key_size = 0;
        uint64_t offset = 0;
        if (!try_read_u32(table.fd, key_size) || !try_read_u64(table.fd, offset)) {
            throw std::runtime_error("open_sstable failed: truncated index");
        }
        std::string key;
        key.resize(key_size);
        if (key_size > 0 && !try_read_bytes(table.fd, key.data(), key_size)) {
            throw std::runtime_error("open_sstable failed: truncated index");
        }
        table.sparse_index.push_back({std::move(key), static_cast<off_t>(offset)});
    }

    return table;
}

void LsmEngine::close_sstable(SSTable& table) const {
    if (table.fd < 0) {
        return;
    }
    ::close(table.fd);
    table.fd = -1;
}

std::vector<LsmEngine::Record> LsmEngine::read_sstable_records(const SSTable& table) const {
    std::vector<Record> records;
    seek_to(table.fd, 0);

    while (current_offset(table.fd) < table.data_end) {
        Record rec;
        if (!try_read_record(table.fd, rec.key, rec.value, rec.deleted)) {
            break;
        }
        records.push_back(std::move(rec));
    }
    return records;
}

std::optional<LsmEngine::Record> LsmEngine::find_in_sstable(
    const SSTable& table, const std::string& key) const {
    if (!bloom_maybe_contains(table.bloom_bits, table.bloom_hash_count, key)) {
        return std::nullopt;
    }

    seek_to(table.fd, sparse_seek_offset(table.sparse_index, key));
    while (current_offset(table.fd) < table.data_end) {
        Record rec;
        if (!try_read_record(table.fd, rec.key, rec.value, rec.deleted)) {
            break;
        }
        if (rec.key == key) {
            return rec;
        }
        if (rec.key > key) {
            break;
        }
    }
    return std::nullopt;
}

void LsmEngine::maybe_compact() {
    if (tables.size() >= COMPACTION_TRIGGER) {
        compact_tables();
    }
}

void LsmEngine::compact_tables() {
    if (tables.empty()) {
        return;
    }

    std::vector<Record> merged = merge_tables();

    std::vector<SSTable> old_tables = std::move(tables);
    tables.clear();

    if (!merged.empty()) {
        uint64_t id = next_sst_id++;
        tables.push_back(create_sstable(id, merged));
    }

    persist_manifest();

    for (SSTable& table : old_tables) {
        close_sstable(table);
        if (::unlink(table.path.c_str()) < 0 && errno != ENOENT) {
            fail("unlink");
        }
    }
    fsync_directory(dir);
}

std::vector<LsmEngine::Record> LsmEngine::merge_tables() const {
    std::vector<std::vector<Record>> runs;
    runs.reserve(tables.size());
    for (const SSTable& table : tables) {
        runs.push_back(read_sstable_records(table));
    }

    std::vector<size_t> pos(runs.size(), 0);
    std::vector<Record> merged;

    for (;;) {
        int min_run = -1;
        for (size_t i = 0; i < runs.size(); i++) {
            if (pos[i] >= runs[i].size()) {
                continue;
            }
            if (min_run < 0 || runs[i][pos[i]].key < runs[min_run][pos[min_run]].key) {
                min_run = static_cast<int>(i);
            }
        }
        if (min_run < 0) {
            break;
        }

        const std::string key = runs[static_cast<size_t>(min_run)][pos[static_cast<size_t>(min_run)]].key;
        Record winner;
        bool found = false;

        for (int i = static_cast<int>(runs.size()) - 1; i >= 0; i--) {
            size_t run = static_cast<size_t>(i);
            if (pos[run] < runs[run].size() && runs[run][pos[run]].key == key) {
                if (!found) {
                    winner = runs[run][pos[run]];
                    found = true;
                }
                pos[run]++;
            }
        }

        if (found && !winner.deleted) {
            merged.push_back(std::move(winner));
        }
    }

    return merged;
}
